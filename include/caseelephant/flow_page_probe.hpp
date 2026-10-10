#pragma once
#include "caseelephant/positioned_page_probe.hpp"
#include "caseelephant/inline_probe.hpp"
namespace caseelephant {
struct ProbeFlowPage {
    ProbePositionedPage page;
    std::vector<ProbeElementGeometry> elements;
};
// Common inline text/spans/br within zero-margin block/flex formatting, positive
// LTR scroll areas with no occupied scrollbar space (no scrollbar paint yet),
// and owning rounded overflow clips.
// A distinct M0 profile preserves the earlier auto/positioned evidence paths.
ProbeFlowPage build_flow_page_probe(const ProbeDomSnapshot&,const ProbeComputedStyle&,
    float viewport_width,float viewport_height,TextProbeFontSet&);
ProbeFlowPage build_flow_page_probe(const ProbeDomSnapshot&,std::string_view css,
    float viewport_width,float viewport_height,TextProbeFontSet&);
}
