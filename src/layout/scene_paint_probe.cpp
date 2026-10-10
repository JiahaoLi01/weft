#include "caseelephant/scene_paint_probe.hpp"
#include "caseelephant/style_probe.hpp"
#include "caseelephant/constants/scene.hpp"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <stdexcept>
#include <numbers>
#include <sstream>

namespace caseelephant {
    namespace {
        std::string trim(std::string s) {
            auto a = s.find_first_not_of(" \t\r\n");
            return a == s.npos ? "" : s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
        }

        ProbePaintNumber number(std::string s) {
            s = trim(s);
            double reference = 0;
            if (s.ends_with('%')) {
                reference = .01;
                s.pop_back();
            } else if (s.ends_with("em")) {
                reference = 1;
                s.resize(s.size() - 2);
            } else if (s.ends_with("px")) {
                s.resize(s.size() - 2);
            }
            std::size_t end{};
            double n = std::stod(s, &end);
            if (end != s.size() || !std::isfinite(n) || std::abs(n) > constants::scene::max_numeric_magnitude) {
                throw std::invalid_argument("paint number");
            }
            return {reference ? 0 : n, reference ? n * reference : 0};
        }
    } // namespace

    const std::array<const char*, constants::scene::paint_property_count>& scene_paint_properties() {
        static const std::array<const char*, constants::scene::paint_property_count> keys{
            "opacity",          "transform",          "background-color",    "color",
            "border-top-color", "border-right-color", "border-bottom-color", "border-left-color"};
        return keys;
    }

    std::optional<ProbePaintValue> parse_scene_paint_value(const std::string& input) {
        try {
            auto s = trim(input);
            ProbePaintValue out;
            if (s == "transparent" || s == "black" || s == "white" || s.starts_with('#') || s.starts_with("rgb(") ||
                s.starts_with("rgba(") || s.starts_with("hsl(") || s.starts_with("hsla(")) {
                out.kind = ProbePaintValue::Color;
                if (s == "black") {
                    s = "#000000";
                }
                if (s == "white") {
                    s = "#ffffff";
                }
                if (s.size() == 4 && s[0] == '#') {
                    s = std::string("#") + s[1] + s[1] + s[2] + s[2] + s[3] + s[3];
                }
                const auto color = parse_probe_css_color(s);
                for (unsigned i = 0; i < 4; ++i) {
                    out.color[i] = color[i];
                }
                return out;
            }
            if (s == "none") {
                out.kind = ProbePaintValue::Transform;
                return out;
            }
            if (s.find('(') == s.npos) {
                out.kind = ProbePaintValue::Scalar;
                out.scalar = number(s);
                return out;
            }
            out.kind = ProbePaintValue::Transform;
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
                ProbePaintOperation op;
                static const std::map<std::string, ProbePaintOp> kinds{
                    {"translate", ProbePaintOp::Translate},     {"translateX", ProbePaintOp::TranslateX},
                    {"translateY", ProbePaintOp::TranslateY},   {"scale", ProbePaintOp::Scale},
                    {"scaleX", ProbePaintOp::ScaleX},           {"scaleY", ProbePaintOp::ScaleY},
                    {"rotate", ProbePaintOp::Rotate},           {"matrix", ProbePaintOp::Matrix},
                    {"perspective", ProbePaintOp::Perspective}, {"rotateY", ProbePaintOp::RotateY}};
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
                    const bool angular = op.kind == ProbePaintOp::Rotate || op.kind == ProbePaintOp::RotateY;
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
                if (!op.count || (op.kind == ProbePaintOp::Matrix && op.count != 6) ||
                    (op.kind == ProbePaintOp::Rotate && op.count != 1) ||
                    ((op.kind == ProbePaintOp::Translate || op.kind == ProbePaintOp::Scale)
                         ? op.count > 2
                         : (op.kind != ProbePaintOp::Matrix && op.count != 1))) {
                    return {};
                }
                if (op.kind == ProbePaintOp::Perspective && !trim(args).ends_with("px")) {
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

    ProbePaintValue interpolate_scene_paint_value(const ProbePaintValue& a, const ProbePaintValue& b, double t) {
        if (a.kind != b.kind) {
            return t < constants::scene::discrete_interpolation_midpoint ? a : b;
        }
        auto out = a;
        auto mix = [&](ProbePaintNumber x, ProbePaintNumber y) {
            return ProbePaintNumber{x.absolute + (y.absolute - x.absolute) * t,
                                    x.relative + (y.relative - x.relative) * t};
        };
        if (a.kind == ProbePaintValue::Scalar) {
            out.scalar = mix(a.scalar, b.scalar);
        } else if (a.kind == ProbePaintValue::Color) {
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

    ProbeScenePaintValues compile_scene_paint_values(const std::map<std::string, std::string>& style) {
        ProbeScenePaintValues out;
        const auto& keys = scene_paint_properties();
        for (unsigned i = 0; i < keys.size(); ++i) {
            auto it = style.find(keys[i]);
            const auto fallback = i == constants::scene::paint_slot_opacity            ? "1"
                                  : i == constants::scene::paint_slot_transform        ? "none"
                                  : i == constants::scene::paint_slot_background_color ? "transparent"
                                  : i == constants::scene::paint_slot_color            ? constants::default_color
                                                                                       : "currentcolor";
            auto s = it == style.end() ? std::string(fallback) : it->second;
            if (s == "currentcolor" || s == "currentColor") {
                auto color = style.find("color");
                s = color == style.end() ? constants::default_color : color->second;
            }
            out.values[i] = parse_scene_paint_value(s);
        }
        return out;
    }
} // namespace caseelephant
