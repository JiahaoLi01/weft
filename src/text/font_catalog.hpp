#pragma once

#include "text/font.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Font selection as data. A platform layer (src/platform) or the host lists the faces it can provide and its
// generic family choices; matching is portable code over that list, so no platform font API is needed to lay
// out or shape text.
namespace weft {
    struct FontFaceEntry {
        // Family name as CSS matches it (ASCII case-insensitive). A face listed under several localized
        // names appears once per name.
        std::string family;
        std::uint16_t weight{400};
        bool italic{};
        FontFaceSource source;
        // The code points the face supplies (CSS unicode-range); empty: every one.
        std::vector<UnicodeRange> unicode_range;
    };

    struct FontCatalog {
        std::vector<FontFaceEntry> faces;
        // CSS generic family keyword ("serif", "sans-serif", "monospace", "system-ui", ...) to a family name.
        std::vector<std::pair<std::string, std::string>> generic_families;
        // Families tried, in order, for characters that no family of the font-family list covers.
        std::vector<std::string> fallback_families;
        // Families tried before those, in order, for text of a script (its ISO 15924 tag, as script_runs gives it), as
        // the platform's own fallback chooses a font by script (the system's choice for a character of each script,
        // script_fallback_samples).
        std::vector<std::pair<std::uint32_t, std::vector<std::string>>> script_fallbacks;
    };

    // The scripts a platform layer asks its fallback about, each with a letter of it whose font stands for the
    // script's (its ISO 15924 tag and a code point): scripts that the fonts of a page's list and of the general
    // fallback list seldom cover.
    std::span<const std::pair<std::uint32_t, char32_t>> script_fallback_samples();

    // An ISO 15924 tag from its four letters ("Arab").
    constexpr std::uint32_t script_tag(const char (&name)[5]) {
        return std::uint32_t(std::uint8_t(name[0])) << 24 | std::uint32_t(std::uint8_t(name[1])) << 16 |
               std::uint32_t(std::uint8_t(name[2])) << 8 | std::uint32_t(std::uint8_t(name[3]));
    }

    // Family names of a CSS font-family value in order: quotes removed, unquoted generic keywords lowercased.
    std::vector<std::string> parse_font_families(std::string_view value);

    // A CSS unicode-range value: comma-separated U+XXXX, U+XXXX-YYYY and wildcard U+XX?? ranges, each within
    // U+0-10FFFF (CSS Fonts 4). None for text that is not one.
    std::optional<std::vector<UnicodeRange>> parse_unicode_range(std::string_view value);

    // CSS Fonts 4 font matching: for each family of the list that the catalog provides (generic keywords
    // mapped first), the face nearest to weight and style; then the same for the fallback families. Families
    // the catalog lacks are skipped and each face appears once. Empty when nothing matches.
    // The family names that a face's name table lists: its typographic family names (name ID 16), else its family
    // names (name ID 1), in every language and once each, for platform font lists that report files but no names.
    // Throws std::runtime_error("WEFT_FONT_FILE: ...") for a file or face FreeType cannot open.
    std::vector<std::string> font_family_names(const FontFaceSource& face);

    // listed, when given, receives how many of the faces come from the font-family list (the rest are fallback).
    // A family whose faces have unicode-ranges is a composite font: every face of the nearest weight and style, the
    // last listed first (CSS Fonts 4, 4.5); ranges, when given, receives each face's unicode-range in order. With a
    // script (an ISO 15924 tag), the catalog's fallback families for it come before the general fallback families.
    std::vector<FontFaceSource> match_font_faces(const FontCatalog& catalog, std::string_view families, int weight,
                                                 bool italic, std::size_t* listed = nullptr,
                                                 std::vector<std::vector<UnicodeRange>>* ranges = nullptr,
                                                 std::uint32_t script = 0);
} // namespace weft
