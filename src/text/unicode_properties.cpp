#include "text/unicode_properties.hpp"

#include <libunicode.h>

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace weft {
    namespace {
        void* reallocate(void*, void* p, std::size_t size) {
            if (!size) {
                std::free(p);
                return nullptr;
            }
            return std::realloc(p, size);
        }

        // A property's characters as sorted half-open ranges [start, end).
        std::vector<std::uint32_t> ranges(const char* property) {
            CharRange cr;
            cr_init(&cr, nullptr, reallocate);
            if (unicode_prop(&cr, property) != 0) {
                cr_free(&cr);
                throw std::runtime_error(std::string("WEFT_UNICODE_PROPERTY: ") + property);
            }
            std::vector<std::uint32_t> out(cr.points, cr.points + cr.len);
            cr_free(&cr);
            return out;
        }

        bool contains(const std::vector<std::uint32_t>& points, char32_t c) {
            // The number of range boundaries at or below c is odd inside a range.
            const auto at = std::upper_bound(points.begin(), points.end(), std::uint32_t(c));
            return (at - points.begin()) % 2 == 1;
        }
    } // namespace

    EmojiPresentation emoji_presentation(char32_t c) {
        static const auto emoji = ranges("Emoji");
        static const auto presentation = ranges("Emoji_Presentation");
        if (!contains(emoji, c)) {
            return EmojiPresentation::None;
        }
        return contains(presentation, c) ? EmojiPresentation::Emoji : EmojiPresentation::Text;
    }
} // namespace weft
