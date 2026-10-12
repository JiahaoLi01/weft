#include "platform/android/system_fonts.hpp"

#include <android/font.h>
#include <android/system_fonts.h>

#include <algorithm>
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
            for (auto& family : families) {
                if (std::find(order.begin(), order.end(), family) == order.end()) {
                    order.push_back(family);
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
        return catalog;
    }
} // namespace weft::platform::android
