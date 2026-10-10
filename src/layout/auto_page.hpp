#pragma once
#include "layout/positioned_page.hpp"

namespace weft {
    // Experimental full-recompute formatter for explicit block/flex/none boxes.
    // Zero margins/borders, px padding, leaf text; no general inline formatting.
    // Render output is owned and survives destruction/reset of the input DOM/VM.
    PositionedPage build_auto_page(const DomSnapshot& root, const ComputedStyle& style, float viewport_width,
                                   float viewport_height, FontSet& fonts);
    PositionedPage build_auto_page(const DomSnapshot& root, std::string_view stylesheet, float viewport_width,
                                   float viewport_height, FontSet& fonts);
} // namespace weft
