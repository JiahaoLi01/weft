#include "constants/text.hpp"
#include "scene/scene_text_image.hpp"
#include "support/check.hpp"
#include "support/scene_fonts.hpp"
#include "test_constants/scene.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

// Portable text images of scene text nodes, and the same glyph runs drawn from SDF fields: the SDF material of
// docs/sdf-font-probe.md reconstructs the FreeType coverage of scene text at every raster scale.
namespace {
    using weft::testing::require;

    weft::SceneNode text_node(std::string text, weft::SceneStyle style, float w, float h) {
        weft::SceneNode n;
        n.tag = "div";
        n.text = std::move(text);
        n.style = std::move(style);
        n.box = {0, 0, w, h};
        return n;
    }

    // Alpha bounding box {left, top, right, bottom} of a straight RGBA image, in pixels.
    std::array<int, 4> ink(const weft::SceneBitmap& b) {
        std::array<int, 4> r{int(b.width), int(b.height), -1, -1};
        for (unsigned y = 0; y < b.height; ++y) {
            for (unsigned x = 0; x < b.width; ++x) {
                if (b.rgba[(std::size_t(y) * b.width + x) * 4 + 3]) {
                    r = {std::min(r[0], int(x)), std::min(r[1], int(y)), std::max(r[2], int(x)),
                         std::max(r[3], int(y))};
                }
            }
        }
        return r;
    }

    void origins() {
        const weft::SceneTextMetrics m{40, 20, 15};
        const auto at = [&](weft::SceneStyle style) {
            return weft::scene_text_origin(text_node("x", std::move(style), 100, 50), m);
        };
        require(at({}) == std::array<float, 2>{0, 0}, "default origin");
        require(at({{"text-align", "center"}}) == std::array<float, 2>{30, 0}, "text-align center");
        require(at({{"text-align", "right"}}) == std::array<float, 2>{60, 0}, "text-align right");
        require(at({{"display", "flex"}, {"justify-content", "center"}, {"align-items", "center"}}) ==
                    std::array<float, 2>{30, 15},
                "flex centering");
        require(at({{"padding-left", "4px"},
                    {"border-left-width", "2px"},
                    {"padding-top", "calc(1px + 2px)"},
                    {"border-top-width", "1"}}) == std::array<float, 2>{6, 4},
                "padding and border offsets");
        // Flex centering is of the border box; the padding still shifts it, as in the reference raster.
        require(at({{"display", "flex"}, {"align-items", "center"}, {"padding-top", "10px"}}) ==
                    std::array<float, 2>{0, 25},
                "flex centering plus padding");
    }

    void images(weft::SceneText& text) {
        const weft::SceneStyle style{{"font-family", "system-ui"}, {"font-size", "20px"}, {"color", "#ff8000"}};
        const auto n = text_node("Hello", style, 80, 30);
        const auto metrics = text.measure(n.text, n.style);
        for (const float scale : {1.f, 2.f}) {
            const auto image = weft::scene_text_image(text, n, scale, 4);
            require(!image.color_glyphs, "Latin text has no color glyphs");
            require(image.bitmap.width == unsigned(std::ceil(88 * scale)) &&
                        image.bitmap.height == unsigned(std::ceil(38 * scale)),
                    "image covers box and margins");
            const auto box = ink(image.bitmap);
            require(box[2] >= 0, "text drawn");
            // Ink stays inside the line box (plus antialiasing) and starts at the margin.
            require(box[0] >= int(std::floor(4 * scale)) && box[2] <= int(std::ceil((4 + metrics.width + 1) * scale)) &&
                        box[1] >= int(std::floor(4 * scale)) && box[3] <= int(std::ceil((4 + metrics.height) * scale)),
                    "ink inside the line box");
            bool colored = true;
            for (std::size_t i = 0; i < image.bitmap.rgba.size(); i += 4) {
                if (image.bitmap.rgba[i + 3]) {
                    colored &=
                        image.bitmap.rgba[i] == 255 && image.bitmap.rgba[i + 1] == 128 && image.bitmap.rgba[i + 2] == 0;
                }
            }
            require(colored, "straight color is the computed color");
        }
        auto right = n;
        right.style["text-align"] = "right";
        const auto shifted = ink(weft::scene_text_image(text, right, 1, 4).bitmap);
        const auto left = ink(weft::scene_text_image(text, n, 1, 4).bitmap);
        require(std::abs((shifted[0] - left[0]) - (80 - metrics.width)) <= 1, "text-align moves the ink");
        // Centered text wider than an overflow: hidden box starts left of the image; the visible part is drawn.
        auto narrow = text_node("Hello", style, 10, 30);
        narrow.style["text-align"] = "center";
        narrow.style["overflow"] = "hidden";
        const auto clipped = ink(weft::scene_text_image(text, narrow, 1, 4).bitmap);
        require(clipped[2] >= 0, "text left of the image is clipped, not rejected");
        auto clear = n;
        clear.style["color"] = "transparent";
        const auto none = weft::scene_text_image(text, clear, 1, 4);
        require(std::all_of(none.bitmap.rgba.begin(), none.bitmap.rgba.end(),
                            [](auto v) {
                                return v == 0;
                            }),
                "transparent text draws nothing");
        // Color font glyphs are reported, not drawn; the rest of the text still is.
        const auto emoji = weft::scene_text_image(text, text_node("A\xF0\x9F\x98\x80", style, 80, 30), 1, 4);
        require(emoji.color_glyphs && ink(emoji.bitmap)[2] >= 0, "color glyphs reported");
        bool rejected{};
        try {
            (void)weft::scene_text_image(text, n, 0, 4);
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected, "zero raster scale rejected");
    }

    struct SdfResult {
        double iou{};
        double mean{};
    };

    // Draws the node's glyph runs from SDF fields (base_ppem, spread) with the shader's coverage
    // saturate(0.5 + distance / pixel) and compares the result with the coverage image. shift (CSS px) misplaces
    // the fields for the negative control.
    SdfResult sdf_against_coverage(weft::SceneText& text, const weft::SceneNode& n, float scale, float shift = 0) {
        constexpr float margin = 4;
        const auto image = weft::scene_text_image(text, n, scale, margin);
        const auto w = image.bitmap.width;
        const auto h = image.bitmap.height;
        std::vector<float> coverage(std::size_t(w) * h);
        const auto run = text.shape(n.text, n.style);
        const auto origin = weft::scene_text_origin(n, run->metrics);
        const auto base = unsigned(weft::constants::text::sdf_default_base_ppem);
        const auto spread = float(weft::constants::text::sdf_default_spread);
        for (const auto& g : run->glyphs) {
            const auto field = text.face(g.face).rasterize_sdf(g.glyph_id);
            if (!field.width) {
                continue;
            }
            // Device pixels per field texel; field texel (0, 0) sits at the bitmap bearing.
            const float k = run->size * scale / float(base);
            const float ox = (margin + origin[0] + g.x + shift) * scale + field.left * k;
            const float oy = (margin + origin[1] + run->metrics.baseline + g.y) * scale - field.top * k;
            const auto sample = [&](float u, float v) {
                // Bilinear over texel centers, clamped to the field (outside is the most negative distance).
                u -= .5f;
                v -= .5f;
                const auto texel = [&](int x, int y) {
                    if (x < 0 || y < 0 || x >= int(field.width) || y >= int(field.height)) {
                        return -spread;
                    }
                    return (int(field.distances[std::size_t(y) * field.width + x]) -
                            weft::constants::text::sdf_zero_distance_byte) *
                           spread / float(weft::constants::text::sdf_zero_distance_byte);
                };
                const int x0 = int(std::floor(u));
                const int y0 = int(std::floor(v));
                const float fx = u - x0;
                const float fy = v - y0;
                return (texel(x0, y0) * (1 - fx) + texel(x0 + 1, y0) * fx) * (1 - fy) +
                       (texel(x0, y0 + 1) * (1 - fx) + texel(x0 + 1, y0 + 1) * fx) * fy;
            };
            const int x_end = std::min(int(w), int(std::ceil(ox + field.width * k)));
            const int y_end = std::min(int(h), int(std::ceil(oy + field.height * k)));
            for (int y = std::max(0, int(std::floor(oy))); y < y_end; ++y) {
                for (int x = std::max(0, int(std::floor(ox))); x < x_end; ++x) {
                    const float d = sample((x + .5f - ox) / k, (y + .5f - oy) / k) * k;
                    const float c = std::clamp(.5f + d, 0.f, 1.f);
                    auto& a = coverage[std::size_t(y) * w + x];
                    a = a + c * (1 - a);
                }
            }
        }
        std::size_t intersection{};
        std::size_t united{};
        double difference{};
        for (std::size_t i = 0; i < coverage.size(); ++i) {
            const float reference = image.bitmap.rgba[i * 4 + 3] / 255.f;
            const bool a = reference >= .5f;
            const bool b = coverage[i] >= .5f;
            intersection += a && b;
            united += a || b;
            difference += std::abs(reference - coverage[i]);
        }
        return {united ? double(intersection) / united : 1, difference / coverage.size()};
    }
} // namespace

int main() {
    try {
        auto& text = weft::testing::thread_scene_text();
        origins();
        images(text);

        struct SdfCase {
            const char* text;
            weft::SceneStyle style;
        };

        const std::vector<SdfCase> cases{
            {"Menu Item 42", {{"font-family", "system-ui"}, {"font-size", "16px"}, {"font-weight", "700"}}},
            {"Hover me", {{"font-family", "\"Segoe UI\", system-ui"}, {"font-size", "13px"}}},
            {"\xE4\xB8\xAD\xE6\x96\x87\xE6\xA0\x87\xE9\xA2\x98",
             {{"font-family", "sans-serif"}, {"font-size", "24px"}}},
            {"Weight", {{"font-family", "\"Noto Serif SC\""}, {"font-size", "32px"}, {"font-weight", "900"}}},
        };
        double worst_iou = 1;
        for (const auto& c : cases) {
            const auto metrics = text.measure(c.text, c.style);
            const auto n = text_node(c.text, c.style, std::ceil(metrics.width) + 2, std::ceil(metrics.height));
            for (const float scale : {1.f, 2.f, 4.f}) {
                const auto r = sdf_against_coverage(text, n, scale);
                std::cout << "SDF " << std::setw(14) << std::left << c.style.at("font-size") << " x" << scale << " IoU "
                          << r.iou << " mean " << r.mean << '\n';
                require(r.iou >= weft::test_constants::scene::sdf_text_min_iou,
                        "SDF scene text shape differs from coverage");
                require(r.mean <= weft::test_constants::scene::sdf_text_max_mean_coverage_difference,
                        "SDF scene text coverage differs");
                worst_iou = std::min(worst_iou, r.iou);
            }
            // The oracle sees a one pixel misplacement of the same fields.
            require(sdf_against_coverage(text, n, 1, 1).iou < weft::test_constants::scene::sdf_text_min_iou,
                    "SDF oracle accepted misplaced glyphs");
        }
        std::cout << "PASS scene text image: origins, color, alignment, color-glyph report; SDF fields of "
                  << cases.size() << " scene texts at 1x/2x/4x reconstruct the coverage (min IoU " << worst_iou
                  << ")\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
