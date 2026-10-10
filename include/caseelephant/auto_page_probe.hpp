#pragma once
#include "caseelephant/positioned_page_probe.hpp"

namespace caseelephant {
    // Experimental full-recompute formatter for explicit block/flex/none boxes.
    // Zero margins/borders, px padding, leaf text; no general inline formatting.
    // Render output is owned and survives destruction/reset of the input DOM/VM.
    ProbePositionedPage build_auto_page_probe(const ProbeDomSnapshot& root, const ProbeComputedStyle& style,
                                              float viewport_width, float viewport_height, TextProbeFontSet& fonts);
    ProbePositionedPage build_auto_page_probe(const ProbeDomSnapshot& root, std::string_view stylesheet,
                                              float viewport_width, float viewport_height, TextProbeFontSet& fonts);
} // namespace caseelephant
