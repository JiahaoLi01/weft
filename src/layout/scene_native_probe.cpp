#include "caseelephant/scene_native_probe.hpp"
#include "caseelephant/base/css_scalar.hpp"
#include "caseelephant/constants/scene.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace weft {
    namespace {
        using Matrix = std::array<float, 6>;
        using Style = SceneStyle;

        using css::trim;

        std::string get(const Style& s, const char* k, const char* d = "") {
            return css::style_value(s, k, d);
        }

        // The message is the fallback reason the native paint compiler reports.
        float number(std::string_view s) {
            const auto n =
                css::parse_number<float>(std::string(s), static_cast<float>(constants::scene::max_numeric_magnitude));
            if (!n) {
                throw std::invalid_argument("number");
            }
            return *n;
        }

        // Unitless numbers are accepted as px; em resolves against ref.
        float len(std::string_view s, float ref = 1) {
            const auto [digits, unit] = css::split_unit(s, {.px = true, .percent = true, .em = true});
            const float n = number(digits);
            if (unit == css::Unit::Percent) {
                return n * ref / constants::percent_scale;
            }
            return unit == css::Unit::Em ? n * ref : n;
        }

        std::vector<std::string> split(std::string_view s, char c = ' ') {
            return css::split(s, c, {.quotes = false}).parts;
        }

        std::array<float, 4> color(std::string s, const Style& style) {
            if (s == "currentcolor" || s == "currentColor") {
                s = get(style, "color", constants::default_color);
            }
            if (s == "transparent" || s == "none") {
                return {};
            }
            return parse_css_color(css::expand_color_shorthand(s, false));
        }

        Matrix multiply(Matrix a, Matrix b) {
            return {a[0] * b[0] + a[2] * b[1], a[1] * b[0] + a[3] * b[1],        a[0] * b[2] + a[2] * b[3],
                    a[1] * b[2] + a[3] * b[3], a[0] * b[4] + a[2] * b[5] + a[4], a[1] * b[4] + a[3] * b[5] + a[5]};
        }

        std::array<float, 2> point(Matrix m, float x, float y) {
            return {m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]};
        }

        Matrix matrix(const SceneNode& n) {
            auto t = n.paint && n.paint->values[constants::scene::paint_slot_transform]
                         ? n.paint->values[constants::scene::paint_slot_transform]
                         : parse_scene_paint_value(get(n.style, "transform", "none"));
            if (!t || t->kind != PaintValue::Transform) {
                throw std::invalid_argument("transform grammar");
            }
            Matrix m{1, 0, 0, 1, 0, 0};
            for (const auto& op : t->operations) {
                auto v = [&](unsigned i, float ref = 1) {
                    return float(op.args[i].absolute + op.args[i].relative * ref);
                };
                Matrix a{1, 0, 0, 1, 0, 0};
                switch (op.kind) {
                    case PaintOp::Translate:
                        a[4] = v(0, n.box[2]);
                        a[5] = op.count > 1 ? v(1, n.box[3]) : 0;
                        break;
                    case PaintOp::TranslateX:
                        a[4] = v(0, n.box[2]);
                        break;
                    case PaintOp::TranslateY:
                        a[5] = v(0, n.box[3]);
                        break;
                    case PaintOp::Scale:
                        a[0] = v(0);
                        a[3] = op.count > 1 ? v(1) : v(0);
                        break;
                    case PaintOp::ScaleX:
                        a[0] = v(0);
                        break;
                    case PaintOp::ScaleY:
                        a[3] = v(0);
                        break;
                    case PaintOp::Rotate: {
                        float r = v(0) * std::numbers::pi_v<float> / constants::degrees_per_half_turn;
                        a = {std::cos(r), std::sin(r), -std::sin(r), std::cos(r), 0, 0};
                        break;
                    }
                    case PaintOp::Matrix:
                        for (unsigned i = 0; i < 6; ++i) {
                            a[i] = v(i);
                        }
                        break;
                    default:
                        throw std::invalid_argument("projective transform");
                }
                m = multiply(m, a);
            }
            auto s = get(n.style, "transform-origin", "50% 50%");
            auto o = split(s);
            float x = n.box[2] / 2;
            float y = n.box[3] / 2;
            if (s == "top") {
                y = 0;
            } else {
                if (!o.empty()) {
                    x = len(o[0], n.box[2]);
                }
                if (o.size() > 1) {
                    y = len(o[1], n.box[3]);
                }
            }
            return multiply(Matrix{1, 0, 0, 1, n.box[0] + x, n.box[1] + y}, multiply(m, Matrix{1, 0, 0, 1, -x, -y}));
        }

        void gradient(NativePaint& p, const Style& s) {
            auto image = get(s, "background-image", "none");
            if (image == "none") {
                return;
            }
            auto b = image.find('(');
            auto e = image.rfind(')');
            if (b == image.npos || e == image.npos) {
                throw std::invalid_argument("gradient grammar");
            }
            auto kind = image.substr(0, b);
            p.gradient = kind == "linear-gradient"   ? constants::scene::gradient_linear
                         : kind == "radial-gradient" ? constants::scene::gradient_radial
                         : kind == "conic-gradient"  ? constants::scene::gradient_conic
                                                     : constants::scene::gradient_none;
            if (!p.gradient) {
                throw std::invalid_argument("background image");
            }
            auto a = split(image.substr(b + 1, e - b - 1), ',');
            if (a.empty()) {
                throw std::invalid_argument("gradient args");
            }
            p.angle = p.gradient == constants::scene::gradient_linear
                          ? static_cast<float>(constants::scene::default_linear_gradient_angle)
                          : 0.f;
            if (p.gradient == constants::scene::gradient_linear && a[0].ends_with("deg")) {
                p.angle = number(a[0].substr(0, a[0].size() - 3));
                a.erase(a.begin());
            }
            if (p.gradient == constants::scene::gradient_radial && a[0] == "circle") {
                a.erase(a.begin());
            }
            if (p.gradient == constants::scene::gradient_conic && a[0].starts_with("from ")) {
                p.angle = number(a[0].substr(5, a[0].size() - 8));
                a.erase(a.begin());
            }
            std::vector<bool> specified;
            for (auto text : a) {
                auto v = split(text);
                auto c = color(v.at(0), s);
                if (v.size() == 1) {
                    p.stops.push_back({0, c});
                    specified.push_back(false);
                } else {
                    for (std::size_t i = 1; i < v.size(); ++i) {
                        if (v[i].ends_with("px")) {
                            throw std::invalid_argument("absolute gradient stop reference");
                        }
                        p.stops.push_back({len(v[i]), c});
                        specified.push_back(true);
                    }
                }
            }
            if (p.stops.size() < 2 || p.stops.size() > constants::scene::max_gradient_stops) {
                throw std::invalid_argument("gradient stop bound");
            }
            if (!specified[0]) {
                p.stops[0].offset = 0;
            }
            if (!specified.back()) {
                p.stops.back().offset = 1;
            }
            specified[0] = specified.back() = true;
            float last = p.stops[0].offset;
            for (std::size_t i = 0; i < p.stops.size(); ++i) {
                if (specified[i]) {
                    p.stops[i].offset = std::max(last, p.stops[i].offset);
                    last = p.stops[i].offset;
                }
            }
            for (std::size_t i = 1; i < p.stops.size() - 1;) {
                if (specified[i]) {
                    ++i;
                    continue;
                }
                auto j = i;
                while (!specified[j]) {
                    ++j;
                }
                for (auto k = i; k < j; ++k) {
                    p.stops[k].offset = p.stops[i - 1].offset + (p.stops[j].offset - p.stops[i - 1].offset) *
                                                                    float(k - i + 1) / float(j - i + 1);
                }
                i = j;
            }
            p.tile_width = p.width;
            p.tile_height = p.height;
            auto size = split(get(s, "background-size", "100% 100%"));
            if (size.size() == 2) {
                p.tile_width = len(size[0], p.width);
                p.tile_height = len(size[1], p.height);
            }
            if (p.tile_width <= 0 || p.tile_height <= 0) {
                throw std::invalid_argument("gradient tile");
            }
            auto pos = split(get(s, "background-position", "0% 0%"));
            if (!pos.empty()) {
                p.offset_x = len(pos[0], p.width - p.tile_width);
            }
            if (pos.size() > 1) {
                p.offset_y = len(pos[1], p.height - p.tile_height);
            }
        }
    } // namespace

    NativeScene compile_native_scene(const SceneFrame& f, const std::vector<NodeIdentity>& layers) {
        NativeScene out;
        try {
            unsigned groups{};
            auto visit = [&](auto&& self, const SceneNode& n, Matrix parent, std::array<float, 4> clip) -> void {
                if (get(n.style, "display") == "none" || n.box[2] <= 0 || n.box[3] <= 0) {
                    return;
                }
                if (out.paints.size() > constants::scene::max_native_paint_commands) {
                    throw std::invalid_argument("native command bound");
                }
                if ((!n.space.empty() && n.space != constants::xhtml_namespace_uri)) {
                    throw std::invalid_argument("SVG reference");
                }
                if (!get(n.style, "box-shadow").empty() && get(n.style, "box-shadow") != "none") {
                    throw std::invalid_argument("shadow reference");
                }
                NativePaint p;
                p.matrix = multiply(parent, matrix(n));
                p.clip = clip;
                p.width = n.box[2];
                p.height = n.box[3];
                float determinant = p.matrix[0] * p.matrix[3] - p.matrix[1] * p.matrix[2];
                if (std::abs(determinant) < constants::scene::singular_determinant_epsilon) {
                    return;
                }
                if (std::hypot(p.matrix[0], p.matrix[1]) + std::hypot(p.matrix[2], p.matrix[3]) >
                    constants::scene::max_geometry_transform_scale) {
                    throw std::invalid_argument("transform scale bound");
                }
                unsigned k{};
                for (const auto* corner : {"top-left", "top-right", "bottom-right", "bottom-left"}) {
                    const auto key = std::string("border-") + corner + "-radius";
                    p.radii[k++] = std::max(0.f, len(get(n.style, key.c_str(), "0"), std::min(p.width, p.height)));
                }
                float factor = 1;
                for (auto q : std::array<std::array<float, 2>, 4>{{{p.width, p.radii[0] + p.radii[1]},
                                                                   {p.width, p.radii[2] + p.radii[3]},
                                                                   {p.height, p.radii[0] + p.radii[3]},
                                                                   {p.height, p.radii[1] + p.radii[2]}}}) {
                    if (q[1] > 0) {
                        factor = std::min(factor, q[0] / q[1]);
                    }
                }
                for (auto& r : p.radii) {
                    r *= factor;
                }
                auto paint_color = [&](unsigned i, const char* key, const char* def) {
                    if (n.paint && n.paint->values[i] && n.paint->values[i]->kind == PaintValue::Color) {
                        std::array<float, 4> c{};
                        for (unsigned j = 0; j < 4; ++j) {
                            c[j] = float(n.paint->values[i]->color[j]);
                        }
                        return c;
                    }
                    return color(get(n.style, key, def), n.style);
                };
                p.background =
                    paint_color(constants::scene::paint_slot_background_color, "background-color", "transparent");
                gradient(p, n.style);
                k = 0;
                for (const auto* edge : {"top", "right", "bottom", "left"}) {
                    auto prefix = std::string("border-") + edge;
                    if (get(n.style, (prefix + "-style").c_str(), "solid") != "solid" &&
                        len(get(n.style, (prefix + "-width").c_str(), "0")) > 0) {
                        throw std::invalid_argument("border style");
                    }
                    p.widths[k] = len(get(n.style, (prefix + "-width").c_str(), "0"));
                    p.edges[k] = paint_color(constants::scene::paint_slot_border_color + k, (prefix + "-color").c_str(),
                                             "currentcolor");
                    ++k;
                }
                float opacity = std::clamp(len(get(n.style, "opacity", "1")), 0.f, 1.f);
                if (n.paint && n.paint->values[constants::scene::paint_slot_opacity]) {
                    opacity = std::clamp(float(n.paint->values[constants::scene::paint_slot_opacity]->scalar.absolute +
                                               n.paint->values[constants::scene::paint_slot_opacity]->scalar.relative),
                                         0.f, 1.f);
                }
                if (std::find(layers.begin(), layers.end(), n.identity) != layers.end()) {
                    auto source = n;
                    source.box[0] = source.box[1] = 0;
                    source.style.erase("transform");
                    source.style.erase("opacity");
                    source.attributes.erase("style");
                    source.paint.reset();
                    p.kind = NativePaint::Layer;
                    p.opacity = opacity;
                    p.text = std::move(source);
                    out.paints.push_back(std::move(p));
                    return;
                }
                const bool isolated =
                    opacity < 1 && (!n.children.empty() || (!n.text.empty() && (p.background[3] > 0 || p.gradient ||
                                                                                std::any_of(
                                                                                    p.widths.begin(), p.widths.end(),
                                                                                    [](float w) {
                                                                                        return w > 0;
                                                                                    }))));
                if (!isolated) {
                    p.opacity = opacity;
                }
                if (isolated) {
                    if (++groups > constants::scene::max_opacity_group_depth) {
                        throw std::invalid_argument("opacity depth bound");
                    }
                    NativePaint g;
                    g.kind = NativePaint::BeginGroup;
                    g.opacity = opacity;
                    g.clip = clip;
                    out.paints.push_back(g);
                }
                if (p.background[3] > 0 || p.gradient || std::any_of(p.widths.begin(), p.widths.end(), [](float w) {
                        return w > 0;
                    })) {
                    out.paints.push_back(p);
                }
                if (get(n.style, "overflow") == "hidden" || get(n.style, "overflow") == "auto") {
                    if (std::any_of(p.radii.begin(), p.radii.end(),
                                    [](float r) {
                                        return r > 0;
                                    }) ||
                        std::abs(p.matrix[1]) > constants::scene::axis_aligned_epsilon ||
                        std::abs(p.matrix[2]) > constants::scene::axis_aligned_epsilon) {
                        throw std::invalid_argument("rounded/transformed clip reference");
                    }
                    auto a = point(p.matrix, 0, 0);
                    auto b = point(p.matrix, p.width, p.height);
                    clip = {std::max(clip[0], std::min(a[0], b[0])), std::max(clip[1], std::min(a[1], b[1])),
                            std::min(clip[2], std::max(a[0], b[0])), std::min(clip[3], std::max(a[1], b[1]))};
                }
                if (!n.text.empty()) {
                    auto t = p;
                    t.kind = NativePaint::Text;
                    t.clip = clip;
                    t.text = n;
                    t.text->children.clear();
                    t.text->box[0] = t.text->box[1] = 0;
                    t.text->style.erase("transform");
                    t.text->style.erase("opacity");
                    // Computed source values below are authoritative. Authored style
                    // still contains composited transform/opacity; retaining it in the
                    // image key rerasterizes static Unicode/emoji on every motion tick.
                    t.text->attributes.erase("style");
                    t.text->style["background-color"] = "transparent";
                    t.text->style["background-image"] = "none";
                    for (const auto* edge : {"top", "right", "bottom", "left"}) {
                        t.text->style[std::string("border-") + edge + "-color"] = "transparent";
                    }
                    t.text->paint.reset();
                    t.text_mask = std::all_of(n.text.begin(), n.text.end(), [](unsigned char c) {
                        return c >= constants::scene::printable_ascii_first &&
                               c < constants::scene::printable_ascii_end;
                    });
                    if (t.text_mask) {
                        t.background =
                            paint_color(constants::scene::paint_slot_color, "color", constants::default_color);
                        t.text->style["color"] = "#ffffff";
                    } else if (n.paint) {
                        t.text->paint.emplace();
                        t.text->paint->values[constants::scene::paint_slot_color] =
                            n.paint->values[constants::scene::paint_slot_color];
                    }
                    out.paints.push_back(std::move(t));
                }
                std::vector<std::size_t> order;
                for (std::size_t i = 0; i < n.children.size(); ++i) {
                    order.push_back(i);
                }
                std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
                    auto z = [&](auto i) {
                        auto v = get(n.children[i].style, "z-index", "0");
                        return v == "auto" ? 0 : number(v);
                    };
                    return z(a) < z(b);
                });
                for (auto i : order) {
                    self(self, n.children[i], p.matrix, clip);
                }
                if (isolated) {
                    NativePaint g;
                    g.kind = NativePaint::EndGroup;
                    g.opacity = opacity;
                    g.clip = p.clip;
                    out.paints.push_back(g);
                    --groups;
                }
            };
            visit(visit, f.root, {1, 0, 0, 1, 0, 0}, {0, 0, f.root.box[2], f.root.box[3]});
            if (out.paints.size() > constants::scene::max_native_paint_commands) {
                throw std::invalid_argument("native command bound");
            }
        } catch (const std::invalid_argument& e) {
            out.paints.clear();
            out.fallback = e.what();
        } catch (const std::out_of_range& e) {
            out.paints.clear();
            out.fallback = e.what();
        }
        return out;
    }
} // namespace weft
