#include "text/font_catalog.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_IDS_H

#include <algorithm>
#include <cctype>
#include <optional>
#include <stdexcept>

namespace weft {
    namespace {
        std::string lower(std::string_view value) {
            std::string out(value);
            for (auto& c : out) {
                c = char(std::tolower(static_cast<unsigned char>(c)));
            }
            return out;
        }

        bool same_family(std::string_view a, std::string_view b) {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                       return std::tolower(static_cast<unsigned char>(x)) ==
                              std::tolower(static_cast<unsigned char>(y));
                   });
        }

        // UTF-8 of big-endian UTF-16 bytes; nothing for a malformed string.
        std::optional<std::string> utf16be(const FT_Byte* bytes, FT_UInt length) {
            if (length % 2) {
                return std::nullopt;
            }
            std::string out;
            for (FT_UInt i = 0; i < length; i += 2) {
                std::uint32_t c = std::uint32_t(bytes[i]) << 8 | bytes[i + 1];
                if (c >= 0xD800 && c < 0xDC00) {
                    if (i + 3 >= length) {
                        return std::nullopt;
                    }
                    const std::uint32_t low = std::uint32_t(bytes[i + 2]) << 8 | bytes[i + 3];
                    if (low < 0xDC00 || low >= 0xE000) {
                        return std::nullopt;
                    }
                    c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                    i += 2;
                } else if (c >= 0xDC00 && c < 0xE000) {
                    return std::nullopt;
                }
                if (c < 0x80) {
                    out += char(c);
                } else if (c < 0x800) {
                    out += char(0xC0 | c >> 6);
                    out += char(0x80 | (c & 0x3F));
                } else if (c < 0x10000) {
                    out += char(0xE0 | c >> 12);
                    out += char(0x80 | (c >> 6 & 0x3F));
                    out += char(0x80 | (c & 0x3F));
                } else {
                    out += char(0xF0 | c >> 18);
                    out += char(0x80 | (c >> 12 & 0x3F));
                    out += char(0x80 | (c >> 6 & 0x3F));
                    out += char(0x80 | (c & 0x3F));
                }
            }
            return out;
        }

        std::string trim(std::string_view value) {
            const auto begin = value.find_first_not_of(" \t\r\n");
            if (begin == value.npos) {
                return {};
            }
            const auto end = value.find_last_not_of(" \t\r\n");
            return std::string(value.substr(begin, end - begin + 1));
        }

        // CSS Fonts 4 section 5.2 step 4: the available weight nearest to the desired one.
        std::optional<std::uint16_t> nearest_weight(std::vector<std::uint16_t> available, int desired) {
            if (available.empty()) {
                return std::nullopt;
            }
            std::sort(available.begin(), available.end());
            if (std::find(available.begin(), available.end(), desired) != available.end()) {
                return std::uint16_t(desired);
            }
            auto above = [&](int from, int to) -> std::optional<std::uint16_t> {
                for (auto w : available) {
                    if (w > from && w <= to) {
                        return w;
                    }
                }
                return std::nullopt;
            };
            auto below = [&](int from) -> std::optional<std::uint16_t> {
                for (auto i = available.rbegin(); i != available.rend(); ++i) {
                    if (*i < from) {
                        return *i;
                    }
                }
                return std::nullopt;
            };
            constexpr int normal = 400;
            constexpr int medium = 500;
            if (desired >= normal && desired <= medium) {
                if (auto w = above(desired, medium)) {
                    return w;
                }
                if (auto w = below(desired)) {
                    return w;
                }
                return above(medium, INT32_MAX);
            }
            if (desired < normal) {
                if (auto w = below(desired)) {
                    return w;
                }
                return above(desired, INT32_MAX);
            }
            if (auto w = above(desired, INT32_MAX)) {
                return w;
            }
            return below(desired);
        }

        // The family's faces nearest to weight and style: the first such face, or, when the faces have unicode-ranges,
        // all of them, the last listed first.
        std::vector<const FontFaceEntry*> match_family(const FontCatalog& catalog, std::string_view family, int weight,
                                                       bool italic) {
            std::vector<const FontFaceEntry*> out;
            for (const bool style : {italic, !italic}) {
                std::vector<std::uint16_t> weights;
                for (const auto& face : catalog.faces) {
                    if (face.italic == style && same_family(face.family, family)) {
                        weights.push_back(face.weight);
                    }
                }
                if (const auto w = nearest_weight(weights, weight)) {
                    for (const auto& face : catalog.faces) {
                        if (face.italic == style && face.weight == *w && same_family(face.family, family)) {
                            out.push_back(&face);
                        }
                    }
                    const bool composite = std::any_of(out.begin(), out.end(), [](const FontFaceEntry* f) {
                        return !f->unicode_range.empty();
                    });
                    if (!composite) {
                        out.resize(1);
                    }
                    std::reverse(out.begin(), out.end());
                    return out;
                }
            }
            return out;
        }

        bool generic(std::string_view family) {
            for (const auto* keyword :
                 {"serif", "sans-serif", "monospace", "cursive", "fantasy", "system-ui", "ui-serif", "ui-sans-serif",
                  "ui-monospace", "ui-rounded", "emoji", "math", "fangsong"}) {
                if (family == keyword) {
                    return true;
                }
            }
            return false;
        }
    } // namespace

    std::vector<std::string> parse_font_families(std::string_view value) {
        std::vector<std::string> out;
        std::string current;
        char quote{};
        bool quoted{};
        auto finish = [&] {
            auto name = quoted ? current : trim(current);
            if (!quoted) {
                // Unquoted identifiers collapse internal whitespace; generic keywords are case-insensitive.
                std::string collapsed;
                for (char c : name) {
                    if (std::isspace(static_cast<unsigned char>(c))) {
                        if (!collapsed.empty() && collapsed.back() != ' ') {
                            collapsed += ' ';
                        }
                    } else {
                        collapsed += c;
                    }
                }
                name = collapsed;
                if (generic(lower(name))) {
                    name = lower(name);
                }
            }
            if (!name.empty()) {
                out.push_back(std::move(name));
            }
            current.clear();
            quoted = false;
        };
        for (char c : value) {
            if (quote) {
                if (c == quote) {
                    quote = 0;
                } else {
                    current += c;
                }
            } else if (c == '"' || c == '\'') {
                quote = c;
                quoted = true;
                current.clear();
            } else if (c == ',') {
                finish();
            } else if (!quoted) {
                current += c;
            }
        }
        finish();
        return out;
    }

    std::vector<std::string> font_family_names(const FontFaceSource& source) {
        FT_Library library{};
        if (FT_Init_FreeType(&library)) {
            throw std::runtime_error("WEFT_FONT_FILE: FreeType initialization failed");
        }
        FT_Face face{};

        struct Release {
            FT_Library library;
            FT_Face& face;

            ~Release() {
                if (face) {
                    FT_Done_Face(face);
                }
                FT_Done_FreeType(library);
            }
        } release{library, face};

        // FreeType reads only the tables it needs from the path, not the whole file.
        if (FT_New_Face(library, source.path.string().c_str(), FT_Long(source.face_index), &face)) {
            throw std::runtime_error("WEFT_FONT_FILE: FreeType rejected font face");
        }
        auto names = [&](FT_UShort id) {
            std::vector<std::string> out;
            const auto count = FT_Get_Sfnt_Name_Count(face);
            for (FT_UInt i = 0; i < count; ++i) {
                FT_SfntName name{};
                if (FT_Get_Sfnt_Name(face, i, &name) || name.name_id != id || !name.string_len) {
                    continue;
                }
                std::optional<std::string> value;
                if (name.platform_id == TT_PLATFORM_APPLE_UNICODE ||
                    (name.platform_id == TT_PLATFORM_MICROSOFT &&
                     (name.encoding_id == TT_MS_ID_UNICODE_CS || name.encoding_id == TT_MS_ID_UCS_4))) {
                    value = utf16be(name.string, name.string_len);
                } else if (name.platform_id == TT_PLATFORM_MACINTOSH && name.encoding_id == TT_MAC_ID_ROMAN &&
                           std::all_of(name.string, name.string + name.string_len, [](FT_Byte b) {
                               return b < 0x80;
                           })) {
                    value = std::string(reinterpret_cast<const char*>(name.string), name.string_len);
                }
                if (value && !value->empty() && std::find(out.begin(), out.end(), *value) == out.end()) {
                    out.push_back(std::move(*value));
                }
            }
            return out;
        };
        auto out = names(TT_NAME_ID_TYPOGRAPHIC_FAMILY);
        if (out.empty()) {
            out = names(TT_NAME_ID_FONT_FAMILY);
        }
        if (out.empty() && face->family_name) {
            out.emplace_back(face->family_name);
        }
        return out;
    }

    std::optional<std::vector<UnicodeRange>> parse_unicode_range(std::string_view value) {
        std::vector<UnicodeRange> out;
        std::size_t at{};
        const auto space = [&] {
            while (at < value.size() && (value[at] == ' ' || value[at] == '\t' || value[at] == '\n' ||
                                         value[at] == '\r' || value[at] == '\f')) {
                ++at;
            }
        };
        const auto hex = [](char c) -> int {
            return c >= '0' && c <= '9'   ? c - '0'
                   : c >= 'a' && c <= 'f' ? c - 'a' + 10
                   : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                          : -1;
        };
        // Up to six hex digits; with wild, '?' digits after them (first takes 0 for each, last F).
        const auto digits = [&](bool wild, std::uint32_t& first, std::uint32_t& last) {
            unsigned n{};
            unsigned wildcards{};
            first = last = 0;
            while (at < value.size() && n < 6) {
                const int d = hex(value[at]);
                if (d >= 0 && !wildcards) {
                    first = first * 16 + std::uint32_t(d);
                    last = last * 16 + std::uint32_t(d);
                } else if (wild && value[at] == '?') {
                    first *= 16;
                    last = last * 16 + 15;
                    ++wildcards;
                } else {
                    break;
                }
                ++at;
                ++n;
            }
            return n > 0;
        };
        while (true) {
            space();
            if (at + 2 > value.size() || (value[at] != 'U' && value[at] != 'u') || value[at + 1] != '+') {
                return std::nullopt;
            }
            at += 2;
            std::uint32_t first{};
            std::uint32_t last{};
            if (!digits(true, first, last)) {
                return std::nullopt;
            }
            if (first == last && at < value.size() && value[at] == '-') {
                ++at;
                std::uint32_t end{};
                std::uint32_t same{};
                if (!digits(false, end, same)) {
                    return std::nullopt;
                }
                last = end;
            }
            if (first > last || last > 0x10FFFF) {
                return std::nullopt;
            }
            out.push_back({char32_t(first), char32_t(last)});
            space();
            if (at == value.size()) {
                return out;
            }
            if (value[at] != ',') {
                return std::nullopt;
            }
            ++at;
        }
    }

    std::span<const std::pair<std::uint32_t, char32_t>> script_fallback_samples() {
        static constexpr std::pair<std::uint32_t, char32_t> samples[]{
            {script_tag("Arab"), U'\u0628'}, {script_tag("Hebr"), U'\u05D0'}, {script_tag("Syrc"), U'\u0710'},
            {script_tag("Thaa"), U'\u0780'}, {script_tag("Nkoo"), U'\u07CA'}, {script_tag("Armn"), U'\u0531'},
            {script_tag("Deva"), U'\u0915'}, {script_tag("Beng"), U'\u0995'}, {script_tag("Guru"), U'\u0A15'},
            {script_tag("Gujr"), U'\u0A95'}, {script_tag("Orya"), U'\u0B15'}, {script_tag("Taml"), U'\u0B95'},
            {script_tag("Telu"), U'\u0C15'}, {script_tag("Knda"), U'\u0C95'}, {script_tag("Mlym"), U'\u0D15'},
            {script_tag("Sinh"), U'\u0D9A'}, {script_tag("Thai"), U'\u0E01'}, {script_tag("Laoo"), U'\u0E81'},
            {script_tag("Tibt"), U'\u0F40'}, {script_tag("Mymr"), U'\u1000'}, {script_tag("Geor"), U'\u10D0'},
            {script_tag("Ethi"), U'\u1200'}, {script_tag("Cher"), U'\u13A0'}, {script_tag("Khmr"), U'\u1780'},
            {script_tag("Mong"), U'\u1820'},
        };
        return samples;
    }

    std::vector<FontFaceSource> match_font_faces(const FontCatalog& catalog, std::string_view families, int weight,
                                                 bool italic, std::size_t* listed,
                                                 std::vector<std::vector<UnicodeRange>>* ranges, std::uint32_t script) {
        std::vector<FontFaceSource> out;
        std::vector<std::vector<UnicodeRange>> out_ranges;
        auto add = [&](std::string_view family) {
            for (const auto* face : match_family(catalog, family, weight, italic)) {
                // A face appears once (with its first unicode-range).
                if (std::find(out.begin(), out.end(), face->source) == out.end()) {
                    out.push_back(face->source);
                    out_ranges.push_back(face->unicode_range);
                }
            }
        };
        for (const auto& family : parse_font_families(families)) {
            bool mapped{};
            for (const auto& [keyword, name] : catalog.generic_families) {
                if (keyword == family) {
                    add(name);
                    mapped = true;
                }
            }
            if (!mapped && !generic(family)) {
                add(family);
            }
        }
        if (listed) {
            *listed = out.size();
        }
        if (script) {
            for (const auto& [tag, script_families] : catalog.script_fallbacks) {
                if (tag == script) {
                    for (const auto& family : script_families) {
                        add(family);
                    }
                }
            }
        }
        for (const auto& family : catalog.fallback_families) {
            add(family);
        }
        if (ranges) {
            *ranges = std::move(out_ranges);
        }
        return out;
    }
} // namespace weft
