#include "text/font.hpp"
#include "support/check.hpp"
#include "constants/text.hpp"
#include "test_constants/style_text.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <thread>
using namespace weft;

namespace {
    using weft::testing::require;

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

    // With roles, as Chrome's system fallback chooses: the font-family list's fonts first; then a monochrome fallback
    // font for an emoji shown as text (by default, or asked with VS15) and a color one for an emoji shown as emoji (by
    // default, or asked with VS16); any fallback font covering it when none of that role does. The installed Segoe UI,
    // Segoe UI Emoji (color) and Segoe UI Symbol (monochrome).
    void emoji_presentation() {
        const std::filesystem::path installed = "C:/Windows/Fonts";
        if (!std::filesystem::exists(installed / "segoeui.ttf") ||
            !std::filesystem::exists(installed / "seguiemj.ttf") ||
            !std::filesystem::exists(installed / "seguisym.ttf")) {
            std::cout << "skip emoji presentation: no Segoe UI, Segoe UI Emoji and Segoe UI Symbol\n";
            return;
        }
        Font ui(installed / "segoeui.ttf");
        Font color(installed / "seguiemj.ttf");
        Font symbol(installed / "seguisym.ttf");
        const std::vector<Font*> fonts{&ui, &color, &symbol};
        const auto font_of = [&](std::string_view text, std::span<const FontRole> roles) {
            const auto r = shape_with_fallback(fonts, text, 16, ShapePrecision::Fixed26_6, {}, roles);
            require(!r.shaped.missing_glyph && r.runs.size() == 1, "one run of one font");
            return r.runs[0].font_id;
        };
        const std::array system{FontRole::Listed, FontRole::FallbackColor, FontRole::FallbackText};
        const auto dagger = "\xF0\x9F\x97\xA1"; // U+1F5E1, text by default
        const auto scroll = "\xF0\x9F\x93\x9C"; // U+1F4DC, emoji by default
        const auto as_emoji = "\xEF\xB8\x8F";   // VS16
        const auto as_text = "\xEF\xB8\x8E";    // VS15
        require(font_of("A", system) == 0, "the listed font covers Latin");
        require(font_of(dagger, system) == 2, "a text emoji in the monochrome fallback");
        require(font_of(std::string(dagger) + as_emoji, system) == 1, "VS16 asks for the color fallback");
        require(font_of(scroll, system) == 1, "an emoji in the color fallback");
        require(font_of(std::string(scroll) + as_text, system) == 2, "VS15 asks for the monochrome fallback");
        const std::array listed{FontRole::Listed, FontRole::Listed, FontRole::FallbackText};
        require(font_of(dagger, listed) == 1, "a listed color font before the monochrome fallback");
        const std::array color_only{FontRole::Listed, FontRole::FallbackColor, FontRole::FallbackColor};
        require(font_of(dagger, color_only) == 1, "a color fallback when no monochrome one covers it");
        require(font_of(dagger, {}) == 1, "without roles, the first font that covers it");
        rejects(
            [&] {
                const std::array one{FontRole::Listed};
                (void)shape_with_fallback(fonts, dagger, 16, ShapePrecision::Fixed26_6, {}, one);
            },
            "WEFT_FONT_SET:");
    }

    // Fonts asked for as shaping needs them shape every text as the whole list does, glyph for glyph, and a later font
    // opens only when a cluster reaches it: Latin text opens the first font alone, Chinese the CJK font too, and no
    // text opens a font that the list holds after the one that renders it. Roboto and Noto Sans CJK SC listed, then the
    // installed Segoe UI, Segoe UI Emoji and Segoe UI Symbol as the system's fallback.
    void fonts_open_when_needed() {
        const std::filesystem::path installed = "C:/Windows/Fonts";
        const std::vector<std::filesystem::path> paths{
            WEFT_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf",
            WEFT_SOURCE_DIR "/third_party/noto-cjk/source/NotoSansCJKsc-Regular.otf", installed / "segoeui.ttf",
            installed / "seguiemj.ttf", installed / "seguisym.ttf"};
        if (!std::all_of(paths.begin(), paths.end(), [](const std::filesystem::path& p) {
                return std::filesystem::exists(p);
            })) {
            std::cout << "skip fonts opened when needed: no Segoe UI, Segoe UI Emoji and Segoe UI Symbol\n";
            return;
        }
        std::vector<std::unique_ptr<Font>> all;
        std::vector<Font*> list;
        for (const auto& p : paths) {
            list.push_back(all.emplace_back(std::make_unique<Font>(p)).get());
        }
        const std::array roles{FontRole::Listed, FontRole::Listed, FontRole::FallbackText, FontRole::FallbackColor,
                               FontRole::FallbackText};
        const auto same = [](const FallbackText& a, const FallbackText& b) {
            const auto glyph = [](const Glyph& x, const Glyph& y) {
                return x.glyph_id == y.glyph_id && x.utf8_cluster == y.utf8_cluster && x.advance_x == y.advance_x &&
                       x.advance_y == y.advance_y && x.offset_x == y.offset_x && x.offset_y == y.offset_y &&
                       x.font_id == y.font_id;
            };
            const auto run = [](const FontRun& x, const FontRun& y) {
                return x.font_id == y.font_id && x.utf8_start == y.utf8_start && x.utf8_length == y.utf8_length &&
                       x.metrics.ascent == y.metrics.ascent && x.metrics.descent == y.metrics.descent &&
                       x.metrics.line_gap == y.metrics.line_gap;
            };
            return a.shaped.advance == b.shaped.advance && a.shaped.missing_glyph == b.shaped.missing_glyph &&
                   std::equal(a.shaped.glyphs.begin(), a.shaped.glyphs.end(), b.shaped.glyphs.begin(),
                              b.shaped.glyphs.end(), glyph) &&
                   std::equal(a.runs.begin(), a.runs.end(), b.runs.begin(), b.runs.end(), run);
        };
        const std::vector<std::string> texts{"Hello, world 123",
                                             "进度 42%",
                                             "A仓B",
                                             "Level 7 · 战斗",
                                             "ｶﾀｶﾅ ひらがな",
                                             "한국어",
                                             "κόσμε Привет",
                                             "\xE2\x98\xA0\xEF\xB8\x8F",
                                             "\xF0\x9F\x97\xA1",
                                             "\xF0\x9F\x97\xA1\xEF\xB8\x8F",
                                             "\xF0\x9F\x93\x9C",
                                             "\xF0\x9F\x93\x9C\xEF\xB8\x8E",
                                             "\xE2\x98\x85 \xE2\x86\x90 \xE2\x99\xAA",
                                             "mixed 战斗 \xE2\x98\xA0\xEF\xB8\x8F end",
                                             " "};
        std::size_t opened_in_all{};
        for (const auto& text : texts) {
            for (const bool with_roles : {true, false}) {
                const auto whole =
                    shape_with_fallback(list, text, 16, ShapePrecision::Fixed16_16, {},
                                        with_roles ? std::span<const FontRole>(roles) : std::span<const FontRole>());
                std::vector<std::unique_ptr<Font>> opened(paths.size());
                const auto font = [&](std::uint32_t i) -> Font& {
                    if (!opened[i]) {
                        opened[i] = std::make_unique<Font>(paths[i]);
                    }
                    return *opened[i];
                };
                const auto role = [&](std::uint32_t i) {
                    return roles[i];
                };
                const auto asked = shape_with_fallback(FallbackFonts{paths.size(), font, role, with_roles}, text, 16,
                                                       ShapePrecision::Fixed16_16);
                require(same(asked, whole), "fonts asked for as needed shape as the whole list: " + text);
                std::size_t count{};
                std::uint32_t last{};
                for (std::uint32_t i = 0; i < opened.size(); ++i) {
                    if (opened[i]) {
                        ++count;
                        last = i;
                    }
                }
                std::uint32_t used{};
                for (const auto& r : whole.runs) {
                    used = std::max(used, r.font_id);
                }
                // A font after every font the text uses is never reached, but an emoji that prefers a later role
                // passes over fonts it does not ask to shape it.
                require(last >= used && (with_roles || last == used),
                        "a font after the ones the text uses was opened: " + text);
                opened_in_all += count;
                if (text == "Hello, world 123") {
                    require(count == 1, "Latin text opened a fallback font");
                }
                if (text == "进度 42%") {
                    require(count == 2 && opened[1], "Chinese text opened more than its CJK font");
                }
            }
        }
        std::cout << "  fonts opened when needed: " << opened_in_all << " of " << texts.size() * 2 * paths.size()
                  << " for " << texts.size() * 2 << " shapings\n";
    }
} // namespace

int main() {
    try {
        const auto latin =
            std::filesystem::path(WEFT_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf");
        const auto cjk =
            std::filesystem::path(WEFT_SOURCE_DIR "/third_party/noto-cjk/source/NotoSansCJKsc-Regular.otf");
        FontSet fonts({latin, cjk});
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
                    weft::test_constants::style_text::float_tolerance,
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
        FontSet reference({latin, cjk});
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
        for (unsigned i = 0; i < weft::test_constants::style_text::shape_cache_churn_inserts; ++i) {
            (void)fonts.shape("cache-churn-" + std::to_string(i), 16);
        }
        require(fonts.shape_cache_stats().entries <= weft::constants::text::shape_cache_max_entries &&
                    fonts.shape_cache_stats().bytes <= weft::constants::text::shape_cache_max_bytes &&
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
        FontSet missing({latin});
        require(missing.shape("仓", 16).shaped.missing_glyph, "missing fallback needs diagnostic data");
        rejects(
            [&] {
                fonts.shape(std::string("\xc0\xaf", 2), 16);
            },
            "WEFT_TEXT_UTF8:");
        // Left-to-right scripts that shape without itemization: kana and Hangul fall to the CJK font, Greek and
        // Cyrillic stay in the Latin one.
        const auto kana = fonts.shape("\u304b\u306a\u30ab\u30ca\ud55c\uae00", 16);
        require(!kana.shaped.missing_glyph && kana.runs.size() == 1 && kana.runs[0].font_id == 1, "kana and Hangul");
        const auto greek = fonts.shape("\u0391\u03bb\u03c6\u03b1 \u041a\u0438\u0440\u0438\u043b\u043b", 16);
        require(!greek.shaped.missing_glyph && greek.runs.size() == 1 && greek.runs[0].font_id == 0,
                "Greek and Cyrillic");
        // unicode-range: a first font of digits only gives its letters to the next; a range that leaves out a
        // character the font has does not cover it.
        {
            Font roboto(latin);
            Font noto(cjk);
            const std::vector<Font*> pair{&roboto, &noto};
            const std::vector<std::vector<UnicodeRange>> digits{{{U'0', U'9'}}, {}};
            const auto ranged = shape_with_fallback(pair, "A1", 16, ShapePrecision::Fixed26_6, {}, {}, digits);
            require(!ranged.shaped.missing_glyph && ranged.runs.size() == 2 && ranged.runs[0].font_id == 1 &&
                        ranged.runs[1].font_id == 0,
                    "letters outside the first font's range");
            const std::vector<std::vector<UnicodeRange>> no_han{{}, {{U'0', U'9'}}};
            require(
                shape_with_fallback(pair, "\u4ed3", 16, ShapePrecision::Fixed26_6, {}, {}, no_han).shaped.missing_glyph,
                "a range leaves out what the font has");
            rejects(
                [&] {
                    const std::vector<std::vector<UnicodeRange>> one{{}};
                    (void)shape_with_fallback(pair, "A", 16, ShapePrecision::Fixed26_6, {}, {}, one);
                },
                "WEFT_FONT_SET:");
        }
        rejects(
            [&] {
                fonts.shape("العربية", 16);
            },
            "WEFT_TEXT_SCRIPT:");
        rejects(
            [&] {
                fonts.shape("A\xe2\x80\xae", 16);
            },
            "WEFT_TEXT_SCRIPT:");
        rejects(
            [&] {
                fonts.shape("test", 0);
            },
            "WEFT_FONT_SIZE:");
        auto bad = mixed.shaped.glyphs[0];
        bad.font_id = 99;
        rejects(
            [&] {
                fonts.rasterize(bad, 16);
            },
            "WEFT_FONT_ID:");
        rejects(
            [&] {
                fonts.primary().rasterize(mixed.shaped.glyphs[1], 16);
            },
            "WEFT_FONT_ID:");
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
        emoji_presentation();
        fonts_open_when_needed();
        std::cout
            << "PASS real CJK fallback, source clusters, combining cluster, Latin ligatures, emoji presentation, fonts "
               "opened when needed, missing glyph, UTF8/script/owner/resource diagnostics\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
