#pragma once
#include "caseelephant/text_probe.hpp"
#include "caseelephant/inline_probe.hpp"
#include <map>
#include <string>
#include <vector>

namespace weft {
    struct FlexNode {
        std::string id;
        std::map<std::string, std::string> style;
        std::string text;
        std::vector<FlexNode> children;
        std::shared_ptr<const InlineInput> inline_content;
    };

    struct FlexBox {
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
    enum class FlexMode {
        RawYoga,
        CssAdapter,
        RestrictedBlockAdapter
    };
    // Pure M0 input/output: no DOM, VM values, or Yoga nodes escape this boundary.
    // Unsupported formatting contexts/properties/units fail explicitly.
    std::vector<FlexBox> run_flex(const FlexNode& root, Font& font, FlexMode mode = FlexMode::CssAdapter);
    std::vector<FlexBox> run_flex(const FlexNode& root, FontSet& fonts,
                                  FlexMode mode = FlexMode::RestrictedBlockAdapter);

    struct TextLine {
        std::string text;
        ShapedText shaped;
    };

    // Shared intrinsic/final measurement and paint. Non-ASCII is nowrap only;
    // ASCII normal wraps at spaces. No Unicode line breaking is implemented here.
    std::vector<TextLine> layout_text(std::string_view text, float size, float available, bool nowrap, Font& font,
                                      FontSet* fonts = nullptr);
} // namespace weft
