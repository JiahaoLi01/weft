#pragma once
#include "layout/output.hpp"
#include "style/style.hpp"

namespace weft {
    // Experimental inline-style/flex/single-line HTML profile. Explicit diagnostics
    // for unsupported formatting and paint. Uses shared bounded style cascade.
    Page build_page(const DomSnapshot& root, Font& font);
    Page build_page(const DomSnapshot& root, std::string_view stylesheet, Font& font);
} // namespace weft
