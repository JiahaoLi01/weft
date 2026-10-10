#include "scene/scene_text_image.hpp"

#include "scene/scene_values.hpp"
#include "style/style.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace weft {
    using scene_values::get;
    using scene_values::len;

    std::array<float, 2> scene_text_origin(const SceneNode& n, const SceneTextMetrics& m) {
        float x{};
        float y{};
        const auto align = get(n.style, "text-align", "left");
        if (align == "center") {
            x = (n.box[2] - m.width) / 2;
        } else if (align == "right") {
            x = n.box[2] - m.width;
        }
        if (get(n.style, "display") == "flex") {
            if (get(n.style, "justify-content") == "center") {
                x = (n.box[2] - m.width) / 2;
            }
            if (get(n.style, "align-items") == "center") {
                y = (n.box[3] - m.height) / 2;
            }
        }
        x += len(get(n.style, "padding-left", "0")) + len(get(n.style, "border-left-width", "0"));
        y += len(get(n.style, "padding-top", "0")) + len(get(n.style, "border-top-width", "0"));
        return {x, y};
    }

    SceneTextImage scene_text_image(SceneText& text, const SceneNode& n, float scale, float margin) {
        if (!std::isfinite(scale) || scale <= 0 || scale > constants::scene::max_raster_scale) {
            scene_values::fail("raster scale");
        }
        SceneTextImage out;
        const unsigned w = unsigned(std::ceil((n.box[2] + 2 * margin) * scale));
        const unsigned h = unsigned(std::ceil((n.box[3] + 2 * margin) * scale));
        if (!w || !h || std::uint64_t(w) * h > constants::scene::max_bitmap_pixels) {
            scene_values::fail("scene backing capacity");
        }
        out.bitmap = {w, h, std::vector<std::uint8_t>(std::size_t(w) * h * 4)};
        if (n.text.empty()) {
            return out;
        }
        // The computed color string, as the reference raster resolves it for text images.
        auto value = css::trim(get(n.style, "color", "#000000"));
        if (value == "currentColor" || value == "currentcolor") {
            value = constants::default_color;
        }
        std::array<float, 4> color{};
        if (value != "none" && value != "transparent") {
            color = parse_css_color(css::expand_color_shorthand(value));
        }
        const auto run = text.shape(n.text, n.style);
        const auto origin = scene_text_origin(n, run->metrics);
        std::vector<float> coverage(std::size_t(w) * h);
        for (const auto& g : run->glyphs) {
            if (text.color_face(g.face)) {
                out.color_glyphs = true;
                continue;
            }
            // Whole pixel and fraction of the pen position; just below zero, p - floor(p) rounds up to 1.
            const auto split = [](float p) {
                auto whole = std::floor(p);
                auto fraction = p - whole;
                if (fraction >= 1) {
                    whole += 1;
                    fraction = 0;
                }
                return std::pair{whole, fraction};
            };
            const auto [ix, fx] = split((margin + origin[0] + g.x) * scale);
            const auto [iy, fy] = split((margin + origin[1] + run->metrics.baseline + g.y) * scale);
            const auto glyph = text.face(g.face).rasterize(g.glyph_id, run->size * scale, fx, fy);
            const int left = int(ix) + glyph.left;
            const int top = int(iy) - glyph.top;
            for (std::uint32_t row = 0; row < glyph.height; ++row) {
                const int y = top + int(row);
                if (y < 0 || y >= int(h)) {
                    continue;
                }
                for (std::uint32_t column = 0; column < glyph.width; ++column) {
                    const int x = left + int(column);
                    if (x < 0 || x >= int(w)) {
                        continue;
                    }
                    const float c =
                        glyph.coverage[std::size_t(row) * glyph.width + column] / float(constants::color_channel_max);
                    auto& a = coverage[std::size_t(y) * w + x];
                    a = a + c * (1 - a);
                }
            }
        }
        for (std::size_t i = 0; i < coverage.size(); ++i) {
            const float alpha = coverage[i] * color[3];
            if (alpha <= 0) {
                continue;
            }
            auto* p = out.bitmap.rgba.data() + i * 4;
            for (int c = 0; c < 3; ++c) {
                p[c] = std::uint8_t(std::lround(std::clamp(color[c], 0.f, 1.f) * constants::color_channel_max));
            }
            p[3] = std::uint8_t(std::lround(std::clamp(alpha, 0.f, 1.f) * constants::color_channel_max));
        }
        return out;
    }
} // namespace weft
