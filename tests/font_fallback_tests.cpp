#include "caseelephant/text_probe.hpp"
#include "support/check.hpp"
#include "caseelephant/constants/text.hpp"
#include "test_constants/style_text.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace caseelephant;

namespace {
    using caseelephant::testing::require;

    template <class F>
    void rejects(F f, const char* prefix) {
        try {
            f();
        } catch (const std::exception& e) {
            require(std::string(e.what()).starts_with(prefix), "wrong diagnostic");
            return;
        }
        throw std::runtime_error("unexpected acceptance");
    }
} // namespace

int main() {
    try {
        const auto latin =
            std::filesystem::path(CE_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf");
        const auto cjk = std::filesystem::path(CE_SOURCE_DIR "/third_party/noto-cjk/source/NotoSansCJKsc-Regular.otf");
        TextProbeFontSet fonts({latin, cjk});
        require(fonts.primary().shape("仓库", 16).missing_glyph, "primary font unexpectedly covers Chinese");
        const auto mixed = fonts.shape("A仓B", 16);
        require(!mixed.shaped.missing_glyph && mixed.runs.size() == 3, "Chinese fallback failed");
        require(mixed.runs[0].font_id == 0 && mixed.runs[1].font_id == 1 && mixed.runs[2].font_id == 0, "face order");
        require(mixed.runs[1].utf8_start == 1 && mixed.runs[1].utf8_length == 3 &&
                    mixed.shaped.glyphs[1].utf8_cluster == 1,
                "UTF8 source clusters lost");
        const auto latin_shape = fonts.primary().shape("office AV", 20);
        const auto set_shape = fonts.shape("office AV", 20);
        require(set_shape.runs.size() == 1 && set_shape.runs[0].font_id == 0 &&
                    latin_shape.glyphs.size() == set_shape.shaped.glyphs.size(),
                "Latin ligatures split by cluster fallback");
        require(std::abs(latin_shape.advance - set_shape.shaped.advance) <
                    caseelephant::test_constants::style_text::float_tolerance,
                "Latin advance changed");
        const auto hits = fonts.shape_cache_stats().hits;
        const auto repeated = fonts.shape("A仓B", 16);
        require(fonts.shape_cache_stats().hits == hits + 1 && repeated.shaped.advance == mixed.shaped.advance,
                "warm shape result not reused");
        // Returned values are independent: a caller cannot poison the next hit.
        auto poisoned = repeated;
        poisoned.shaped.glyphs.front().glyph_id = 0;
        require(fonts.shape("A仓B", 16).shaped.glyphs.front().glyph_id == mixed.shaped.glyphs.front().glyph_id,
                "shape cache exposes mutable storage");
        TextProbeFontSet reference({latin, cjk});
        for (const auto size : {13.25f, 52.f, 16.f, 27.f, 13.25f}) {
            const auto actual = fonts.shape("AV 仓库 office", size);
            const auto expected = reference.shape("AV 仓库 office", size);
            require(actual.shaped.advance == expected.shaped.advance &&
                        actual.shaped.glyphs.size() == expected.shaped.glyphs.size(),
                    "cached scale/fallback advance differs from fresh font set");
            for (std::size_t i = 0; i < actual.shaped.glyphs.size(); ++i) {
                const auto& a = actual.shaped.glyphs[i];
                const auto& e = expected.shaped.glyphs[i];
                require(a.glyph_id == e.glyph_id && a.font_id == e.font_id && a.utf8_cluster == e.utf8_cluster &&
                            a.advance_x == e.advance_x && a.offset_x == e.offset_x && a.offset_y == e.offset_y,
                        "cached source cluster/font/position differs");
            }
        }
        for (unsigned i = 0; i < caseelephant::test_constants::style_text::shape_cache_churn_inserts; ++i) {
            (void)fonts.shape("cache-churn-" + std::to_string(i), 16);
        }
        require(fonts.shape_cache_stats().entries <= caseelephant::constants::text::shape_cache_max_entries &&
                    fonts.shape_cache_stats().bytes <= caseelephant::constants::text::shape_cache_max_bytes &&
                    fonts.shape("A仓B", 16).shaped.advance == mixed.shaped.advance,
                "bounded shape cache eviction changed output");
        const auto composed = fonts.shape("仓\xcc\x81", 20);
        require(composed.runs.size() == 1 && composed.runs[0].font_id == 1 && composed.runs[0].utf8_length == 5,
                "combining cluster split across faces");
        for (const auto& g : composed.shaped.glyphs) {
            require(g.utf8_cluster == 0 && g.font_id == 1, "combining cluster identity");
        }
        for (const auto& g : mixed.shaped.glyphs) {
            const auto bitmap = fonts.rasterize(g, 16);
            require(!bitmap.coverage.empty(), "actual fallback glyph not rasterized");
        }
        require(fonts.shape("\xf0\xb1\x8d\x90", 16).shaped.missing_glyph,
                "missing Extension-H glyph silently accepted");
        TextProbeFontSet missing({latin});
        require(missing.shape("仓", 16).shaped.missing_glyph, "missing fallback needs diagnostic data");
        rejects(
            [&] {
                fonts.shape(std::string("\xc0\xaf", 2), 16);
            },
            "CE_TEXT_UTF8:");
        rejects(
            [&] {
                fonts.shape("العربية", 16);
            },
            "CE_TEXT_SCRIPT:");
        rejects(
            [&] {
                fonts.shape("A\xe2\x80\xae", 16);
            },
            "CE_TEXT_SCRIPT:");
        rejects(
            [&] {
                fonts.shape("test", 0);
            },
            "CE_FONT_SIZE:");
        auto bad = mixed.shaped.glyphs[0];
        bad.font_id = 99;
        rejects(
            [&] {
                fonts.rasterize(bad, 16);
            },
            "CE_FONT_ID:");
        rejects(
            [&] {
                fonts.primary().rasterize(mixed.shaped.glyphs[1], 16);
            },
            "CE_FONT_ID:");
        bool owner_rejected{};
        std::thread other([&] {
            try {
                fonts.shape("仓", 16);
            } catch (const std::logic_error&) {
                owner_rejected = true;
            }
        });
        other.join();
        require(owner_rejected, "font owner violation");
        std::cout
            << "PASS real CJK fallback, source clusters, combining cluster, Latin ligatures, missing glyph, UTF8/script/owner/resource diagnostics\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
