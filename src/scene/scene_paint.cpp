#include "scene/scene_paint.hpp"
#include "style/style.hpp"
#include "base/css_scalar.hpp"
#include "constants/scene.hpp"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <stdexcept>
#include <numbers>

namespace weft {
    namespace {
        using css::trim;

        // Percentages are fractions of the reference length; em, like the geometry lengths, multiplies it.
        std::optional<PaintNumber> paint_number(std::string_view input) {
            const auto s = trim(input);
            const auto [digits, unit] = css::split_unit(s, {.px = true, .percent = true, .em = true});
            const double reference = unit == css::Unit::Percent ? .01 : unit == css::Unit::Em ? 1 : 0;
            const auto n = css::parse_number<double>(std::string(digits), constants::scene::max_numeric_magnitude);
            if (!n) {
                return std::nullopt;
            }
            return PaintNumber{reference ? 0 : *n, reference ? *n * reference : 0};
        }
    } // namespace

    Result<std::vector<PaintOperation>> parse_scene_transform(std::string_view text, bool svg) {
        const auto s = trim(text);
        std::vector<PaintOperation> out;
        if (s.empty() || s == "none") {
            return out;
        }
        // translate3d() and translateZ() without depth are translations of the plane (what framer-motion and pages
        // write to promote a layer): any depth is a 3D transform, outside the profile.
        static const std::map<std::string, PaintOp, std::less<>> kinds{
            {"translate", PaintOp::Translate},     {"translateX", PaintOp::TranslateX},
            {"translateY", PaintOp::TranslateY},   {"translate3d", PaintOp::Translate},
            {"translateZ", PaintOp::Translate},    {"scale", PaintOp::Scale},
            {"scaleX", PaintOp::ScaleX},           {"scaleY", PaintOp::ScaleY},
            {"rotate", PaintOp::Rotate},           {"matrix", PaintOp::Matrix},
            {"perspective", PaintOp::Perspective}, {"rotateY", PaintOp::RotateY}};
        std::size_t pos{};
        while (pos < s.size()) {
            while (pos < s.size() && (std::isspace(static_cast<unsigned char>(s[pos])) || (svg && s[pos] == ','))) {
                ++pos;
            }
            if (pos == s.size()) {
                break;
            }
            const auto b = s.find('(', pos);
            const auto e = b == s.npos ? s.npos : s.find(')', b);
            if (e == s.npos) {
                return make_diagnostic(Diag::SceneRaster, "transform syntax");
            }
            const auto name = trim(s.substr(pos, b - pos));
            const auto kind = kinds.find(name);
            if (kind == kinds.end()) {
                return make_diagnostic(Diag::SceneRaster, "unsupported transform " + name);
            }
            PaintOperation op;
            op.kind = kind->second;
            // The arguments, apart at spaces and commas.
            const std::string_view args = std::string_view(s).substr(b + 1, e - b - 1);
            const auto separator = [](char c) {
                return std::isspace(static_cast<unsigned char>(c)) || c == ',';
            };
            for (std::size_t at = 0;;) {
                while (at < args.size() && separator(args[at])) {
                    ++at;
                }
                if (at == args.size()) {
                    break;
                }
                auto end = at;
                while (end < args.size() && !separator(args[end])) {
                    ++end;
                }
                auto arg = args.substr(at, end - at);
                at = end;
                if (op.count >= op.args.size()) {
                    return make_diagnostic(Diag::SceneRaster, "transform arguments of " + name);
                }
                // The angle of rotate and rotateY: deg, or radians when unitless (SVG: degrees).
                if ((op.kind == PaintOp::Rotate || op.kind == PaintOp::RotateY) && op.count == 0) {
                    const bool degrees = arg.ends_with("deg");
                    if (degrees) {
                        arg.remove_suffix(3);
                    }
                    auto value = paint_number(arg);
                    if (!value) {
                        return make_diagnostic(Diag::SceneRaster, "transform number " + std::string(arg));
                    }
                    const auto factor =
                        degrees || svg
                            ? 1.
                            : double(static_cast<float>(constants::degrees_per_half_turn) / std::numbers::pi_v<float>);
                    value->absolute *= factor;
                    value->relative *= factor;
                    op.args[op.count++] = *value;
                } else if (const auto value = paint_number(arg)) {
                    op.args[op.count++] = *value;
                } else {
                    return make_diagnostic(Diag::SceneRaster, "transform number " + std::string(arg));
                }
            }
            if (name == "translate3d" || name == "translateZ") {
                const auto depth = name == "translateZ" ? 0u : 2u;
                if (op.count != depth + 1) {
                    return make_diagnostic(Diag::SceneRaster, "transform arguments of " + name);
                }
                if (op.args[depth].absolute != 0 || op.args[depth].relative != 0) {
                    return make_diagnostic(Diag::SceneRaster, "3D transform " + name + " with a depth");
                }
                if (name == "translateZ") {
                    op.args[0] = {};
                } else {
                    op.count = 2;
                }
            }
            bool arity{};
            switch (op.kind) {
                case PaintOp::Matrix:
                    arity = op.count == 6;
                    break;
                case PaintOp::Translate:
                case PaintOp::Scale:
                    arity = op.count == 1 || op.count == 2;
                    break;
                case PaintOp::Rotate:
                    arity = op.count == 1 || (svg && op.count == 3);
                    break;
                default:
                    arity = op.count == 1;
            }
            if (!arity) {
                return make_diagnostic(Diag::SceneRaster, "transform arguments of " + name);
            }
            if (op.kind == PaintOp::Perspective && !trim(args).ends_with("px")) {
                return make_diagnostic(Diag::SceneRaster, "perspective needs a px distance");
            }
            if (out.size() >= constants::scene::max_paint_transform_operations) {
                return make_diagnostic(Diag::SceneRaster, "transform list length");
            }
            out.push_back(op);
            pos = e + 1;
        }
        return out;
    }

    const std::array<const char*, constants::scene::paint_property_count>& scene_paint_properties() {
        static const std::array<const char*, constants::scene::paint_property_count> keys{
            "opacity",          "transform",          "background-color",    "color",
            "border-top-color", "border-right-color", "border-bottom-color", "border-left-color"};
        return keys;
    }

    std::optional<PaintValue> parse_scene_paint_value(const std::string& input) {
        try {
            auto s = trim(input);
            PaintValue out;
            if (s == "transparent" || s == "black" || s == "white" || s.starts_with('#') || s.starts_with("rgb(") ||
                s.starts_with("rgba(") || s.starts_with("hsl(") || s.starts_with("hsla(")) {
                out.kind = PaintValue::Color;
                s = css::expand_color_shorthand(s);
                const auto color = parse_css_color(s);
                for (unsigned i = 0; i < 4; ++i) {
                    out.color[i] = color[i];
                }
                return out;
            }
            if (s == "none") {
                out.kind = PaintValue::Transform;
                return out;
            }
            if (s.find('(') == s.npos) {
                const auto scalar = paint_number(s);
                if (!scalar) {
                    return {};
                }
                out.kind = PaintValue::Scalar;
                out.scalar = *scalar;
                return out;
            }
            auto operations = parse_scene_transform(s);
            if (!operations) {
                return {};
            }
            out.kind = PaintValue::Transform;
            out.operations = std::move(*operations);
            return out;
        } catch (const std::exception&) {
            return {};
        }
    }

    PaintValue interpolate_scene_paint_value(const PaintValue& a, const PaintValue& b, double t) {
        if (a.kind != b.kind) {
            return t < constants::scene::discrete_interpolation_midpoint ? a : b;
        }
        auto out = a;
        auto mix = [&](PaintNumber x, PaintNumber y) {
            return PaintNumber{x.absolute + (y.absolute - x.absolute) * t, x.relative + (y.relative - x.relative) * t};
        };
        if (a.kind == PaintValue::Scalar) {
            out.scalar = mix(a.scalar, b.scalar);
        } else if (a.kind == PaintValue::Color) {
            out.color[3] = a.color[3] + (b.color[3] - a.color[3]) * t;
            for (unsigned i = 0; i < 3; ++i) {
                out.color[i] =
                    out.color[3] ? ((1 - t) * a.color[i] * a.color[3] + t * b.color[i] * b.color[3]) / out.color[3] : 0;
            }
        } else {
            if (a.operations.size() != b.operations.size()) {
                return t < constants::scene::discrete_interpolation_midpoint ? a : b;
            }
            for (std::size_t i = 0; i < a.operations.size(); ++i) {
                if (a.operations[i].kind != b.operations[i].kind || a.operations[i].count != b.operations[i].count) {
                    return t < constants::scene::discrete_interpolation_midpoint ? a : b;
                }
                for (unsigned j = 0; j < a.operations[i].count; ++j) {
                    out.operations[i].args[j] = mix(a.operations[i].args[j], b.operations[i].args[j]);
                }
            }
        }
        return out;
    }

    namespace {
        // The text a paint slot reads of a style: its property's value or the slot's initial one, currentcolor
        // taking the color's.
        std::string_view paint_text(const StyleValues& style, unsigned slot) {
            static const auto keys = [] {
                std::array<PropertyId, constants::scene::paint_property_count> ids{};
                for (std::size_t i = 0; i < ids.size(); ++i) {
                    ids[i] = *find_property(scene_paint_properties()[i]);
                }
                return ids;
            }();
            const auto* value = style.find(keys[slot]);
            const auto fallback = slot == constants::scene::paint_slot_opacity            ? "1"
                                  : slot == constants::scene::paint_slot_transform        ? "none"
                                  : slot == constants::scene::paint_slot_background_color ? "transparent"
                                  : slot == constants::scene::paint_slot_color            ? constants::default_color
                                                                                          : "currentcolor";
            const std::string_view s = value ? std::string_view(value->text()) : std::string_view(fallback);
            if (s == "currentcolor" || s == "currentColor") {
                return style.text(PropertyId::Color, constants::default_color);
            }
            return s;
        }
    } // namespace

    ScenePaintValues compile_scene_paint_values(const StyleValues& style) {
        ScenePaintValues out;
        for (unsigned i = 0; i < out.values.size(); ++i) {
            out.values[i] = parse_scene_paint_value(std::string(paint_text(style, i)));
        }
        return out;
    }

    ScenePaintValues compile_scene_paint_values(const StyleValues& style, const StyleValues& before,
                                                const ScenePaintValues& last) {
        ScenePaintValues out;
        for (unsigned i = 0; i < out.values.size(); ++i) {
            const auto text = paint_text(style, i);
            out.values[i] = text == paint_text(before, i) ? last.values[i] : parse_scene_paint_value(std::string(text));
        }
        return out;
    }
} // namespace weft
