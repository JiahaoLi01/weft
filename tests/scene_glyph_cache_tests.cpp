#include "constants/scene.hpp"
#include "constants/text.hpp"
#include "scene/scene_text_image.hpp"
#include "support/check.hpp"
#include "test_constants/scene.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

// The glyph cache of still text's images (SceneText::glyph, scene_text_image with a phase): an image drawn from kept
// glyphs is byte for byte the one a SceneText that keeps none draws (FreeType anew), over random text, sizes, scales,
// phases, colors and synthetic oblique; a line whose text changes rasterizes only the glyphs it has not drawn; the
// cache stays within its bounds and starts over when full, its images still alike; text off the pixels (no phase)
// neither reads nor fills it. Portable: Roboto from the HarfBuzz sources.
namespace {
    using weft::testing::require;
    namespace tc = weft::test_constants::scene;

    weft::FontCatalog catalog() {
        weft::FontCatalog c;
        weft::FontFaceEntry face;
        face.family = "Roboto";
        face.source.path = WEFT_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf";
        c.faces.push_back(face);
        c.generic_families = {{"sans-serif", "Roboto"}, {"system-ui", "Roboto"}};
        return c;
    }

    weft::SceneNode text_node(std::string text, float size, bool italic, const std::string& color, float width) {
        weft::SceneNode n;
        n.tag = "div";
        n.text = std::move(text);
        n.style = {{"font-family", "Roboto"}, {"font-size", std::to_string(size) + "px"}, {"color", color}};
        if (italic) {
            n.style.set(weft::PropertyId::FontStyle, "italic");
        }
        n.box = {0, 0, width, size * 1.5f};
        return n;
    }

    struct Random {
        std::uint32_t state;

        std::uint32_t next(std::uint32_t n) {
            state = state * 1664525u + 1013904223u;
            return (state >> 8) % n;
        }
    };

    bool same(const weft::SceneTextImage& a, const weft::SceneTextImage& b) {
        return a.bitmap.width == b.bitmap.width && a.bitmap.height == b.bitmap.height && a.bitmap.rgba == b.bitmap.rgba;
    }

    // Random still text drawn twice: from the warm SceneText's kept glyphs, and by a fresh SceneText.
    void MatchesFreshRaster() {
        const auto fonts = catalog();
        weft::SceneText warm(fonts);
        Random random{tc::glyph_cache_seed};
        const std::string alphabet = "0123456789:.-+%ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz ,;!?()";
        const float sizes[]{11, 13, 13.5f, 16, 20, 27.25f, 64};
        const float scales[]{1, 1.25f, 1.5f, 2, 3};
        const char* colors[]{"#000000", "#ff8000", "rgba(10, 200, 255, 0.5)", "#ffffff"};
        for (int i = 0; i < tc::glyph_cache_cases; ++i) {
            std::string text;
            const auto length = 1 + random.next(18);
            for (std::uint32_t k = 0; k < length; ++k) {
                text += alphabet[random.next(std::uint32_t(alphabet.size()))];
            }
            const float size = sizes[random.next(std::size(sizes))];
            const float scale = scales[random.next(std::size(scales))];
            const bool italic = random.next(4) == 0;
            const auto n = text_node(text, size, italic, colors[random.next(std::size(colors))], size * 12);
            const float phase = float(random.next(16)) / 16;
            const auto kept = weft::scene_text_image(warm, n, scale, 2, phase);
            weft::SceneText fresh(fonts);
            const auto drawn = weft::scene_text_image(fresh, n, scale, 2, phase);
            require(same(kept, drawn), "case " + std::to_string(i) + " \"" + text + "\" at " + std::to_string(size) +
                                           " px, scale " + std::to_string(scale) + ", phase " + std::to_string(phase) +
                                           ": kept glyphs draw another image");
        }
        const auto s = warm.stats();
        std::cout << "glyph cache: " << s.glyph_hits << " hits, " << s.glyph_misses << " misses, " << s.glyphs
                  << " kept (" << s.glyph_bytes << " bytes)\n";
        // Random sizes, scales and phases seldom meet again: what matters here is that kept glyphs drew the images.
        require(s.glyph_hits > 0, "some glyphs came from the cache");
        require(s.glyphs <= std::size_t(weft::constants::scene::text_glyph_cache_max_entries) &&
                    s.glyph_bytes <= std::size_t(weft::constants::scene::text_glyph_cache_max_bytes),
                "the cache within its bounds");
    }

    // A clock: each second's line rasterizes only the glyphs it has not drawn before.
    void ChangingLineRasterizesNewGlyphsOnly() {
        weft::SceneText text(catalog());
        const auto draw = [&](const std::string& line) {
            weft::scene_text_image(text, text_node(line, 22, false, "#ffffff", 200), 2, 2, 0.f);
        };
        draw("00:00:00");
        const auto first = text.stats();
        draw("00:00:01");
        const auto second = text.stats();
        // "00:00:0" is drawn at the same places: only the last digit is new.
        require(second.glyph_misses - first.glyph_misses == 1, "one new glyph for the next second");
        require(second.glyph_hits - first.glyph_hits == 7, "the other seven from the cache");
        draw("00:00:01");
        require(text.stats().glyph_misses == second.glyph_misses, "the same line again: no glyph rasterized");
    }

    // Text off the pixels (moving, transformed) lies anywhere: it neither reads nor fills the cache.
    void TextOffThePixelsIsDrawnAnew() {
        weft::SceneText text(catalog());
        weft::scene_text_image(text, text_node("moving 12.5", 20, false, "#000000", 200), 1.5f, 2);
        const auto s = text.stats();
        require(!s.glyph_hits && !s.glyph_misses && !s.glyphs, "no phase: no cache");
    }

    // More glyphs than the cache keeps: it starts over within its bounds, and its images stay alike.
    void FullCacheStartsOver() {
        const auto fonts = catalog();
        weft::SceneText warm(fonts);
        std::size_t most{};
        bool cleared{};
        // Each size a new set of glyphs.
        for (int k = 0; k < tc::glyph_cache_sizes; ++k) {
            const auto n =
                text_node("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", 8 + float(k) * .5f, k % 3 == 0, "#000000", 2000);
            const auto kept = weft::scene_text_image(warm, n, 1, 2, .25f);
            const auto s = warm.stats();
            cleared = cleared || s.glyphs < most;
            most = std::max(most, s.glyphs);
            require(s.glyphs <= std::size_t(weft::constants::scene::text_glyph_cache_max_entries) &&
                        s.glyph_bytes <= std::size_t(weft::constants::scene::text_glyph_cache_max_bytes),
                    "within the bounds at size " + std::to_string(k));
            if (k % 16 == 0) {
                weft::SceneText fresh(fonts);
                require(same(kept, weft::scene_text_image(fresh, n, 1, 2, .25f)), "alike after the cache filled");
            }
        }
        require(cleared, "the cache started over");
    }

    // A clock's line drawn each frame, from kept glyphs and without (for the log).
    void Cost() {
        const auto fonts = catalog();
        weft::SceneText warm(fonts);
        const auto run = [&](bool keep) {
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < 400; ++i) {
                char line[16];
                std::snprintf(line, sizeof line, "%02d:%02d.%03d", i / 60, i % 60, i * 7 % 1000);
                const auto n = text_node(line, 22, false, "#ffffff", 200);
                if (keep) {
                    weft::scene_text_image(warm, n, 2, 2, 0.f);
                } else {
                    weft::scene_text_image(warm, n, 2, 2);
                }
            }
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 400;
        };
        run(true);
        const double kept = run(true);
        const double anew = run(false);
        std::cout << "a clock's line: " << kept << " ms from kept glyphs, " << anew << " ms rasterized anew\n";
    }
} // namespace

int main() {
    try {
        MatchesFreshRaster();
        ChangingLineRasterizesNewGlyphsOnly();
        TextOffThePixelsIsDrawnAnew();
        FullCacheStartsOver();
        Cost();
        std::cout << "PASS scene glyph cache: kept glyphs draw the images FreeType draws, within bounds\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
