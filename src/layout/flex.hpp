#pragma once
#include "base/function_ref.hpp"
#include "constants/common.hpp"
#include "layout/inline_layout.hpp"
#include "style/values.hpp"
#include "text/font.hpp"
#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The Yoga adapter, shared by the document formatter and the scene: one mapping from computed CSS to boxes.
// Block, flex and absolute boxes go through Yoga, grid containers through CSS Grid track sizing (layout/grid.hpp)
// with each item laid out by Yoga in its area; text is measured by the formatter's text system. Pure input and output: no DOM, script values or Yoga nodes cross this boundary. Unsupported
// formatting contexts, properties and units fail explicitly.
namespace weft {
    // A text leaf as the formatter's text system lays it out.
    struct TextMetrics {
        float width{};
        float height{};
        // Distance from the top of the first line box to its baseline.
        float baseline{};
        std::size_t lines{};
    };

    // The formatter's text system, for the boxes marked as text. A box is named by its index.
    struct TextMeasurer {
        // The text in lines at most `available` wide; infinity lays out unconstrained lines.
        FunctionRef<TextMetrics(std::size_t box, float available)> layout;
        // The narrowest width the text takes without overflowing: its widest unbreakable run.
        FunctionRef<float(std::size_t box)> min_content;
        // Whether a narrower width breaks the text into more lines, so that its height depends on its width.
        FunctionRef<bool(std::size_t box)> wraps;
    };

    // One box of the formatting tree. Boxes are in tree order: a parent precedes its children, which follow in
    // order. The formatter owns the styles and texts for the duration of the call.
    struct LayoutBox {
        static constexpr std::size_t root = std::numeric_limits<std::size_t>::max();

        const StyleValues* style{};
        std::size_t parent{root};
        // A leaf whose content the TextMeasurer lays out.
        bool text{};
        // A replaced element (an outer svg element). Its width and height attributes stand in for absent width
        // and height properties, with unitless numbers in px. ratio is the intrinsic aspect ratio (viewBox width
        // over height): 0 for none, NaN for an invalid viewBox, which is a diagnostic only when the ratio is used.
        bool replaced{};
        std::string_view replaced_width;
        std::string_view replaced_height;
        float ratio{};
        // A replaced element of a natural size (an img) rather than an svg: sized as CSS 2 §10.3.2, §10.4 and §10.6.2
        // size one, by its width, height, minimum and maximum sizes, aspect-ratio and its natural size (px; none
        // without an image: 0 x 0 then), whose ratio is `ratio`. Its width and height attributes are in its style.
        bool image{};
        std::optional<std::array<float, 2>> natural;
    };

    struct BoxGeometry {
        // Border box relative to the parent's border box; the root's is relative to the layout origin.
        float left{};
        float top{};
        float width{};
        float height{};
        // Border box relative to the layout origin.
        float x{};
        float y{};
        // The text laid out at the final content width (LayoutOptions::final_text).
        TextMetrics text;
        std::size_t measure_calls{};
    };

    enum class FlexMode {
        // Yoga with web defaults, for comparison: flex containers only, no static position, content-box or
        // automatic minimum mapping.
        RawYoga,
        CssAdapter,
    };

    struct LayoutOptions {
        FlexMode mode = FlexMode::CssAdapter;
        // The root's border-box size as a viewport gives it (the scene root); unset, the root sizes itself.
        std::optional<std::array<float, 2>> viewport;
        // Lay out each text box again at its final content width (BoxGeometry::text).
        bool final_text{};
        // Without a viewport: the width of the root's containing block, as an atomic inline has one. Percentages
        // resolve against it, and a root of auto width shrinks to fit it less its margins (its max-content width, but
        // not narrower than its min-content width); a replaced root keeps its own size. Unset, the root takes its
        // max-content width.
        std::optional<float> fit_width;
    };

    // One geometry per box, in input order.
    std::vector<BoxGeometry> layout_boxes(std::span<const LayoutBox> boxes, const TextMeasurer& text,
                                          const LayoutOptions& options = {});

    // The root's border-box min-content and max-content widths (CSS Sizing 3 §5), as layout_boxes sizes a root that
    // shrinks to fit: from its content, its padding and borders, each box in it contributing its px width where it
    // states one, within its px min-width and max-width (§5.1). A px width of the root's own is both, as the root
    // would contribute to a container (a table cell's sizing passes a root of auto width). Percentages in the content
    // resolve against nothing, as intrinsic sizes take them.
    struct IntrinsicWidths {
        float min{};
        float max{};
    };

    IntrinsicWidths layout_intrinsic_widths(std::span<const LayoutBox> boxes, const TextMeasurer& text);

    // A box tree that owns its styles and text, laid out with the document text system: the flex fixtures, the
    // probes and the document formatter. Font size and line height (px) are inherited down the tree.
    struct FlexNode {
        std::string id;
        StyleValues style;
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

    // Boxes in tree order.
    std::vector<FlexBox> run_flex(const FlexNode& root, Font& font, FlexMode mode = FlexMode::CssAdapter);
    std::vector<FlexBox> run_flex(const FlexNode& root, FontSet& fonts, FlexMode mode = FlexMode::CssAdapter);

    struct TextLine {
        std::string text;
        ShapedText shaped;
    };

    // Shared intrinsic/final measurement and paint. Non-ASCII is nowrap only;
    // ASCII normal wraps at spaces. No Unicode line breaking is implemented here.
    std::vector<TextLine> layout_text(std::string_view text, float size, float available, bool nowrap, Font& font,
                                      FontSet* fonts = nullptr);
} // namespace weft
