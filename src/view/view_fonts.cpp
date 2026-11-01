#include "view/view_fonts.hpp"

#include "constants/text.hpp"
#include "text/font.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iterator>
#include <memory>
#include <optional>

namespace weft {
    namespace {
        // The format() hints of files weft reads.
        bool readable_format(std::string_view format) {
            return format == "truetype" || format == "opentype" || format == "truetype-variations" ||
                   format == "opentype-variations";
        }

        // What a font file's first four bytes say it is.
        enum class FileKind : std::uint8_t {
            Sfnt,
            Collection,
            Woff,
            Woff2,
            Other,
        };

        FileKind file_kind(const std::vector<char>& bytes) {
            if (bytes.size() < 4) {
                return FileKind::Other;
            }
            const std::string_view tag(bytes.data(), 4);
            if (tag == std::string_view("\0\1\0\0", 4) || tag == "true" || tag == "OTTO") {
                return FileKind::Sfnt;
            }
            return tag == "ttcf"   ? FileKind::Collection
                   : tag == "wOFF" ? FileKind::Woff
                   : tag == "wOF2" ? FileKind::Woff2
                                   : FileKind::Other;
        }

        bool same_family(std::string_view a, std::string_view b) {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                       const auto lower = [](char c) {
                           return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
                       };
                       return lower(x) == lower(y);
                   });
        }
    } // namespace

    std::vector<FontFaceEntry> view_font_faces(const std::vector<FontFaceRule>& rules, std::string_view base,
                                               const ViewResourceReader& reader,
                                               const std::function<void(std::string)>& diagnose) {
        std::vector<FontFaceEntry> out;
        for (const auto& rule : rules) {
            std::string reasons;
            const auto pass = [&](std::string reason) {
                reasons += (reasons.empty() ? "" : "; ") + std::move(reason);
            };
            std::optional<FontFaceSource> found;
            for (const auto& source : rule.sources) {
                if (!source.local.empty()) {
                    pass("local(" + source.local + "): installed fonts are not looked up by name");
                    continue;
                }
                if (!source.formats.empty() &&
                    std::none_of(source.formats.begin(), source.formats.end(), readable_format)) {
                    pass("url(" + source.url + "): format " + source.formats.front() + " is not read");
                    continue;
                }
                const auto path = view_resolve_url(base, source.url);
                auto file = reader ? reader(path) : std::nullopt;
                if (!file) {
                    pass("url(" + path + "): no such file");
                    continue;
                }
                if (file->empty() || file->size() > static_cast<std::size_t>(constants::text::max_font_file_bytes)) {
                    pass("url(" + path + "): no font file of a size weft reads");
                    continue;
                }
                auto data = std::make_shared<const std::vector<char>>(file->begin(), file->end());
                const auto kind = file_kind(*data);
                if (kind == FileKind::Woff || kind == FileKind::Woff2) {
                    pass("url(" + path + "): a " + (kind == FileKind::Woff ? "WOFF" : "WOFF2") + " file is not read");
                    continue;
                }
                if (kind == FileKind::Other) {
                    pass("url(" + path + "): no TrueType or OpenType font");
                    continue;
                }
                FontFaceSource face{path, 0, {}, std::move(data)};
                try {
                    const Font probe(face);
                } catch (const std::exception& e) {
                    pass("url(" + path + "): " + e.what());
                    continue;
                }
                found = std::move(face);
                break;
            }
            if (!found) {
                diagnose("WEFT_FONT_FACE: " + rule.family + ": " + (reasons.empty() ? "no source" : reasons));
                continue;
            }
            FontFaceEntry entry;
            entry.family = rule.family;
            entry.weight = std::uint16_t(rule.weight);
            entry.italic = rule.italic;
            entry.source = std::move(*found);
            if (!rule.unicode_range.empty()) {
                if (auto ranges = parse_unicode_range(rule.unicode_range)) {
                    entry.unicode_range = std::move(*ranges);
                } else {
                    diagnose("WEFT_FONT_FACE: " + rule.family + ": unicode-range " + rule.unicode_range +
                             " does not parse");
                }
            }
            out.push_back(std::move(entry));
        }
        return out;
    }

    FontCatalog view_page_catalog(const FontCatalog& runtime, std::vector<FontFaceEntry> page) {
        FontCatalog out = runtime;
        std::erase_if(out.faces, [&](const FontFaceEntry& e) {
            return std::any_of(page.begin(), page.end(), [&](const FontFaceEntry& p) {
                return same_family(e.family, p.family);
            });
        });
        out.faces.insert(out.faces.end(), std::make_move_iterator(page.begin()), std::make_move_iterator(page.end()));
        return out;
    }
} // namespace weft
