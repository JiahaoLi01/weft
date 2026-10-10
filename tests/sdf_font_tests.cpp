#include "text/font.hpp"
#include "support/check.hpp"
#include "constants/text.hpp"
#include "test_constants/style_text.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
    using weft::testing::require;

    float distance(std::uint8_t v, unsigned spread) {
        return (int(v) - weft::constants::text::sdf_zero_distance_byte) * static_cast<float>(spread) /
               static_cast<float>(weft::constants::text::sdf_zero_distance_byte);
    }
} // namespace

int main() {
    try {
        weft::Font font(WEFT_SOURCE_DIR "/third_party/ahem/Ahem.ttf");
        const auto shaped = font.shape("X X", 40);
        const auto id = shaped.glyphs[0].glyph_id;
        const auto normal = font.rasterize(id, 40);
        const auto sdf = font.rasterize_sdf(id, 40, 8);
        require(normal.width == 40 && normal.height == 40, "known square geometry");
        require(sdf.width == 56 && sdf.height == 56 && sdf.left == normal.left - 8 && sdf.top == normal.top + 8,
                "SDF padding and bearing");
        require(sdf.base_ppem == 40 && sdf.spread == 8 && sdf.source == weft::SdfSource::Bitmap,
                "SDF generator metadata");
        require(sdf.distances.size() == 3136 && sdf.distances[28 * 56 + 28] == 255, "interior positive and saturated");
        require(sdf.distances.front() < weft::constants::text::sdf_zero_distance_byte, "exterior sign");
        float max_error{};
        for (unsigned x = 0; x < 56; ++x) {
            const float geometric = std::clamp(std::min(x + 0.5f - 8, 48 - (x + 0.5f)), -8.0f, 127 / 128.0f * 8);
            max_error = std::max(max_error, std::abs(distance(sdf.distances[28 * 56 + x], 8) - geometric));
        }
        require(max_error < weft::test_constants::style_text::sdf_max_distance_error,
                "SDF distance ramp differs from independent square edge");
        const auto spread4 = font.rasterize_sdf(id, 40, 4);
        require(spread4.width == 48 && spread4.left == normal.left - 4, "spread change did not reset padding");
        const auto repeat = font.rasterize_sdf(id, 40, 8);
        require(repeat.distances == sdf.distances, "spread reset changed result");
        const auto outline = font.rasterize_sdf(id, 40, 8, weft::SdfSource::Outline);
        require(outline.width == sdf.width &&
                    outline.distances[28 * 56 + 28] > weft::constants::text::sdf_zero_distance_byte,
                "explicit outline generator");
        const auto space = font.rasterize_sdf(shaped.glyphs[1].glyph_id, 40, 8);
        require(space.distances.empty() && space.width == 0 && font.shape(" ", 40).advance == 40,
                "space acquired an SDF quad or lost advance");
        const auto after = font.shape("X X", 40);
        require(after.advance == shaped.advance && after.glyphs[0].advance_x == shaped.glyphs[0].advance_x,
                "SDF changed shaping metrics");
        require(font.rasterize(id, 40).coverage == normal.coverage, "SDF changed grayscale raster");
        for (auto spread : {0u, 1u, 33u}) {
            bool rejected{};
            try {
                (void)font.rasterize_sdf(id, 40, spread);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            require(rejected, "invalid spread accepted");
        }
        for (auto base : {0u, 15u, 129u}) {
            bool rejected{};
            try {
                (void)font.rasterize_sdf(id, base, 8);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            require(rejected, "invalid base size accepted");
        }
        bool wrong_source{};
        bool wrong_glyph{};
        bool wrong_owner{};
        try {
            (void)font.rasterize_sdf(id, 40, 8, static_cast<weft::SdfSource>(99));
        } catch (const std::invalid_argument&) {
            wrong_source = true;
        }
        try {
            (void)font.rasterize_sdf(UINT32_MAX, 40, 8);
        } catch (const std::invalid_argument&) {
            wrong_glyph = true;
        }
        std::thread outsider([&] {
            try {
                (void)font.rasterize_sdf(id, 40, 8);
            } catch (const std::logic_error&) {
                wrong_owner = true;
            }
        });
        outsider.join();
        require(wrong_source && wrong_glyph && wrong_owner, "invalid SDF input or cross-thread font access accepted");
        weft::Font roboto(WEFT_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf");
        const auto latin = roboto.shape("SDF Ag", 40);
        require(!latin.missing_glyph && latin.glyphs.size() == 6, "Roboto actual Latin shaping");
        for (const auto& g : latin.glyphs) {
            const auto field = roboto.rasterize_sdf(g.glyph_id, 40, 8);
            if (field.distances.empty()) {
                continue;
            }
            require(*std::min_element(field.distances.begin(), field.distances.end()) <
                            weft::constants::text::sdf_zero_distance_byte &&
                        *std::max_element(field.distances.begin(), field.distances.end()) >
                            weft::constants::text::sdf_zero_distance_byte,
                    "Latin distance field lacks contour");
        }
        std::cout
            << "PASS SDF sign/range/padding/metrics/space, generator and spread isolation, actual Latin; square distance max error "
            << max_error << " source texels\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
