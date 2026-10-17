#include "text/font_catalog.hpp"
#include "support/check.hpp"

#include <iostream>
#include <string>
#include <vector>

// Portable CSS font matching over catalog data: family list parsing, generic families, CSS Fonts 4 weight and
// style matching and fallback order. No font files are opened.
namespace {
#define CHECK(v) WEFT_REQUIRE(v)

    weft::FontFaceSource face(const char* name, std::uint32_t index = 0) {
        return {name, index, {}};
    }

    weft::FontCatalog catalog() {
        weft::FontCatalog c;
        for (int w : {300, 400, 600, 700}) {
            c.faces.push_back({"Body", std::uint16_t(w), false, face("body.ttf", std::uint32_t(w))});
        }
        c.faces.push_back({"Body", 400, true, face("body-italic.ttf")});
        c.faces.push_back({"Mono", 400, false, face("mono.ttf")});
        c.faces.push_back({"Mono", 700, false, face("mono-bold.ttf")});
        c.faces.push_back({"Han", 400, false, face("han.ttc", 1)});
        c.faces.push_back({"Thin Only", 100, false, face("thin.ttf")});
        c.faces.push_back({"Heavy Only", 900, false, face("heavy.ttf")});
        c.generic_families = {{"monospace", "Mono"}, {"sans-serif", "Body"}};
        c.fallback_families = {"Han", "Body"};
        return c;
    }

    void Parsing() {
        const auto names = weft::parse_font_families(" \"Noto Serif SC\" , Cinzel,  Times   New Roman,SERIF, 'A, B' ");
        CHECK((names == std::vector<std::string>{"Noto Serif SC", "Cinzel", "Times New Roman", "serif", "A, B"}));
        CHECK(weft::parse_font_families("").empty());
        // A quoted generic keyword is a family name, not the generic.
        CHECK((weft::parse_font_families("\"Serif\", Monospace") == std::vector<std::string>{"Serif", "monospace"}));
    }

    void WeightMatching() {
        const auto c = catalog();
        auto primary = [&](const char* families, int weight, bool italic = false) {
            return weft::match_font_faces(c, families, weight, italic).front();
        };
        CHECK(primary("Body", 400) == face("body.ttf", 400));
        // 400..500: prefer up to 500, then lighter, then heavier.
        CHECK(primary("Body", 500) == face("body.ttf", 400));
        // Below 400: lighter first, then heavier.
        CHECK(primary("Body", 200) == face("body.ttf", 300));
        CHECK(primary("Thin Only", 400) == face("thin.ttf"));
        // Above 500: heavier first, then lighter.
        CHECK(primary("Body", 650) == face("body.ttf", 700));
        CHECK(primary("Body", 800) == face("body.ttf", 700));
        CHECK(primary("Heavy Only", 400) == face("heavy.ttf"));
        // Italic preferred when requested, normal faces otherwise; a missing style falls back to the other one.
        CHECK(primary("Body", 400, true) == face("body-italic.ttf"));
        CHECK(primary("Mono", 400, true) == face("mono.ttf"));
        // Family names compare ASCII case-insensitively.
        CHECK(primary("body", 700) == face("body.ttf", 700));
    }

    void ListsGenericsAndFallback() {
        const auto c = catalog();
        // Missing families are skipped, generics map, fallback families follow, and no face appears twice.
        const auto faces = weft::match_font_faces(c, "Missing, monospace, Body", 700, false);
        CHECK((faces ==
               std::vector<weft::FontFaceSource>{face("mono-bold.ttf"), face("body.ttf", 700), face("han.ttc", 1)}));
        // A generic the catalog does not map contributes nothing; fallback still applies.
        CHECK((weft::match_font_faces(c, "cursive", 400, false) ==
               std::vector<weft::FontFaceSource>{face("han.ttc", 1), face("body.ttf", 400)}));
        CHECK(weft::match_font_faces(weft::FontCatalog{}, "Body", 400, false).empty());
        // How many faces the list gives, before the fallback families' (a fallback family it names counts as listed).
        std::size_t listed{99};
        CHECK(weft::match_font_faces(c, "Missing, monospace, Body", 700, false, &listed).size() == 3 && listed == 2);
        CHECK(weft::match_font_faces(c, "cursive", 400, false, &listed).size() == 2 && listed == 0);
        CHECK(weft::match_font_faces(c, "Body", 400, false, &listed).size() == 2 && listed == 1);
    }

    // CSS unicode-range values, and a family whose faces have ranges as a composite font.
    void UnicodeRanges() {
        using R = std::vector<weft::UnicodeRange>;
        CHECK((*weft::parse_unicode_range("U+0030-0039") == R{{0x30, 0x39}}));
        CHECK((*weft::parse_unicode_range(" u+3040-309F , U+4??, U+20 ") ==
               R{{0x3040, 0x309F}, {0x400, 0x4FF}, {0x20, 0x20}}));
        CHECK((*weft::parse_unicode_range("U+10FFFF") == R{{0x10FFFF, 0x10FFFF}}));
        for (const char* bad :
             {"", "0030", "U+", "U+39-30", "U+110000", "U+30,", "U+3?-40", "U+1234567", "U+30 U+40"}) {
            CHECK(!weft::parse_unicode_range(bad));
        }
        // All faces of the nearest weight, the last listed first, each with its range; a family without ranges gives
        // one face.
        weft::FontCatalog c;
        c.faces.push_back({"Mixed", 400, false, face("latin.ttf"), {{0x0, 0xFF}}});
        c.faces.push_back({"Mixed", 400, false, face("kana.ttf"), {{0x3040, 0x30FF}}});
        c.faces.push_back({"Mixed", 700, false, face("bold.ttf"), {{0x0, 0xFF}}});
        c.faces.push_back({"Plain", 400, false, face("plain.ttf")});
        c.faces.push_back({"Plain", 400, false, face("plain-copy.ttf")});
        std::vector<std::vector<weft::UnicodeRange>> ranges;
        CHECK((weft::match_font_faces(c, "Mixed, Plain", 400, false, nullptr, &ranges) ==
               std::vector<weft::FontFaceSource>{face("kana.ttf"), face("latin.ttf"), face("plain.ttf")}));
        CHECK((ranges == std::vector<R>{{{0x3040, 0x30FF}}, {{0x0, 0xFF}}, {}}));
        CHECK((weft::match_font_faces(c, "Mixed", 700, false, nullptr, &ranges) ==
               std::vector<weft::FontFaceSource>{face("bold.ttf")}) &&
              ranges.size() == 1);
    }

    // Family names read from the name table, for platform font lists that report only files.
    void FamilyNames() {
        auto names = [](const char* path, std::uint32_t index = 0) {
            return weft::font_family_names({path, index, {}});
        };
        CHECK((names(WEFT_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf") ==
               std::vector<std::string>{"Roboto"}));
        CHECK((names(WEFT_SOURCE_DIR "/third_party/ahem/Ahem.ttf") == std::vector<std::string>{"Ahem"}));
        // Its English and Chinese records name the same family: listed once.
        CHECK((names(WEFT_SOURCE_DIR "/third_party/noto-cjk/source/NotoSansCJKsc-Regular.otf") ==
               std::vector<std::string>{"Noto Sans CJK SC"}));
        bool rejected{};
        try {
            names(WEFT_SOURCE_DIR "/third_party/ahem/LICENSE.txt");
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        CHECK(rejected);
    }
} // namespace

int main() {
    try {
        Parsing();
        WeightMatching();
        ListsGenericsAndFallback();
        UnicodeRanges();
        FamilyNames();
        std::cout << "PASS font catalog: family parsing, CSS weight/style matching, generics and fallback, family "
                     "names\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
