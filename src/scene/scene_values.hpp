#pragma once

#include "base/css_scalar.hpp"
#include "base/diagnostic.hpp"
#include "constants/common.hpp"
#include "constants/scene.hpp"
#include "dom/node.hpp"
#include "paint/display_list.hpp"
#include "style/style.hpp"
#include "render/rounded_clip.hpp"
#include "style/values.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// Computed style values of the scene renderers (scene_geometry, scene_text_image and the D2D reference raster): one
// parser, so every renderer resolves a length to the same float.
namespace weft::scene_values {
    using Style = StyleValues;

    [[noreturn]] inline void fail(const std::string& s) {
        throw_diagnostic(Diag::SceneRaster, s);
    }

    inline std::string get(const Style& s, const std::string& k, const char* fallback = "") {
        return css::style_value(s, k, fallback);
    }

    inline std::string get(const Style& s, PropertyId k, const char* fallback = "") {
        return std::string(s.text(k, fallback));
    }

    // A computed value without copying it, for read-only walks; valid while the style is unchanged.
    inline std::string_view view(const Style& s, PropertyId k, const char* fallback = "") {
        return s.text(k, fallback);
    }

    // Element attributes stay a string map.
    inline std::string get(const AttributeMap& s, const std::string& k, const char* fallback = "") {
        return css::style_value(s, k, fallback);
    }

    inline float num(const std::string& s) {
        const auto n = css::parse_number<float>(s, static_cast<float>(constants::scene::max_numeric_magnitude));
        if (!n) {
            fail("numeric value " + s);
        }
        return *n;
    }

    // Unitless numbers are accepted as px; em resolves against reference; calc() takes a + b.
    inline float len(std::string_view input, float reference = 1) {
        const auto s = css::trim(input);
        if (s.starts_with("calc(")) {
            const auto plus = s.find(" + ");
            if (plus == s.npos) {
                fail("mixed length calc");
            }
            return len(s.substr(5, plus - 5), reference) + len(s.substr(plus + 3, s.size() - plus - 4), reference);
        }
        const auto [digits, unit] = css::split_unit(s, {.px = true, .percent = true, .em = true});
        const float n = num(std::string(digits));
        if (unit == css::Unit::Percent) {
            return n * reference / constants::percent_scale;
        }
        return n * (unit == css::Unit::Em ? reference : 1);
    }

    // Corner radii of a w x h box as the scene paints them, top-left first and clockwise: circular, percentages of
    // the shorter side, reduced together so that adjacent corners never overlap.
    inline std::array<float, 4> corner_radii(const Style& s, float w, float h) {
        std::array<float, 4> r{};
        unsigned k{};
        for (const auto id : {PropertyId::BorderTopLeftRadius, PropertyId::BorderTopRightRadius,
                              PropertyId::BorderBottomRightRadius, PropertyId::BorderBottomLeftRadius}) {
            r[k++] = std::max(0.f, len(get(s, id, "0"), std::min(w, h)));
        }
        float factor = 1;
        for (auto q : std::array<std::array<float, 2>, 4>{
                 {{w, r[0] + r[1]}, {w, r[2] + r[3]}, {h, r[0] + r[3]}, {h, r[1] + r[2]}}}) {
            if (q[1] > 0) {
                factor = std::min(factor, q[0] / q[1]);
            }
        }
        for (auto& v : r) {
            v *= factor;
        }
        return r;
    }

    // The overflow clip of a w x h box in its own coordinates: its padding box, with the inner corner radii. A
    // corner's horizontal radius loses the adjacent left or right border width and its vertical radius the top or
    // bottom one, so unequal widths give an elliptical corner; a corner with either radius at zero is square
    // (CSS Backgrounds 3, 5.3). Scene borders are solid, so every width is used.
    inline RoundedClip padding_box(const Style& s, float w, float h) {
        std::array<float, 4> b{};
        unsigned k{};
        for (const auto id : {PropertyId::BorderTopWidth, PropertyId::BorderRightWidth, PropertyId::BorderBottomWidth,
                              PropertyId::BorderLeftWidth}) {
            b[k++] = std::max(0.f, len(get(s, id, "0")));
        }
        const auto r = corner_radii(s, w, h);
        const std::array<float, 4> horizontal{b[3], b[1], b[1], b[3]};
        const std::array<float, 4> vertical{b[0], b[0], b[2], b[2]};
        RoundedClip c;
        c.rect = {b[3], b[0], std::max(0.f, w - b[1] - b[3]), std::max(0.f, h - b[0] - b[2])};
        for (unsigned i = 0; i < 4; ++i) {
            c.radius_x[i] = std::max(0.f, r[i] - horizontal[i]);
            c.radius_y[i] = std::max(0.f, r[i] - vertical[i]);
            if (c.radius_x[i] <= 0 || c.radius_y[i] <= 0) {
                c.radius_x[i] = c.radius_y[i] = 0;
            }
        }
        return normalize_rounded(c);
    }

    inline bool is_rounded(const RoundedClip& c) {
        return std::any_of(c.radius_x.begin(), c.radius_x.end(), [](float r) {
            return r > 0;
        });
    }

    // A box's corner radii as specified, top-left first and clockwise: percentages of the shorter side, not yet
    // reduced against each other.
    inline std::array<float, 4> specified_radii(const Style& s, float w, float h) {
        std::array<float, 4> r{};
        unsigned k{};
        for (const auto id : {PropertyId::BorderTopLeftRadius, PropertyId::BorderTopRightRadius,
                              PropertyId::BorderBottomRightRadius, PropertyId::BorderBottomLeftRadius}) {
            r[k++] = len(get(s, id, "0"), std::min(w, h));
        }
        return r;
    }

    // A box-shadow color: transparent for none, transparent and #0000, rgb(r g b / a) with space separated
    // channels, currentcolor as the color property, and otherwise a CSS color.
    inline paint::Color shadow_color(std::string text, const Style& s) {
        text = css::trim(text);
        if (text == "none" || text == "transparent" || text == "#0000") {
            return {};
        }
        if (text.starts_with("rgb(") && text.find('/') != text.npos) {
            auto v = text.substr(4, text.size() - 5);
            std::replace(v.begin(), v.end(), '/', ' ');
            std::istringstream parser(v);
            float r{};
            float g{};
            float b{};
            float a{};
            if (!(parser >> r >> g >> b >> a)) {
                fail("shadow color");
            }
            return {r / constants::color_channel_max, g / constants::color_channel_max,
                    b / constants::color_channel_max, a};
        }
        if (text == "currentColor" || text == "currentcolor") {
            text = get(s, PropertyId::Color, constants::default_color);
        }
        return parse_css_color(css::expand_color_shorthand(text));
    }

    // A w x h box's visible outer shadows in paint order, bottom first (CSS lists the topmost first), each with the
    // box's specified radii. A layer is "x y [blur [spread]] color"; an inset layer, a missing length or color, more
    // than max_box_shadow_layers layers, or a blur or spread past max_box_shadow_extent_px fails. Transparent shadows
    // and shadows whose spread empties the box are left out.
    inline std::vector<paint::Shadow> box_shadows(const Style& s, float w, float h, const std::array<float, 4>& radii) {
        std::vector<paint::Shadow> out;
        const auto value = get(s, PropertyId::BoxShadow, "none");
        if (value.empty() || value == "none") {
            return out;
        }
        const auto layers = css::split(value, ',', {.quotes = false}).parts;
        if (layers.size() > constants::scene::max_box_shadow_layers) {
            fail("shadow layer bound");
        }
        for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
            const auto parts = css::split(*it, ' ', {.quotes = false}).parts;
            if (parts.size() < 3 || parts.size() > 5 || parts[0] == "inset") {
                fail("outer shadow lengths and color required");
            }
            paint::Shadow shadow;
            shadow.color = shadow_color(parts.back(), s);
            if (shadow.color[3] == 0) {
                continue;
            }
            shadow.x = len(parts[0]);
            shadow.y = len(parts[1]);
            shadow.blur = parts.size() > 3 ? len(parts[2]) : 0;
            shadow.spread = parts.size() > 4 ? len(parts[3]) : 0;
            if (shadow.blur < 0 || shadow.blur > constants::scene::max_box_shadow_extent_px ||
                std::abs(shadow.spread) > constants::scene::max_box_shadow_extent_px) {
                fail("shadow blur/spread bound");
            }
            if (w + 2 * shadow.spread <= 0 || h + 2 * shadow.spread <= 0) {
                continue;
            }
            shadow.radii = radii;
            out.push_back(shadow);
        }
        return out;
    }

    // The rounded border box of a w x h box in its own coordinates, when a corner is rounded.
    inline std::optional<RoundedClip> rounded_box(const Style& s, float w, float h) {
        const auto r = corner_radii(s, w, h);
        if (std::all_of(r.begin(), r.end(), [](float v) {
                return v <= 0;
            })) {
            return std::nullopt;
        }
        return RoundedClip{{0, 0, w, h}, r, r};
    }
} // namespace weft::scene_values
