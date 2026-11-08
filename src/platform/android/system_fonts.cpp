#include "platform/android/system_fonts.hpp"

#include <android/font.h>
#include <android/system_fonts.h>

#include <algorithm>
#include <cctype>
#include <string_view>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace weft::platform::android {
    FontCatalog system_font_catalog() {
        FontCatalog catalog;
        catalog.generic_families = {{"serif", "Noto Serif"},
                                    {"sans-serif", "Roboto"},
                                    {"monospace", "Droid Sans Mono"},
                                    {"system-ui", "Roboto"}};
        std::vector<std::string> order;
        std::vector<std::pair<std::uint32_t, std::vector<std::string>>> scripts;
        auto* iterator = ASystemFontIterator_open();
        if (!iterator) {
            throw std::runtime_error("WEFT_SYSTEM_FONTS: ASystemFontIterator_open failed");
        }

        struct Close {
            ASystemFontIterator* iterator;

            ~Close() {
                ASystemFontIterator_close(iterator);
            }
        } close{iterator};

        while (AFont* font = ASystemFontIterator_next(iterator)) {
            struct Release {
                AFont* font;

                ~Release() {
                    AFont_close(font);
                }
            } release{font};

            FontFaceSource source;
            source.path = AFont_getFontFilePath(font);
            source.face_index = std::uint32_t(AFont_getCollectionIndex(font));
            for (std::size_t axis = 0; axis < AFont_getAxisCount(font); ++axis) {
                source.variations.push_back(
                    {AFont_getAxisTag(font, std::uint32_t(axis)), AFont_getAxisValue(font, std::uint32_t(axis))});
            }
            std::vector<std::string> families;
            try {
                families = font_family_names(source);
            } catch (const std::runtime_error&) {
                continue;
            }
            // fonts.xml tags a script's fallback font with its locale ("und-Arab", "ja-Jpan"): its script subtag.
            std::uint32_t script{};
            if (const char* locale = AFont_getLocale(font)) {
                const std::string_view tags(locale);
                for (std::size_t at = 0; at <= tags.size();) {
                    const auto end = std::min(tags.find_first_of("-,", at), tags.size());
                    if (end - at == 4 && std::isupper(static_cast<unsigned char>(tags[at]))) {
                        script = std::uint32_t(std::uint8_t(tags[at])) << 24 |
                                 std::uint32_t(std::uint8_t(tags[at + 1])) << 16 |
                                 std::uint32_t(std::uint8_t(tags[at + 2])) << 8 | std::uint8_t(tags[at + 3]);
                        break;
                    }
                    at = end + 1;
                }
            }
            for (auto& family : families) {
                if (std::find(order.begin(), order.end(), family) == order.end()) {
                    order.push_back(family);
                }
                if (script) {
                    auto entry = std::find_if(scripts.begin(), scripts.end(), [&](const auto& s) {
                        return s.first == script;
                    });
                    if (entry == scripts.end()) {
                        scripts.push_back({script, {}});
                        entry = scripts.end() - 1;
                    }
                    if (std::find(entry->second.begin(), entry->second.end(), family) == entry->second.end()) {
                        entry->second.push_back(family);
                    }
                }
                catalog.faces.push_back({std::move(family), AFont_getWeight(font), AFont_isItalic(font), source});
            }
        }
        for (const auto& family : order) {
            const bool generic =
                std::any_of(catalog.generic_families.begin(), catalog.generic_families.end(), [&](const auto& g) {
                    return g.second == family;
                });
            if (!generic) {
                catalog.fallback_families.push_back(family);
            }
        }
        catalog.script_fallbacks = std::move(scripts);
        return catalog;
    }
} // namespace weft::platform::android
