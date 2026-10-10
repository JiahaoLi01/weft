#pragma once

#include <string>
#include <string_view>

namespace weft::testing {
    // JSON string literal for report and trace output: quotes, backslashes and control characters
    // are escaped; other bytes (UTF-8) pass through.
    inline std::string json_string(std::string_view value) {
        constexpr char hex[] = "0123456789abcdef";
        std::string out = "\"";
        for (const unsigned char c : value) {
            if (c == '"' || c == '\\') {
                out += '\\';
                out += static_cast<char>(c);
            } else if (c < 0x20) {
                out += "\\u00";
                out += hex[c >> 4];
                out += hex[c & 0xf];
            } else {
                out += static_cast<char>(c);
            }
        }
        return out + '"';
    }
} // namespace weft::testing
