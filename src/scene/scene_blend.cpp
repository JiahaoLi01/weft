#include "scene/scene_blend.hpp"

#include "base/css_scalar.hpp"

#include <string>

namespace weft {
    namespace {
        std::string value(const StyleValues& style, PropertyId id, const char* initial) {
            auto v = css::trim(style.text(id, initial));
            return v.empty() ? std::string(initial) : v;
        }

        bool set(const StyleValues& style, PropertyId id) {
            const auto v = value(style, id, "none");
            return v != "none";
        }
    } // namespace

    paint::BlendMode scene_blend_mode(const StyleValues& style) {
        using Mode = paint::BlendMode;
        const auto v = value(style, PropertyId::MixBlendMode, "normal");
        constexpr std::pair<const char*, Mode> modes[]{
            {"multiply", Mode::Multiply},     {"screen", Mode::Screen},         {"overlay", Mode::Overlay},
            {"darken", Mode::Darken},         {"lighten", Mode::Lighten},       {"color-dodge", Mode::ColorDodge},
            {"color-burn", Mode::ColorBurn},  {"hard-light", Mode::HardLight},  {"soft-light", Mode::SoftLight},
            {"difference", Mode::Difference}, {"exclusion", Mode::Exclusion},   {"hue", Mode::Hue},
            {"saturation", Mode::Saturation}, {"color", Mode::Color},           {"luminosity", Mode::Luminosity},
            {"plus-lighter", Mode::PlusLighter}};
        for (const auto& [name, mode] : modes) {
            if (v == name) {
                return mode;
            }
        }
        return Mode::Normal;
    }

    bool scene_stacking_context(const SceneNode& n, const SceneNode* parent) {
        const auto& s = n.style;
        const auto position = value(s, PropertyId::Position, "static");
        if (position == "fixed" || position == "sticky") {
            return true;
        }
        if (value(s, PropertyId::ZIndex, "auto") != "auto") {
            const auto display = parent ? value(parent->style, PropertyId::Display, "block") : std::string();
            if (position != "static" || display.find("flex") != std::string::npos ||
                display.find("grid") != std::string::npos) {
                return true;
            }
        }
        if (const auto opacity = css::parse_number<float>(value(s, PropertyId::Opacity, "1")); opacity && *opacity < 1) {
            return true;
        }
        for (const auto id : {PropertyId::Transform, PropertyId::Translate, PropertyId::Rotate, PropertyId::Scale,
                              PropertyId::Filter, PropertyId::BackdropFilter, PropertyId::ClipPath,
                              PropertyId::WebkitClipPath, PropertyId::MaskImage}) {
            if (set(s, id)) {
                return true;
            }
        }
        if (scene_blend_mode(s) != paint::BlendMode::Normal || value(s, PropertyId::Isolation, "auto") == "isolate") {
            return true;
        }
        const auto will = value(s, PropertyId::WillChange, "auto");
        for (const auto* p : {"transform", "opacity", "filter", "translate", "rotate", "scale", "isolation",
                              "mix-blend-mode", "clip-path", "mask"}) {
            if (will.find(p) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    bool scene_isolates_blending(const SceneNode& n) {
        for (const auto& c : n.children) {
            if (scene_blend_mode(c.style) != paint::BlendMode::Normal) {
                return true;
            }
            if (!scene_stacking_context(c, &n) && scene_isolates_blending(c)) {
                return true;
            }
        }
        return false;
    }
} // namespace weft
