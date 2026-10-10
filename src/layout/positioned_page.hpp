#pragma once
#include "layout/output.hpp"
#include "style/style.hpp"
#include <string_view>

namespace weft {
    // Shared bounded author style evaluator; definite-size formatting remains an
    // independent adapter. A paired style subtree can inherit document :root vars.
    PositionedPage build_positioned_page(const DomSnapshot& root, std::string_view stylesheet, float viewport_width,
                                         float viewport_height, Font& font);
    PositionedPage build_positioned_page(const DomSnapshot& root, std::string_view stylesheet, float viewport_width,
                                         float viewport_height, FontSet& fonts);
    PositionedPage build_positioned_page(const DomSnapshot& root, const ComputedStyle& style, float viewport_width,
                                         float viewport_height, Font& font);
    PositionedPage build_positioned_page(const DomSnapshot& root, const ComputedStyle& style, float viewport_width,
                                         float viewport_height, FontSet& fonts);
} // namespace weft
