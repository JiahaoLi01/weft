#include "scene/scene_text_image.hpp"

#include "constants/text.hpp"
#include "scene/scene_values.hpp"
#include "style/style.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>

namespace weft {
    using scene_values::get;
    using scene_values::len;

    std::array<float, 2> scene_text_origin(const SceneNode& n, const SceneTextMetrics& m) {
        float x{};
        float y{};
        const auto align = get(n.style, PropertyId::TextAlign, "left");
        if (align == "center") {
            x = (n.box[2] - m.width) / 2;
        } else if (align == "right") {
            x = n.box[2] - m.width;
        }
        if (get(n.style, PropertyId::Display) == "flex") {
            if (get(n.style, PropertyId::JustifyContent) == "center") {
                x = (n.box[2] - m.width) / 2;
            }
            if (get(n.style, PropertyId::AlignItems) == "center") {
                y = (n.box[3] - m.height) / 2;
            }
        }
        x += len(get(n.style, PropertyId::PaddingLeft, "0")) + len(get(n.style, PropertyId::BorderLeftWidth, "0"));
        y += len(get(n.style, PropertyId::PaddingTop, "0")) + len(get(n.style, PropertyId::BorderTopWidth, "0"));
        return {x, y};
    }

    SceneTextPlace scene_text_place(std::array<float, 2> corner, float baseline) {
        constexpr float steps = constants::text::text_phase_steps;
        float x = std::floor(corner[0]);
        float step = std::floor((corner[0] - x) * steps + .5f);
        if (step == steps) {
            x += 1;
            step = 0;
        }
        const float y = std::floor(corner[1] + baseline + .5f) - std::floor(baseline + .5f);
        return {{x, y}, step / steps};
    }

    float scene_text_baseline(SceneText& text, const SceneNode& n, float scale, float margin) {
        const auto run =
            n.text_run ? n.text_run : text.shape(n.text, n.style, ShapeContext{n.text_context[0], n.text_context[1]});
        return (margin + scene_text_origin(n, run->metrics)[1] + run->metrics.baseline) * scale;
    }

    SceneTextImage scene_text_image(SceneText& text, const SceneNode& n, float scale, float margin,
                                    std::optional<float> phase) {
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
        auto value = css::trim(get(n.style, PropertyId::Color, "#000000"));
        if (value == "currentColor" || value == "currentcolor") {
            value = constants::default_color;
        }
        std::array<float, 4> color{};
        if (value != "none" && value != "transparent") {
            color = parse_css_color(css::expand_color_shorthand(value));
        }
        const auto run = n.text_run ? text.adopted(n.text_run)
                                    : text.shape(n.text, n.style, ShapeContext{n.text_context[0], n.text_context[1]});
        const auto origin = scene_text_origin(n, run->metrics);
        std::vector<float> coverage(std::size_t(w) * h);
        // The rows and columns the glyphs reach: the pixels past them stay transparent.
        int ink_top = int(h);
        int ink_bottom = -1;
        int ink_left = int(w);
        int ink_right = -1;
        // Premultiplied colors of the color glyphs, each source over the ones before; empty without any.
        std::vector<std::array<float, 4>> painted;
        for (const auto& g : run->glyphs) {
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
            float pen_x = (margin + origin[0] + g.x) * scale;
            float pen_y = (margin + origin[1] + run->metrics.baseline + g.y) * scale;
            if (phase) {
                constexpr float positions = constants::text::glyph_subpixel_positions;
                pen_x = std::floor((pen_x + *phase) * positions + .5f) / positions;
                pen_y = std::floor(pen_y + .5f);
            }
            const auto [ix, fx] = split(pen_x);
            const auto [iy, fy] = split(pen_y);
            if (text.color_face(g.face)) {
                const auto glyph =
                    text.face(g.face).rasterize_color(g.glyph_id, run->size * scale, fx, fy, color,
                                                      g.oblique ? constants::text::synthetic_oblique_skew : 0);
                if (!glyph) {
                    out.color_glyphs = true;
                    continue;
                }
                if (painted.empty()) {
                    painted.resize(coverage.size());
                }
                const int left = int(ix) + glyph->left;
                const int top = int(iy) - glyph->top;
                for (std::uint32_t row = 0; row < glyph->height; ++row) {
                    const int y = top + int(row);
                    for (std::uint32_t column = 0; y >= 0 && y < int(h) && column < glyph->width; ++column) {
                        const int x = left + int(column);
                        if (x < 0 || x >= int(w)) {
                            continue;
                        }
                        const auto* s = glyph->rgba.data() + (std::size_t(row) * glyph->width + column) * 4;
                        auto& d = painted[std::size_t(y) * w + x];
                        const float keep = 1 - s[3] / float(constants::color_channel_max);
                        for (int c = 0; c < 4; ++c) {
                            d[c] = s[c] / float(constants::color_channel_max) + d[c] * keep;
                        }
                    }
                }
                continue;
            }
            const float skew = g.oblique ? constants::text::synthetic_oblique_skew : 0;
            // Still text's glyphs lie on quarter pixels across and whole rows down: the SceneText keeps their coverage
            // for the next image that draws them (a line whose text changes). Others lie anywhere and are drawn anew.
            std::shared_ptr<const GlyphBitmap> kept;
            GlyphBitmap drawn;
            if (phase) {
                kept = text.glyph(g.face, g.glyph_id, run->size * scale, fx, fy, skew);
            } else {
                drawn = text.face(g.face).rasterize(g.glyph_id, run->size * scale, fx, fy, skew);
            }
            const GlyphBitmap& glyph = kept ? *kept : drawn;
            const int left = int(ix) + glyph.left;
            const int top = int(iy) - glyph.top;
            if (glyph.width && glyph.height) {
                ink_top = std::min(ink_top, std::max(0, top));
                ink_bottom = std::max(ink_bottom, std::min(int(h) - 1, top + int(glyph.height) - 1));
                ink_left = std::min(ink_left, std::max(0, left));
                ink_right = std::max(ink_right, std::min(int(w) - 1, left + int(glyph.width) - 1));
            }
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
        if (!painted.empty()) {
            // Color glyphs over the text's other glyphs, back to straight color.
            for (std::size_t i = 0; i < coverage.size(); ++i) {
                const float alpha = std::clamp(coverage[i] * color[3], 0.f, 1.f);
                auto& d = painted[i];
                const float keep = 1 - d[3];
                for (int c = 0; c < 3; ++c) {
                    d[c] += std::clamp(color[c], 0.f, 1.f) * alpha * keep;
                }
                d[3] += alpha * keep;
                if (d[3] <= 0) {
                    continue;
                }
                auto* p = out.bitmap.rgba.data() + i * 4;
                for (int c = 0; c < 3; ++c) {
                    p[c] = std::uint8_t(std::lround(std::clamp(d[c] / d[3], 0.f, 1.f) * constants::color_channel_max));
                }
                p[3] = std::uint8_t(std::lround(std::clamp(d[3], 0.f, 1.f) * constants::color_channel_max));
            }
            return out;
        }
        std::array<std::uint8_t, 3> rgb{};
        for (int c = 0; c < 3; ++c) {
            rgb[c] = std::uint8_t(std::lround(std::clamp(color[c], 0.f, 1.f) * constants::color_channel_max));
        }
        for (int row = ink_top; row <= ink_bottom; ++row) {
            for (int col = ink_left; col <= ink_right; ++col) {
                const auto i = std::size_t(row) * w + col;
                const float alpha = coverage[i] * color[3];
                if (alpha <= 0) {
                    continue;
                }
                auto* p = out.bitmap.rgba.data() + i * 4;
                p[0] = rgb[0];
                p[1] = rgb[1];
                p[2] = rgb[2];
                p[3] = std::uint8_t(std::lround(std::clamp(alpha, 0.f, 1.f) * constants::color_channel_max));
            }
        }
        return out;
    }
} // namespace weft
