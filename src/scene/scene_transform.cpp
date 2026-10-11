#include "scene/scene_transform.hpp"
#include "scene/scene_values.hpp"

#include "constants/common.hpp"
#include "constants/scene.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace weft {
    namespace {
        constexpr double pi = std::numbers::pi;

        using scene_values::fail;
        using scene_values::get;
        using scene_values::len;
    } // namespace

    SceneAffine SceneAffine::rotation(double degrees, double cx, double cy) {
        const double a = degrees * pi / constants::degrees_per_half_turn;
        const double c = std::cos(a);
        const double s = std::sin(a);
        return {c, s, -s, c, cx - cx * c + cy * s, cy - cx * s - cy * c};
    }

    ScenePlane ScenePlane::operator*(const ScenePlane& b) const {
        ScenePlane out;
        out.m.fill(0);
        for (unsigned y = 0; y < 3; ++y) {
            for (unsigned x = 0; x < 3; ++x) {
                for (unsigned k = 0; k < 3; ++k) {
                    out.m[y * 3 + x] += m[y * 3 + k] * b.m[k * 3 + x];
                }
            }
        }
        return out;
    }

    std::array<double, 2> ScenePlane::point(double x, double y) const {
        const double z = m[6] * x + m[7] * y + m[8];
        if (std::abs(z) < constants::scene::plane_singular_epsilon) {
            return {constants::scene::plane_far_point, constants::scene::plane_far_point};
        }
        return {(m[0] * x + m[1] * y + m[2]) / z, (m[3] * x + m[4] * y + m[5]) / z};
    }

    bool ScenePlane::inverse(ScenePlane& out) const {
        const auto& a = m;
        const double det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) +
                           a[2] * (a[3] * a[7] - a[4] * a[6]);
        if (std::abs(det) < constants::scene::plane_singular_epsilon) {
            return false;
        }
        out.m = {
            (a[4] * a[8] - a[5] * a[7]) / det, (a[2] * a[7] - a[1] * a[8]) / det, (a[1] * a[5] - a[2] * a[4]) / det,
            (a[5] * a[6] - a[3] * a[8]) / det, (a[0] * a[8] - a[2] * a[6]) / det, (a[2] * a[3] - a[0] * a[5]) / det,
            (a[3] * a[7] - a[4] * a[6]) / det, (a[1] * a[6] - a[0] * a[7]) / det, (a[0] * a[4] - a[1] * a[3]) / det};
        return true;
    }

    SceneAffine scene_affine(std::span<const PaintOperation> operations, float w, float h) {
        SceneAffine m;
        for (const auto& op : operations) {
            // A length: absolute px plus a fraction of the reference (translate percentages use the box).
            const auto v = [&](unsigned i, double reference = 1) {
                return i < op.count ? op.args[i].absolute + op.args[i].relative * reference : 0.;
            };
            SceneAffine local;
            switch (op.kind) {
                case PaintOp::Translate:
                    local = SceneAffine::translation(v(0, w), v(1, h));
                    break;
                case PaintOp::TranslateX:
                    local = SceneAffine::translation(v(0, w), 0);
                    break;
                case PaintOp::TranslateY:
                    local = SceneAffine::translation(0, v(0, h));
                    break;
                case PaintOp::Scale:
                    local = SceneAffine::scale(v(0), op.count > 1 ? v(1) : v(0));
                    break;
                case PaintOp::ScaleX:
                    local = SceneAffine::scale(v(0), 1);
                    break;
                case PaintOp::ScaleY:
                    local = SceneAffine::scale(1, v(0));
                    break;
                case PaintOp::Rotate:
                    local = SceneAffine::rotation(v(0), v(1), v(2));
                    break;
                case PaintOp::Matrix:
                    local = {v(0), v(1), v(2), v(3), v(4), v(5)};
                    break;
                case PaintOp::Perspective:
                case PaintOp::RotateY:
                    fail("projective transform requires layer");
            }
            m = local * m;
        }
        return m;
    }

    std::optional<ScenePerspective> scene_perspective(std::span<const PaintOperation> operations) {
        if (std::none_of(operations.begin(), operations.end(), [](const auto& op) {
                return op.kind == PaintOp::Perspective || op.kind == PaintOp::RotateY;
            })) {
            return std::nullopt;
        }
        if (operations.size() != 2 || operations[0].kind != PaintOp::Perspective ||
            operations[1].kind != PaintOp::RotateY || operations[0].args[0].relative != 0 ||
            operations[1].args[0].relative != 0) {
            fail("3D transform outside perspective rotateY plane subset");
        }
        const double distance = operations[0].args[0].absolute;
        if (distance <= 0) {
            fail("perspective distance");
        }
        return ScenePerspective{distance, operations[1].args[0].absolute * pi / constants::degrees_per_half_turn};
    }

    std::array<float, 2> scene_transform_origin(const SceneStyle& style, float w, float h) {
        const auto v = get(style, PropertyId::TransformOrigin, "50% 50%");
        if (v == "top") {
            return {w / 2, 0};
        }
        const auto a = css::split(v, ' ', {.quotes = false}).parts;
        return {a.empty() ? w / 2 : len(a[0], w), a.size() < 2 ? h / 2 : len(a[1], h)};
    }

    ScenePlane scene_plane(const SceneNode& n) {
        const auto o = scene_transform_origin(n.style, n.box[2], n.box[3]);
        const auto operations = parse_scene_transform(n.style.text(PropertyId::Transform, "none")).value();
        ScenePlane local;
        if (const auto p = scene_perspective(operations)) {
            local.m = {std::cos(p->radians), 0, 0, 0, 1, 0, std::sin(p->radians) / p->distance, 0, 1};
        } else {
            local = ScenePlane::affine(scene_affine(operations, n.box[2], n.box[3]));
        }
        return ScenePlane::translation(o[0] + n.box[0], o[1] + n.box[1]) * local *
               ScenePlane::translation(-o[0], -o[1]);
    }

    SceneAffine scene_node_affine(const SceneNode& n) {
        return scene_affine(parse_scene_transform(n.style.text(PropertyId::Transform, "none")).value(), n.box[2],
                            n.box[3]);
    }
} // namespace weft
