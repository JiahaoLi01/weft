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

    // A background-image of -weft-texture(name) (Weft extension, a host texture): its name, or an empty one when the
    // parentheses hold anything but one name of letters, digits, '-' and '_' of at most max_texture_name_bytes;
    // nothing for any other value.
    inline std::optional<std::string> texture_name(std::string_view image) {
        constexpr std::string_view open = "-weft-texture(";
        if (!image.starts_with(open)) {
            return std::nullopt;
        }
        auto name =
            image.ends_with(')') ? css::trim(image.substr(open.size(), image.size() - open.size() - 1)) : std::string{};
        const bool valid = !name.empty() && name.size() <= std::size_t(constants::scene::max_texture_name_bytes) &&
                           std::all_of(name.begin(), name.end(), [](char ch) {
                               return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                                      (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
                           });
        return valid ? std::move(name) : std::string{};
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

    // Unitless numbers are accepted as px; em resolves against reference; calc() takes a + b and a - b.
    inline float len(std::string_view input, float reference = 1) {
        const auto s = css::trim(input);
        if (s.starts_with("calc(")) {
            auto at = s.find(" + ");
            float sign = 1;
            if (at == s.npos) {
                at = s.find(" - ");
                sign = -1;
            }
            if (at == s.npos) {
                fail("mixed length calc");
            }
            return len(s.substr(5, at - 5), reference) + sign * len(s.substr(at + 3, s.size() - at - 4), reference);
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

    // A box-shadow layer and the box it shadows in the element's coordinates: an outer shadow's border box, an inset
    // shadow's padding box.
    struct BoxShadow {
        paint::Shadow shadow;
        std::array<float, 4> rect{};
    };

    // A w x h box's visible shadows in paint order, bottom first (CSS lists the topmost first): outer shadows of its
    // border box with radii, inset ones of its padding box with its inner radii. A layer is "x y [blur [spread]]" with
    // a color before or after the lengths (currentcolor without one) and inset first or last; fewer than two lengths
    // or more than four, more than max_box_shadow_layers layers, a blur or spread past max_box_shadow_extent_px, or an
    // inset shadow inside elliptical inner corners fails. Transparent shadows, outer shadows whose spread empties the
    // box and inset shadows of an empty padding box are left out.
    inline std::vector<BoxShadow> box_shadows(const Style& s, float w, float h, const std::array<float, 4>& radii) {
        std::vector<BoxShadow> out;
        const auto value = get(s, PropertyId::BoxShadow, "none");
        if (value.empty() || value == "none") {
            return out;
        }
        const auto layers = css::split(value, ',', {.quotes = false}).parts;
        if (layers.size() > constants::scene::max_box_shadow_layers) {
            fail("shadow layer bound");
        }
        const auto length = [](const std::string& t) {
            return !t.empty() &&
                   (std::isdigit(static_cast<unsigned char>(t[0])) || t[0] == '.' || t[0] == '-' || t[0] == '+');
        };
        for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
            auto parts = css::split(*it, ' ', {.quotes = false}).parts;
            paint::Shadow shadow;
            if (!parts.empty() && (parts.front() == "inset" || parts.back() == "inset")) {
                shadow.inset = true;
                parts.erase(parts.front() == "inset" ? parts.begin() : parts.end() - 1);
            }
            std::string color = "currentcolor";
            if (!parts.empty() && !length(parts.front())) {
                color = parts.front();
                parts.erase(parts.begin());
            } else if (!parts.empty() && !length(parts.back())) {
                color = parts.back();
                parts.pop_back();
            }
            if (parts.size() < 2 || parts.size() > 4 || !std::all_of(parts.begin(), parts.end(), length)) {
                fail("shadow lengths and color required");
            }
            shadow.color = shadow_color(color, s);
            if (shadow.color[3] == 0) {
                continue;
            }
            shadow.x = len(parts[0]);
            shadow.y = len(parts[1]);
            shadow.blur = parts.size() > 2 ? len(parts[2]) : 0;
            shadow.spread = parts.size() > 3 ? len(parts[3]) : 0;
            if (shadow.blur < 0 || shadow.blur > constants::scene::max_box_shadow_extent_px ||
                std::abs(shadow.spread) > constants::scene::max_box_shadow_extent_px) {
                fail("shadow blur/spread bound");
            }
            if (shadow.inset) {
                const auto padding = padding_box(s, w, h);
                if (padding.rect[2] <= 0 || padding.rect[3] <= 0) {
                    continue;
                }
                for (unsigned i = 0; i < 4; ++i) {
                    if (padding.radius_x[i] != padding.radius_y[i]) {
                        fail("inset shadow inside elliptical corners outside scene profile");
                    }
                }
                shadow.radii = padding.radius_x;
                out.push_back({shadow, padding.rect});
                continue;
            }
            if (w + 2 * shadow.spread <= 0 || h + 2 * shadow.spread <= 0) {
                continue;
            }
            shadow.radii = radii;
            out.push_back({shadow, {0, 0, w, h}});
        }
        return out;
    }

    // A text-shadow layer (CSS Text Decoration 3): its color, offset and blur radius, in px.
    struct TextShadow {
        paint::Color color{};
        float x{};
        float y{};
        float blur{};
    };

    // An element's visible text shadows in paint order, bottom first (CSS lists the topmost first). A layer is "x y
    // [blur]" with a color before or after the lengths (currentcolor without one); fewer than two lengths or more than
    // three, more than max_box_shadow_layers layers, or a negative blur or one past max_box_shadow_extent_px fails.
    // Transparent shadows are left out.
    inline std::vector<TextShadow> text_shadows(const Style& s) {
        std::vector<TextShadow> out;
        const auto value = get(s, PropertyId::TextShadow, "none");
        if (value.empty() || value == "none") {
            return out;
        }
        const auto layers = css::split(value, ',', {.quotes = false}).parts;
        if (layers.size() > constants::scene::max_box_shadow_layers) {
            fail("text shadow layer bound");
        }
        const auto length = [](const std::string& t) {
            return !t.empty() &&
                   (std::isdigit(static_cast<unsigned char>(t[0])) || t[0] == '.' || t[0] == '-' || t[0] == '+');
        };
        for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
            auto parts = css::split(*it, ' ', {.quotes = false}).parts;
            std::string color = "currentcolor";
            if (!parts.empty() && !length(parts.front())) {
                color = parts.front();
                parts.erase(parts.begin());
            } else if (!parts.empty() && !length(parts.back())) {
                color = parts.back();
                parts.pop_back();
            }
            if (parts.size() < 2 || parts.size() > 3 || !std::all_of(parts.begin(), parts.end(), length)) {
                fail("text shadow lengths and color required");
            }
            TextShadow shadow;
            shadow.color = shadow_color(color, s);
            shadow.x = len(parts[0]);
            shadow.y = len(parts[1]);
            shadow.blur = parts.size() > 2 ? len(parts[2]) : 0;
            if (!(shadow.blur >= 0) || shadow.blur > constants::scene::max_box_shadow_extent_px) {
                fail("text shadow blur bound");
            }
            if (shadow.color[3] > 0) {
                out.push_back(shadow);
            }
        }
        return out;
    }

    // A drop shadow (Filter Effects 1, drop-shadow()): its color, offset and the standard deviation of its blur, in px.
    struct DropShadow {
        paint::Color color{};
        float x{};
        float y{};
        float sigma{};
    };

    // The drop shadow an element's filter applies: none for none, or one drop-shadow() of "x y [deviation]" with a
    // color before or after the lengths (currentcolor without one). Another filter function, more than one, fewer than
    // two lengths or more than three, or a negative deviation or one past max_box_shadow_extent_px fails; a transparent
    // shadow is none.
    inline std::optional<DropShadow> drop_shadow(const Style& s) {
        const auto value = css::trim(get(s, PropertyId::Filter, "none"));
        if (value.empty() || value == "none") {
            return std::nullopt;
        }
        const auto functions = css::split(value, ' ', {.quotes = false}).parts;
        if (functions.size() != 1 || !functions[0].starts_with("drop-shadow(") || !functions[0].ends_with(')')) {
            fail("filter " + value + " outside scene profile");
        }
        const auto& f = functions[0];
        auto parts = css::split(std::string_view(f).substr(12, f.size() - 13), ' ', {.quotes = false}).parts;
        const auto length = [](const std::string& t) {
            return !t.empty() &&
                   (std::isdigit(static_cast<unsigned char>(t[0])) || t[0] == '.' || t[0] == '-' || t[0] == '+');
        };
        std::string color = "currentcolor";
        if (!parts.empty() && !length(parts.front())) {
            color = parts.front();
            parts.erase(parts.begin());
        } else if (!parts.empty() && !length(parts.back())) {
            color = parts.back();
            parts.pop_back();
        }
        if (parts.size() < 2 || parts.size() > 3 || !std::all_of(parts.begin(), parts.end(), length)) {
            fail("drop-shadow lengths and color required");
        }
        DropShadow out;
        out.color = shadow_color(color, s);
        out.x = len(parts[0]);
        out.y = len(parts[1]);
        out.sigma = parts.size() > 2 ? len(parts[2]) : 0;
        if (!(out.sigma >= 0) || out.sigma > constants::scene::max_box_shadow_extent_px) {
            fail("drop-shadow deviation bound");
        }
        if (out.color[3] <= 0) {
            return std::nullopt;
        }
        return out;
    }

    // The standard deviation of a shadow's Gaussian for its blur radius, as CSS defines it for box-shadow and
    // text-shadow: half the radius.
    inline float shadow_sigma(float blur) {
        return blur / 2;
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
