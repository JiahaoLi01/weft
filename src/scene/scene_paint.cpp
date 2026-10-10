#include "scene/scene_paint.hpp"
#include "style/style.hpp"
#include "base/css_scalar.hpp"
#include "constants/scene.hpp"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <stdexcept>
#include <numbers>
#include <sstream>

namespace weft {
    namespace {
        using css::trim;

        // Percentages resolve against the reference box (as hundredths), em against the font size.
        PaintNumber number(std::string_view input) {
            const auto s = trim(input);
            const auto [digits, unit] = css::split_unit(s, {.px = true, .percent = true, .em = true});
            const double reference = unit == css::Unit::Percent ? .01 : unit == css::Unit::Em ? 1 : 0;
            const auto n = css::parse_number<double>(std::string(digits), constants::scene::max_numeric_magnitude);
            if (!n) {
                throw std::invalid_argument("paint number");
            }
            return {reference ? 0 : *n, reference ? *n * reference : 0};
        }
    } // namespace

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
                out.kind = PaintValue::Scalar;
                out.scalar = number(s);
                return out;
            }
            out.kind = PaintValue::Transform;
            std::size_t pos{};
            while (pos < s.size()) {
                while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) {
                    ++pos;
                }
                if (pos == s.size()) {
                    break;
                }
                const auto b = s.find('(', pos);
                const auto e = s.find(')', b);
                if (b == s.npos || e == s.npos) {
                    return {};
                }
                const auto name = trim(s.substr(pos, b - pos));
                PaintOperation op;
                static const std::map<std::string, PaintOp> kinds{
                    {"translate", PaintOp::Translate},     {"translateX", PaintOp::TranslateX},
                    {"translateY", PaintOp::TranslateY},   {"scale", PaintOp::Scale},
                    {"scaleX", PaintOp::ScaleX},           {"scaleY", PaintOp::ScaleY},
                    {"rotate", PaintOp::Rotate},           {"matrix", PaintOp::Matrix},
                    {"perspective", PaintOp::Perspective}, {"rotateY", PaintOp::RotateY}};
                auto k = kinds.find(name);
                if (k == kinds.end()) {
                    return {};
                }
                op.kind = k->second;
                auto args = s.substr(b + 1, e - b - 1);
                std::replace(args.begin(), args.end(), ',', ' ');
                std::istringstream stream(args);
                std::string arg;
                while (stream >> arg) {
                    if (op.count >= 6) {
                        return {};
                    }
                    const bool angular = op.kind == PaintOp::Rotate || op.kind == PaintOp::RotateY;
                    if (angular && op.count == 0) {
                        const bool degrees = arg.ends_with("deg");
                        if (degrees) {
                            arg.resize(arg.size() - 3);
                        }
                        auto value = number(arg);
                        const auto factor = degrees ? 1.
                                                    : double(static_cast<float>(constants::degrees_per_half_turn) /
                                                             std::numbers::pi_v<float>);
                        value.absolute *= factor;
                        value.relative *= factor;
                        op.args[op.count++] = value;
                    } else {
                        op.args[op.count++] = number(arg);
                    }
                }
                if (!op.count || (op.kind == PaintOp::Matrix && op.count != 6) ||
                    (op.kind == PaintOp::Rotate && op.count != 1) ||
                    ((op.kind == PaintOp::Translate || op.kind == PaintOp::Scale)
                         ? op.count > 2
                         : (op.kind != PaintOp::Matrix && op.count != 1))) {
                    return {};
                }
                if (op.kind == PaintOp::Perspective && !trim(args).ends_with("px")) {
                    return {};
                }
                if (out.operations.size() >= constants::scene::max_paint_transform_operations) {
                    return {};
                }
                out.operations.push_back(op);
                pos = e + 1;
            }
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

    ScenePaintValues compile_scene_paint_values(const StyleValues& style) {
        ScenePaintValues out;
        static const auto keys = [] {
            std::array<PropertyId, constants::scene::paint_property_count> ids{};
            for (std::size_t i = 0; i < ids.size(); ++i) {
                ids[i] = *find_property(scene_paint_properties()[i]);
            }
            return ids;
        }();
        for (unsigned i = 0; i < keys.size(); ++i) {
            const auto* value = style.find(keys[i]);
            const auto fallback = i == constants::scene::paint_slot_opacity            ? "1"
                                  : i == constants::scene::paint_slot_transform        ? "none"
                                  : i == constants::scene::paint_slot_background_color ? "transparent"
                                  : i == constants::scene::paint_slot_color            ? constants::default_color
                                                                                       : "currentcolor";
            auto s = value ? value->text() : std::string(fallback);
            if (s == "currentcolor" || s == "currentColor") {
                s = std::string(style.text(PropertyId::Color, constants::default_color));
            }
            out.values[i] = parse_scene_paint_value(s);
        }
        return out;
    }
} // namespace weft
