#include "caseelephant/text_probe.hpp"
#include "support/check.hpp"
#include "test_constants/style_text.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <thread>

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
        std::cout
            << "PASS FreeType metrics/raster, HarfBuzz advances/clusters, scale invalidation, missing glyph and owner\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
