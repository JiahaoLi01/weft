#pragma once
#include "caseelephant/text_probe.hpp"
#include "caseelephant/inline_probe.hpp"
#include <map>
#include <string>
#include <vector>

namespace caseelephant {
    struct FlexProbeNode {
        std::string id;
        std::map<std::string, std::string> style;
        std::string text;
        std::vector<FlexProbeNode> children;
        std::shared_ptr<const ProbeInlineInput> inline_content;
    };

    struct FlexProbeBox {
        std::string id;
        float x{};
        float y{};
        float width{};
        float height{};
        float text_width{};
        float text_height{};
        float baseline{};
        std::size_t text_lines{};
        std::size_t measure_calls{};
    };
    enum class FlexProbeMode {
        RawYoga,
        CssAdapter,
        RestrictedBlockAdapter
    };
    // Pure M0 input/output: no DOM, VM values, or Yoga nodes escape this boundary.
    // Unsupported formatting contexts/properties/units fail explicitly.
    std::vector<FlexProbeBox> run_flex_probe(const FlexProbeNode& root, TextProbeFont& font,
                                             FlexProbeMode mode = FlexProbeMode::CssAdapter);
    std::vector<FlexProbeBox> run_flex_probe(const FlexProbeNode& root, TextProbeFontSet& fonts,
                                             FlexProbeMode mode = FlexProbeMode::RestrictedBlockAdapter);

    struct ProbeTextLine {
        std::string text;
        ProbeShapedText shaped;
    };

    // Shared intrinsic/final measurement and paint. Non-ASCII is nowrap only;
    // ASCII normal wraps at spaces. No Unicode line breaking is implemented here.
    std::vector<ProbeTextLine> layout_probe_text(std::string_view text, float size, float available, bool nowrap,
                                                 TextProbeFont& font, TextProbeFontSet* fonts = nullptr);
} // namespace caseelephant
