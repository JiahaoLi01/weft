#pragma once
#include "layout/inline_layout.hpp"
#include "layout/output.hpp"
#include "style/style.hpp"

namespace weft {
    // The formatted document: boxes, paint items and hit regions in page, and element geometry for script reads.
    struct DocumentLayout {
        PositionedPage page;
        std::vector<ElementGeometry> elements;
    };

    // The document formatter: block, flex and absolute boxes (Yoga), inline text with spans and br in Blink line
    // boxes, zero-margin block flow, positive LTR scroll areas without occupied scrollbar space (no scrollbar paint
    // yet) and owning rounded overflow clips. Anything outside this subset is a Diag::DocumentProfile diagnostic.
    DocumentLayout layout_document(const DomSnapshot&, const ComputedStyle&, float viewport_width,
                                   float viewport_height, FontSet&);
    DocumentLayout layout_document(const DomSnapshot&, std::string_view css, float viewport_width,
                                   float viewport_height, FontSet&);
} // namespace weft
