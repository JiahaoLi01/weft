#include "text/font.hpp"
#include "support/check.hpp"
#include "test_constants/style_text.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <thread>
#include <vector>

namespace {
    using weft::testing::require;

    bool near(float a, float b) {
        return std::abs(a - b) < weft::test_constants::style_text::float_tolerance;
    }
} // namespace

int main() {
    try {
        weft::Font font(WEFT_SOURCE_DIR "/third_party/ahem/Ahem.ttf");
        const auto metrics = font.metrics(20);
        require(near(metrics.ascent, 16) && near(metrics.descent, 4), "Ahem baseline metrics");
        const auto shaped = font.shape("XX XX", 20);
        require(near(shaped.advance, 100) && shaped.glyphs.size() == 5 && !shaped.missing_glyph,
                "real HB advances and glyphs");
        require(shaped.glyphs[3].utf8_cluster == 3, "HB cluster byte offset");
        require(near(font.shape("XX", 40).advance, 80) && near(font.shape("XX", 20).advance, 40),
                "font size changes do not reuse stale scale");
        require(font.shape("", 20).glyphs.empty(), "empty shape");
        const auto bitmap = font.rasterize(shaped.glyphs[0].glyph_id, 20);
        require(bitmap.width == 20 && bitmap.height == 20 && bitmap.top == 16, "FreeType square glyph extent");
        require(bitmap.coverage.size() == 400 && std::all_of(bitmap.coverage.begin(), bitmap.coverage.end(),
                                                             [](auto c) {
                                                                 return c == 255;
                                                             }),
                "actual square glyph coverage");
        const auto double_bitmap = font.rasterize(shaped.glyphs[0].glyph_id, 40);
        require(double_bitmap.width == 40 && double_bitmap.height == 40, "raster scale changed");
        // A subpixel origin moves the square, not the whole pixel origin: half covered edge columns and rows.
        const auto offset = font.rasterize(shaped.glyphs[0].glyph_id, 20, .5f, .25f);
        require(offset.width == 21 && offset.height == 21 && offset.left == 0 && offset.top == 16,
                "subpixel glyph extent");
        const auto at = [&](unsigned x, unsigned y) {
            return int(offset.coverage[std::size_t(y) * offset.width + x]);
        };
        require(std::abs(at(0, 10) - 128) <= 1 && std::abs(at(20, 10) - 128) <= 1 && at(10, 10) == 255,
                "half pixel horizontal offset coverage");
        require(std::abs(at(10, 0) - 191) <= 1 && std::abs(at(10, 20) - 64) <= 1,
                "quarter pixel vertical offset coverage (y down)");
        require(font.rasterize(shaped.glyphs[0].glyph_id, 20, 0, 0).coverage == bitmap.coverage,
                "zero offset is the whole pixel raster");
        for (const auto bad : {-.25f, 1.f, std::nanf("")}) {
            bool rejected{};
            try {
                (void)font.rasterize(shaped.glyphs[0].glyph_id, 20, bad, 0);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            require(rejected, "glyph offset outside [0,1) accepted");
        }
        require(font.shape("\xE4\xB8\xAD", 20).missing_glyph, "CJK missing glyph cannot masquerade as supported font");
        bool invalid{};
        try {
            (void)font.metrics(0);
        } catch (const std::invalid_argument&) {
            invalid = true;
        }
        require(invalid, "invalid size rejected");
        std::atomic<bool> rejected{};
        std::thread outsider([&] {
            try {
                (void)font.shape("X", 20);
            } catch (const std::logic_error&) {
                rejected = true;
            }
        });
        outsider.join();
        require(rejected, "font owner thread enforced");
        // A font file is read once while fonts hold it, and the fonts of other threads read the same bytes, through the
        // same HarfBuzz face: they shape alike, at once.
        {
            const auto file = weft::FontFile::open(WEFT_SOURCE_DIR "/third_party/ahem/Ahem.ttf");
            require(weft::FontFile::open(WEFT_SOURCE_DIR "/third_party/ahem/Ahem.ttf") == file,
                    "a font file held is not read again");
            std::atomic<int> alike{};
            std::vector<std::thread> shapers;
            for (int t = 0; t < 4; ++t) {
                shapers.emplace_back([&] {
                    weft::Font there(file, 0);
                    bool same = true;
                    for (int i = 0; i < 200; ++i) {
                        same = same && near(there.shape("XX XX", 20).advance, shaped.advance);
                    }
                    alike += same;
                });
            }
            for (auto& shaper : shapers) {
                shaper.join();
            }
            require(alike == 4, "fonts of one file on four threads shape alike");
        }
        std::cout
            << "PASS FreeType metrics/raster, HarfBuzz advances/clusters, scale invalidation, missing glyph, owner and "
               "shared font files\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
