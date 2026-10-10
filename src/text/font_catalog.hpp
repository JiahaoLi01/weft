#pragma once

#include "text/font.hpp"

#include <cstdint>
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
    };

    struct FontCatalog {
        std::vector<FontFaceEntry> faces;
        // CSS generic family keyword ("serif", "sans-serif", "monospace", "system-ui", ...) to a family name.
        std::vector<std::pair<std::string, std::string>> generic_families;
        // Families tried, in order, for characters that no family of the font-family list covers.
        std::vector<std::string> fallback_families;
    };

    // Family names of a CSS font-family value in order: quotes removed, unquoted generic keywords lowercased.
    std::vector<std::string> parse_font_families(std::string_view value);

    // CSS Fonts 4 font matching: for each family of the list that the catalog provides (generic keywords
    // mapped first), the face nearest to weight and style; then the same for the fallback families. Families
    // the catalog lacks are skipped and each face appears once. Empty when nothing matches.
    std::vector<FontFaceSource> match_font_faces(const FontCatalog& catalog, std::string_view families, int weight,
                                                 bool italic);
} // namespace weft
