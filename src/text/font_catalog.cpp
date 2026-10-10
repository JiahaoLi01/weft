#include "text/font_catalog.hpp"

#include <algorithm>
#include <cctype>
#include <optional>

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

        std::optional<FontFaceSource> match_family(const FontCatalog& catalog, std::string_view family, int weight,
                                                   bool italic) {
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
                            return face.source;
                        }
                    }
                }
            }
            return std::nullopt;
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

    std::vector<FontFaceSource> match_font_faces(const FontCatalog& catalog, std::string_view families, int weight,
                                                 bool italic) {
        std::vector<FontFaceSource> out;
        auto add = [&](std::string_view family) {
            if (auto face = match_family(catalog, family, weight, italic)) {
                if (std::find(out.begin(), out.end(), *face) == out.end()) {
                    out.push_back(std::move(*face));
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
        for (const auto& family : catalog.fallback_families) {
            add(family);
        }
        return out;
    }
} // namespace weft
