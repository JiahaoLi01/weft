#include "dom/text_field.hpp"

#include <algorithm>
#include <cctype>

namespace weft {
    namespace {
        // The bytes of the UTF-8 sequence that starts with b (1 for a stray continuation byte).
        std::size_t sequence(unsigned char b) {
            return b < 0x80 ? 1 : b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
        }
    } // namespace

    std::string text_field_type(const std::string* type) {
        if (!type) {
            return "text";
        }
        std::string lower(*type);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return char(std::tolower(c));
        });
        for (const auto* known : {"text", "search", "password", "email", "url", "tel", "number", "button", "submit",
                                  "reset", "image", "checkbox", "radio", "range", "color", "file", "hidden", "date",
                                  "datetime-local", "month", "week", "time"}) {
            if (lower == known) {
                return lower;
            }
        }
        return "text";
    }

    bool text_field_element(std::string_view local, bool html, const std::string* type) {
        if (!html) {
            return false;
        }
        if (local == "textarea") {
            return true;
        }
        if (local != "input") {
            return false;
        }
        const auto kind = text_field_type(type);
        return kind == "text" || kind == "search" || kind == "password" || kind == "email" || kind == "url" ||
               kind == "tel" || kind == "number";
    }

    std::size_t utf16_length(std::string_view utf8) {
        return utf16_offset(utf8, utf8.size());
    }

    std::size_t utf16_offset(std::string_view utf8, std::size_t byte) {
        std::size_t units = 0;
        for (std::size_t i = 0; i < utf8.size() && i < byte;) {
            const auto n = sequence(static_cast<unsigned char>(utf8[i]));
            units += n == 4 ? 2 : 1;
            i += n;
        }
        return units;
    }

    std::size_t byte_offset(std::string_view utf8, std::size_t utf16) {
        std::size_t units = 0;
        std::size_t i = 0;
        while (i < utf8.size() && units < utf16) {
            const auto n = sequence(static_cast<unsigned char>(utf8[i]));
            units += n == 4 ? 2 : 1;
            i = std::min(utf8.size(), i + n);
        }
        return i;
    }
} // namespace weft
