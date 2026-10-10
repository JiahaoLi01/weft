#pragma once
#include "layout/positioned_page.hpp"
#include "layout/inline_layout.hpp"

namespace weft {
    struct FlowPage {
        PositionedPage page;
        std::vector<ElementGeometry> elements;
    };

    // Common inline text/spans/br within zero-margin block/flex formatting, positive
    // LTR scroll areas with no occupied scrollbar space (no scrollbar paint yet),
    // and owning rounded overflow clips.
    // A distinct M0 profile preserves the earlier auto/positioned evidence paths.
    FlowPage build_flow_page(const DomSnapshot&, const ComputedStyle&, float viewport_width, float viewport_height,
                             FontSet&);
    FlowPage build_flow_page(const DomSnapshot&, std::string_view css, float viewport_width, float viewport_height,
                             FontSet&);
} // namespace weft
