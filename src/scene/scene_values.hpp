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
#include <cctype>
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

    // A background-image of url(...): the URL as written, without the quotes of a string (a backslash in it escapes
    // the next character) or the spaces an unquoted URL's tokens were joined with, or an empty one for an empty or
    // unterminated URL; nothing for any other value.
    inline std::optional<std::string> image_url(std::string_view image) {
        constexpr std::string_view open = "url(";
        if (!image.starts_with(open)) {
            return std::nullopt;
        }
        if (!image.ends_with(')')) {
            return std::string{};
        }
        const auto inner = css::trim(image.substr(open.size(), image.size() - open.size() - 1));
        std::string url;
        if (!inner.empty() && (inner.front() == '"' || inner.front() == '\'')) {
            const char quote = inner.front();
            if (inner.size() < 2 || inner.back() != quote) {
                return std::string{};
            }
            for (std::size_t i = 1; i + 1 < inner.size(); ++i) {
                if (inner[i] == quote) {
                    return std::string{};
                }
                if (inner[i] == '\\' && i + 2 < inner.size()) {
                    ++i;
                }
                url += inner[i];
            }
        } else {
            for (const char ch : inner) {
                if (!std::isspace(static_cast<unsigned char>(ch))) {
                    url += ch;
                }
            }
        }
        return url;
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

    // A text line's decorations (CSS Text Decoration 3 §2), as bands in the line's px from its rect's top left: the
    // underline below the baseline and the overline at the line box's top, under the text; the line-through about the
    // middle of lowercase letters, over it. Their thickness: text-decoration-thickness, auto (a 14th of the font size)
    // at least 1px. Every style draws solid.
    struct TextDecorationBands {
        std::vector<std::array<float, 4>> under;
        std::vector<std::array<float, 4>> over;
    };

    // visibility: hidden or collapse (CSS Display 3 §4): the element paints nothing of its own and takes no hits; its
    // children follow their own (inherited) visibility.
    inline bool invisible(const Style& s) {
        const auto v = s.text(PropertyId::Visibility, "visible");
        return v == "hidden" || v == "collapse";
    }

    // An outline (CSS UI 4 §5) as the scene draws it: a ring of the outline's width whose inner edge lies outline-offset
    // outside the border box (inside for a negative offset), in the box's px; its corners round with the border's
    // (radius + offset + width where the border rounds). Every style but none and auto draws solid; auto draws
    // Chrome's focus ring (outline_rings). white: the ring is white, not outline-color.
    struct OutlineRing {
        std::array<float, 4> rect{};
        float width{};
        std::array<float, 4> radii{};
        bool white{};
    };

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

    // A border side's width as Chrome paints it (CSS Values 4 snaps a border width to device pixels): under one device
    // pixel to one, else down to whole device pixels; scale is device pixels per px. Layout keeps the width it computed.
    inline float snapped_border_width(float width, float scale) {
        if (!(width > 0) || !(scale > 0)) {
            return std::max(0.f, width);
        }
        const float device = width * scale;
        return (device < 1 ? 1.f : std::floor(device + constants::scene::border_snap_epsilon)) / scale;
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

    // The ring of a w x h box's outline (OutlineRing), or none: outline-style none (or hidden), no width, or a negative
    // offset that leaves no box.
    inline std::optional<OutlineRing> outline(const Style& s, float w, float h) {
        const auto style = s.text(PropertyId::OutlineStyle, "none");
        if (style.empty() || style == "none" || style == "hidden") {
            return std::nullopt;
        }
        const auto width_text = s.text(PropertyId::OutlineWidth, "medium");
        const float width = width_text == "thin"     ? 1.f
                            : width_text == "medium" ? 3.f
                            : width_text == "thick"  ? 5.f
                                                     : len(width_text);
        if (!(width > 0)) {
            return std::nullopt;
        }
        const float grow = len(s.text(PropertyId::OutlineOffset, "0")) + width;
        OutlineRing ring;
        ring.rect = {-grow, -grow, w + 2 * grow, h + 2 * grow};
        if (!(ring.rect[2] > 0) || !(ring.rect[3] > 0)) {
            return std::nullopt;
        }
        ring.width = std::min(width, std::min(ring.rect[2], ring.rect[3]) / 2);
        const auto radii = corner_radii(s, w, h);
        for (unsigned k = 0; k < 4; ++k) {
            ring.radii[k] = radii[k] > 0 ? std::max(0.f, radii[k] + grow) : 0.f;
        }
        return ring;
    }

    // The rings a w x h box's outline draws, back to front. outline-style auto is Chrome's focus ring (Blink's
    // OutlinePainter::PaintFocusRing): a 2 px ring of outline-color centred on the border box moved out by
    // outline-offset (in by 1 px when every border is at least 1 px wide), over a 2 px white ring 1 px further out, of
    // which the outer pixel shows; corners round with the border's radii moved out the same, and at least 3 px. Any
    // other style draws outline().
    inline std::vector<OutlineRing> outline_rings(const Style& s, float w, float h) {
        if (s.text(PropertyId::OutlineStyle, "none") != "auto") {
            const auto ring = outline(s, w, h);
            return ring ? std::vector<OutlineRing>{*ring} : std::vector<OutlineRing>{};
        }
        constexpr float stroke = 2;
        constexpr float min_radius = 3;
        const float outline_offset = len(s.text(PropertyId::OutlineOffset, "0"));
        float offset = outline_offset;
        float border = len(s.text(PropertyId::BorderTopWidth, "0"));
        for (const auto id : {PropertyId::BorderRightWidth, PropertyId::BorderBottomWidth, PropertyId::BorderLeftWidth}) {
            border = std::min(border, len(s.text(id, "0")));
        }
        if (border >= 1) {
            offset -= 1;
        }
        const auto radii = corner_radii(s, w, h);
        const bool rounded = radii[0] > 0 || radii[1] > 0 || radii[2] > 0 || radii[3] > 0;
        std::vector<OutlineRing> rings;
        // The centre lines lie `centre` outside the border box; a ring reaches a half stroke beyond it.
        for (const auto [centre, white] : {std::pair{offset + 1, true}, std::pair{offset, false}}) {
            const float grow = centre + stroke / 2;
            OutlineRing ring;
            ring.rect = {-grow, -grow, w + 2 * grow, h + 2 * grow};
            if (!(ring.rect[2] > 0) || !(ring.rect[3] > 0)) {
                continue;
            }
            ring.width = std::min(stroke, std::min(ring.rect[2], ring.rect[3]) / 2);
            // Both rings take the border's radii moved out by outline-offset (Blink's GetFocusRingCornerRadii).
            for (unsigned k = 0; k < 4; ++k) {
                const float r = rounded && radii[k] > 0 ? radii[k] + outline_offset : 0.f;
                ring.radii[k] = std::max(r, min_radius) + stroke / 2;
            }
            ring.white = white;
            rings.push_back(ring);
        }
        return rings;
    }

    // The decoration bands of a text line width px wide whose baseline lies baseline below its top (TextDecorationBands).
    inline TextDecorationBands text_decoration_bands(const Style& s, float width, float baseline) {
        TextDecorationBands out;
        const auto line = s.text(PropertyId::TextDecorationLine, "none");
        if (line.empty() || line == "none" || !(width > 0)) {
            return out;
        }
        const float size = len(s.text(PropertyId::FontSize, "16px"));
        const auto t = s.text(PropertyId::TextDecorationThickness, "auto");
        const float thickness = std::max(1.f, t.empty() || t == "auto" || t == "from-font" ? size / 14 : len(t, size));
        for (const auto& word : css::split(line, ' ').parts) {
            if (word == "underline") {
                out.under.push_back({0, baseline + size * .09f, width, thickness});
            } else if (word == "overline") {
                out.under.push_back({0, 0, width, thickness});
            } else if (word == "line-through") {
                out.over.push_back({0, baseline - size * .3f - thickness / 2, width, thickness});
            }
        }
        return out;
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
