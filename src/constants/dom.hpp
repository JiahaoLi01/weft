#pragma once

#include "constants/common.hpp"

// DOM values, the shared selector engine and attribute values (src/dom). Each constant has the type of the literal
// it replaced.
namespace weft::constants::dom {
    // CSS escapes take at most six hex digits.
    inline constexpr int max_escape_hex_digits = 6;
    // One past the last ASCII code point: an escaped selector code point must be below it.
    inline constexpr int ascii_end = 128;
    // Selectors in the selector list of one DOM query.
    inline constexpr int max_selector_list_items = 32;
    // An HTML canvas element's bitmap size when its width or height attribute is absent or invalid.
    inline constexpr int default_canvas_width = 300;
    inline constexpr int default_canvas_height = 150;
} // namespace weft::constants::dom
