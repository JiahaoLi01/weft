#include "layout/flex.hpp"
#include "constants/layout.hpp"
#include "support/check.hpp"
#include "test_constants/layout.hpp"
#include <algorithm>
#include <array>
#include <deque>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

// The shared Yoga adapter (layout_boxes) as both formatters call it, with a fixed-width text system. Each case is a
// CSS behavior the scene's former adapter did not have, or a diagnostic of the shared input contract.
namespace {
    using weft::testing::require;
    namespace limits = weft::test_constants::layout;
    constexpr auto none = weft::LayoutBox::root;

    struct Tree {
        std::deque<weft::StyleValues> styles;
        std::vector<weft::LayoutBox> boxes;
        std::vector<std::string> texts;
        // Text breaks between its words, separated by single spaces, where the width available ends.
        bool wrapping{};

        std::size_t add(std::size_t parent, weft::StyleValues style, std::string text = {}) {
            styles.push_back(std::move(style));
            weft::LayoutBox box;
            box.style = &styles.back();
            box.parent = parent;
            box.text = !text.empty();
            boxes.push_back(box);
            texts.push_back(std::move(text));
            return boxes.size() - 1;
        }

        std::size_t svg(std::size_t parent, weft::StyleValues style, std::string_view width, float ratio) {
            const auto i = add(parent, std::move(style));
            boxes[i].replaced = true;
            boxes[i].replaced_width = width;
            boxes[i].ratio = ratio;
            return i;
        }

        // The widths of a text's words.
        std::vector<float> words(std::size_t i) const {
            std::vector<float> out;
            std::size_t from{};
            while (from <= texts[i].size()) {
                const auto end = std::min(texts[i].find(' ', from), texts[i].size());
                out.push_back(float(end - from) * limits::box_text_char_width_px);
                from = end + 1;
            }
            return out;
        }

        std::vector<weft::BoxGeometry> layout(std::optional<std::array<float, 2>> viewport = std::nullopt) const {
            weft::LayoutOptions options;
            options.viewport = viewport;
            return layout(options);
        }

        std::vector<weft::BoxGeometry> layout(const weft::LayoutOptions& options) const;
        weft::IntrinsicWidths intrinsic_widths() const;

        // f with the tree's text system.
        template <class F>
        auto measured(F&& f) const {
            // Text never wraps unless it is wrapping text: one line, as wide as its characters.
            const auto lines = [&](std::size_t i, float available) {
                if (!wrapping) {
                    return weft::TextMetrics{float(texts[i].size()) * limits::box_text_char_width_px,
                                             limits::box_text_line_height_px, limits::box_text_baseline_px, 1};
                }
                float widest{};
                float line{};
                std::size_t count{};
                for (const float word : words(i)) {
                    if (count && line + limits::box_text_char_width_px + word <= available) {
                        line += limits::box_text_char_width_px + word;
                        continue;
                    }
                    widest = std::max(widest, line);
                    line = word;
                    ++count;
                }
                return weft::TextMetrics{std::max(widest, line), float(count) * limits::box_text_line_height_px,
                                         limits::box_text_baseline_px, count};
            };
            const auto min_content = [&](std::size_t i) {
                if (!wrapping) {
                    return float(texts[i].size()) * limits::box_text_char_width_px;
                }
                const auto w = words(i);
                return *std::max_element(w.begin(), w.end());
            };
            const auto wraps = [&](std::size_t) {
                return wrapping;
            };
            return f(weft::TextMeasurer{lines, min_content, wraps});
        }
    };

    std::vector<weft::BoxGeometry> Tree::layout(const weft::LayoutOptions& options) const {
        return measured([&](const weft::TextMeasurer& text) {
            return weft::layout_boxes(boxes, text, options);
        });
    }

    weft::IntrinsicWidths Tree::intrinsic_widths() const {
        return measured([&](const weft::TextMeasurer& text) {
            return weft::layout_intrinsic_widths(boxes, text);
        });
    }

    void rejected(const Tree& tree, const char* expected, const char* message) {
        bool matched{};
        try {
            (void)tree.layout();
        } catch (const std::exception& e) {
            matched = std::string(e.what()).find(expected) != std::string::npos;
        }
        require(matched, message);
    }

    // Block layout places children in a column; flex item properties do not apply there.
    void block_children_do_not_flex() {
        Tree t;
        const auto root = t.add(none, {{"display", "block"}, {"width", "200px"}, {"height", "100px"}});
        const auto item = t.add(root, {{"display", "block"},
                                       {"flex-grow", "1"},
                                       {"flex-basis", "0%"},
                                       {"align-self", "center"},
                                       {"height", "20px"}});
        const auto g = t.layout();
        require(g[item].width == 200 && g[item].height == 20 && g[item].left == 0 && g[item].top == 0,
                "block layout flexed or aligned its child");
    }

    // Flex items shrink by default, but not below their content (the automatic minimum size).
    void flex_items_shrink_to_their_content() {
        Tree t;
        const auto root = t.add(none, {{"display", "flex"}, {"width", "100px"}});
        const auto word = t.add(root, {{"display", "block"}}, "XXXXXXXX");
        const auto fixed = t.add(root, {{"display", "block"}, {"width", "60px"}});
        const auto g = t.layout();
        const float word_width = 8 * limits::box_text_char_width_px;
        require(g[word].width == word_width, "flex item shrank below its content");
        require(g[fixed].width == 100 - word_width && g[fixed].left == word_width,
                "an empty flex item did not shrink by default");
    }

    // A wrapping row in a column shorter than its content: its automatic minimum height is its lines at its width (two
    // here), not one line at its max-content width, so it does not shrink below them and what follows comes after.
    void wrapping_row_keeps_its_lines() {
        Tree t;
        const auto root =
            t.add(none, {{"display", "flex"}, {"flex-direction", "column"}, {"width", "100px"}, {"height", "40px"}});
        const auto row = t.add(root, {{"display", "flex"}, {"flex-wrap", "wrap"}});
        t.add(row, {{"display", "block"}, {"width", "60px"}, {"height", "30px"}});
        const auto second = t.add(row, {{"display", "block"}, {"width", "60px"}, {"height", "30px"}});
        const auto after = t.add(root, {{"display", "block"}, {"height", "10px"}, {"flex-shrink", "0"}});
        const auto g = t.layout();
        require(g[row].height == 60 && g[second].top == 30 && g[after].top == 60,
                "a wrapping row shrank below its lines in a short column");
    }

    // A text box is as tall as its lines even where less height is available.
    void text_overflows_available_height() {
        Tree t;
        const auto root =
            t.add(none, {{"display", "flex"}, {"flex-direction", "column"}, {"width", "100px"}, {"height", "10px"}});
        const auto text = t.add(root, {{"flex-shrink", "0"}, {"min-height", "0"}}, "XX");
        require(t.layout()[text].height == limits::box_text_line_height_px,
                "text height clamped to the space available");
    }

    // Equal columns; a display:none child takes no cell; gap applies to rows and columns.
    void grid_places_visible_children() {
        Tree t;
        const auto grid = t.add(none, {{"display", "grid"},
                                       {"grid-template-columns", "repeat(3, minmax(0, 1fr))"},
                                       {"gap", "10px"},
                                       {"width", "320px"}});
        const auto first = t.add(grid, {{"height", "20px"}});
        const auto hidden = t.add(grid, {{"display", "none"}, {"height", "20px"}});
        const auto second = t.add(grid, {{"height", "20px"}});
        const auto third = t.add(grid, {{"height", "40px"}});
        const auto fourth = t.add(grid, {{"height", "20px"}});
        const auto g = t.layout();
        // Columns of (320 - 2 gaps) / 3; the first row is as tall as its tallest cell.
        const auto at = [&](std::size_t i, float x, float y, float height) {
            return g[i].left == x && g[i].top == y && g[i].width == 100 && g[i].height == height;
        };
        require(at(first, 0, 0, 20) && at(second, 110, 0, 20) && at(third, 220, 0, 40) && at(fourth, 0, 50, 20),
                "grid cells misplaced");
        require(g[hidden].width == 0 && g[hidden].height == 0 && g[hidden].x == 0,
                "display:none grid child took a cell");
        require(g[grid].height == 70, "grid height is not its rows and gap");
    }

    // Grid values outside the adapter's profile are diagnostics, which a forgiving page's scene drops before layout:
    // line names, named lines, an automatic repetition of flexible tracks, units other than px, % and fr, repeat() in
    // an implicit track list, line 0 and a self-alignment the grid does not take. An absolutely positioned child with
    // insets needs the grid as its containing block. Geometry is the Chrome fixture's (flex_browser_diff).
    void grid_diagnostics() {
        const auto refused = [](weft::StyleValues grid, weft::StyleValues item, const char* expected,
                                const char* message) {
            Tree t;
            grid["display"] = "grid";
            grid["width"] = "200px";
            const auto g = t.add(none, std::move(grid));
            t.add(g, std::move(item));
            rejected(t, expected, message);
        };
        refused({{"grid-template-columns", "[full] 1fr"}}, {}, "WEFT_UNSUPPORTED_FORMAT", "line names accepted");
        refused({{"grid-template-columns", "repeat(auto-fill, 1fr)"}}, {}, "WEFT_UNSUPPORTED_FORMAT",
                "an automatic repetition of a flexible track accepted");
        refused({{"grid-template-columns", "10em 1fr"}}, {}, "WEFT_UNSUPPORTED_FORMAT", "an em track accepted");
        refused({{"grid-auto-rows", "repeat(2, 10px)"}}, {}, "WEFT_UNSUPPORTED_FORMAT",
                "repeat() in grid-auto-rows accepted");
        refused({}, {{"grid-column-start", "header"}}, "WEFT_UNSUPPORTED_FORMAT", "a named line accepted");
        refused({}, {{"grid-row-start", "0"}}, "WEFT_UNSUPPORTED_FORMAT", "line 0 accepted");
        refused({}, {{"justify-self", "anchor-center"}}, "WEFT_UNSUPPORTED_ALIGN", "an unknown justify-self accepted");
        refused({}, {{"position", "absolute"}, {"left", "0px"}}, "WEFT_UNSUPPORTED_FORMAT",
                "an absolute child placed against a box outside the grid");
        // The same child in a positioned grid is placed in its padding box.
        Tree positioned;
        const auto g = positioned.add(
            none, {{"display", "grid"}, {"position", "relative"}, {"width", "200px"}, {"padding-left", "10px"}});
        const auto child =
            positioned.add(g, {{"position", "absolute"}, {"right", "0px"}, {"width", "20px"}, {"height", "10px"}});
        require(positioned.layout()[child].left == 190, "absolute grid child against the grid's padding box");
    }

    // A box of px size contributes that size to its ancestors' intrinsic sizes, whatever is unknown inside it (an svg
    // of percentage size, as the CRPG ability icons in their 34px frames): a column flex item of percentage height
    // above it takes its automatic minimum, no diagnostic. With the unknown size not inside a px box, the specified
    // size suggestion (a percentage of a height known before layout) bounds the minimum instead.
    void unknown_sizes_stop_at_px_boxes() {
        for (const bool framed : {true, false}) {
            Tree t;
            const auto root = t.add(
                none, {{"display", "flex"}, {"flex-direction", "column"}, {"width", "200px"}, {"height", "100px"}});
            const auto item = t.add(root, {{"display", "flex"}, {"flex-direction", "column"}, {"height", "100%"}});
            const auto parent =
                framed ? t.add(item, {{"display", "block"}, {"width", "34px"}, {"height", "34px"}}) : item;
            t.svg(parent, {{"display", "block"}, {"width", "100%"}, {"height", "100%"}}, {}, 1);
            t.add(item, {{"display", "block"}, {"height", "300px"}, {"flex-shrink", "0"}});
            const auto g = t.layout();
            require(g[item].height == 100, framed ? "the svg's unknown height passed its 34px frame"
                                                  : "the specified size did not bound an unknown content minimum");
        }
    }

    // A block of a px size holds the flex item around it (CSS Flexbox 1 §4.5 with CSS Sizing 3 §5.1): the item's
    // automatic minimum is its content's min-content size, to which a child of a px size contributes that size whatever
    // it holds. The scene page that showed it (box-sizing: border-box on every box, the root the viewport's size): cards
    // of padding 8px around blocks 140px wide, 12px apart in a row 300px wide, keep 156px and overflow the row; with
    // min-width: 0 they share it. In a column a child's margins count. A percentage max-width of the item caps the
    // minimum, in a row whose width only layout gives too. A root that shrinks to fit is no narrower than the block.
    // Geometry is the Chrome fixture's (flex_browser_diff, auto-min-fixed-block and the cases after it).
    void fixed_blocks_hold_flex_items() {
        for (const bool zero : {false, true}) {
            Tree t;
            const auto root = t.add(none, {{"display", "block"}, {"box-sizing", "border-box"}});
            const auto row = t.add(
                root, {{"display", "flex"}, {"box-sizing", "border-box"}, {"gap", "12px"}, {"width", "300px"}});
            std::array<std::size_t, 3> cards{};
            for (auto& card : cards) {
                weft::StyleValues style{{"display", "block"}, {"box-sizing", "border-box"}, {"padding", "8px"}};
                if (zero) {
                    style["min-width"] = "0";
                }
                card = t.add(row, std::move(style));
                t.add(card, {{"display", "block"}, {"box-sizing", "border-box"}, {"width", "140px"}, {"height", "20px"}});
            }
            const auto g = t.layout(std::array{800.f, 500.f});
            const float width = zero ? 92 : 156;
            for (std::size_t i = 0; i < cards.size(); ++i) {
                require(g[cards[i]].width == width && g[cards[i]].left == float(i) * (width + 12),
                        zero ? "cards of min-width 0 did not share the row"
                             : "a flex item shrank below the block of a px width in it");
            }
        }
        Tree column;
        const auto root =
            column.add(none, {{"display", "flex"}, {"flex-direction", "column"}, {"width", "300px"}, {"height", "60px"}});
        const auto card = column.add(root, {{"display", "block"}, {"flex", "1"}});
        column.add(card, {{"display", "block"}, {"height", "40px"}, {"margin-top", "10px"}, {"margin-bottom", "5px"}});
        const auto other = column.add(root, {{"display", "block"}, {"height", "50px"}});
        const auto c = column.layout();
        require(c[card].height == 55 && c[other].height == 5, "a child's margins are part of the item's minimum height");

        Tree capped;
        const auto outer = capped.add(none, {{"display", "flex"}, {"width", "300px"}});
        const auto mid = capped.add(outer, {{"display", "flex"}, {"flex", "1"}, {"min-width", "0"}});
        const auto item = capped.add(mid, {{"display", "block"}, {"max-width", "80%"}});
        capped.add(item, {{"display", "block"}, {"width", "300px"}, {"height", "10px"}});
        capped.add(outer, {{"display", "block"}, {"width", "100px"}, {"height", "20px"}});
        const auto p = capped.layout();
        require(p[mid].width == 200 && p[item].width == 160, "a percentage max-width did not cap the item's minimum");

        Tree fit;
        const auto box = fit.add(none, {{"display", "block"}, {"padding-left", "5px"}, {"padding-right", "5px"}});
        fit.add(box, {{"display", "block"}, {"width", "140px"}, {"height", "10px"}});
        weft::LayoutOptions options;
        options.fit_width = 100;
        const auto widths = fit.intrinsic_widths();
        require(fit.layout(options)[box].width == 150 && widths.min == 150 && widths.max == 150,
                "a root that shrinks to fit is narrower than the block of a px width in it");
    }

    // A block whose children are all inline-level places them on one top-aligned line.
    void inline_children_share_a_line() {
        Tree t;
        const auto line = t.add(none, {{"display", "block"}, {"width", "200px"}});
        const auto a = t.add(line, {{"display", "inline-block"}, {"width", "30px"}, {"height", "10px"}});
        const auto b = t.add(line, {{"display", "inline-block"}, {"width", "40px"}, {"height", "20px"}});
        const auto g = t.layout();
        require(g[a].left == 0 && g[a].top == 0 && g[b].left == 30 && g[b].top == 0 && g[line].height == 20,
                "inline-level children did not share a line");
    }

    // svg width attributes stand in for width, unitless in px; in a parent of indefinite height the height follows
    // the viewBox ratio. Without attributes an svg is 300 by 150.
    void svg_sizes_from_attributes_and_ratio() {
        Tree t;
        // The root's height is definite (a viewport); its child's auto height is not.
        const auto root = t.add(none, {{"display", "block"}, {"width", "300px"}});
        const auto host = t.add(root, {{"display", "block"}});
        const auto fixed = t.svg(host, {{"display", "block"}}, "120", 2);
        const auto fluid = t.svg(host, {{"display", "block"}, {"width", "100%"}}, {}, 2);
        const auto plain = t.svg(host, {{"display", "block"}}, {}, 0);
        const auto g = t.layout();
        require(g[fixed].width == 120 && g[fixed].height == 60, "svg width attribute or ratio ignored");
        require(g[fluid].width == 300 && g[fluid].height == 150, "svg percentage width did not drive its height");
        require(g[plain].width == 300 && g[plain].height == 150, "svg default object size");
        Tree invalid;
        const auto parent =
            invalid.add(invalid.add(none, {{"display", "block"}, {"width", "300px"}}), {{"display", "block"}});
        invalid.svg(parent, {{"display", "block"}}, {}, std::numeric_limits<float>::quiet_NaN());
        rejected(invalid, "SVG intrinsic ratio", "invalid viewBox ratio accepted where it decides the height");
    }

    // A transformed box contains its absolutely positioned descendants.
    void transform_forms_containing_block() {
        for (const bool transformed : {false, true}) {
            Tree t;
            const auto root =
                t.add(none, {{"display", "block"}, {"position", "relative"}, {"width", "200px"}, {"height", "200px"}});
            weft::StyleValues middle{
                {"display", "block"}, {"margin-left", "20px"}, {"width", "100px"}, {"height", "100px"}};
            if (transformed) {
                middle["transform"] = "translateX(1px)";
            }
            const auto box = t.add(root, std::move(middle));
            const auto inner = t.add(
                box,
                {{"position", "absolute"}, {"left", "5px"}, {"top", "5px"}, {"width", "10px"}, {"height", "10px"}});
            require(t.layout()[inner].x == (transformed ? 25 : 5), "transform containing block");
        }
    }

    // An absolutely positioned box of auto width shrinks to fit the width its containing block leaves beside its insets
    // (CSS 2 10.3.7), and is no narrower than its text's min-content width, its lines laid out at the width it takes:
    // in a containing block 1px wide (a strip that moves along a timeline), at a left inset and at a right one; and in
    // a wide containing block at an inset that leaves less than the text's max-content width, in a block and in a row
    // flex container. Between two insets it stays as wide as they leave.
    void absolute_boxes_shrink_to_fit() {
        constexpr float c = limits::box_text_char_width_px;
        constexpr float line = limits::box_text_line_height_px;
        Tree t;
        t.wrapping = true;
        const auto root =
            t.add(none, {{"display", "block"}, {"position", "relative"}, {"width", "300px"}, {"height", "200px"}});
        const auto strip = t.add(
            root, {{"position", "absolute"}, {"left", "100px"}, {"top", "0px"}, {"width", "1px"}, {"height", "40px"}});
        const auto word = t.add(strip, {{"position", "absolute"}, {"left", "4px"}, {"top", "0px"}}, "abcdefghij");
        const auto right = t.add(strip, {{"position", "absolute"}, {"right", "4px"}, {"top", "0px"}}, "abcdefghij");
        const auto words = t.add(strip, {{"position", "absolute"}, {"left", "4px"}, {"top", "0px"}}, "ab cdef gh");
        const auto wide = t.add(
            root, {{"position", "absolute"}, {"left", "0px"}, {"top", "50px"}, {"width", "300px"}, {"height", "60px"}});
        const auto inset = t.add(wide, {{"position", "absolute"}, {"left", "230px"}, {"top", "0px"}}, "abc defgh ijk");
        const auto between = t.add(
            wide, {{"position", "absolute"}, {"left", "10px"}, {"right", "10px"}, {"top", "0px"}}, "abc defgh ijk");
        const auto row = t.add(root, {{"display", "flex"},
                                      {"position", "absolute"},
                                      {"left", "0px"},
                                      {"top", "120px"},
                                      {"width", "300px"},
                                      {"height", "60px"}});
        const auto in_row = t.add(row, {{"position", "absolute"}, {"right", "230px"}, {"top", "0px"}}, "abc defgh ijk");
        const auto g = t.layout();
        require(g[word].width == 10 * c && g[word].x == 104, "a word in a containing block 1px wide");
        require(g[right].width == 10 * c && g[right].x == 101 - 4 - 10 * c, "a word at a right inset in the strip");
        require(g[words].width == 4 * c && g[words].height == 3 * line,
                "wrapping text in the strip at its min-content width, a line for each word");
        require(g[inset].width == 70 && g[inset].height == 3 * line, "text at an inset wraps in the width it leaves");
        require(g[between].width == 280 && g[between].height == line, "a box between two insets");
        require(g[in_row].width == 70 && g[in_row].x == 0 && g[in_row].height == 3 * line,
                "a row flex container's absolute child at a right inset");
    }

    // A display:none box and its subtree have empty geometry at the origin.
    void hidden_subtrees_are_empty() {
        Tree t;
        const auto root = t.add(none, {{"display", "block"}, {"width", "100px"}, {"padding-left", "30px"}});
        const auto hidden = t.add(root, {{"display", "none"}, {"width", "50px"}, {"height", "50px"}});
        const auto inner = t.add(hidden, {{"width", "10px"}, {"height", "10px"}});
        const auto g = t.layout();
        require(g[hidden].x == 0 && g[hidden].width == 0 && g[inner].x == 0 && g[inner].width == 0,
                "hidden subtree has geometry");
    }

    // The viewport sizes the root (the scene root).
    void viewport_sizes_root() {
        Tree t;
        const auto root = t.add(none, {{"display", "block"}});
        const auto g = t.layout(std::array{300.f, 200.f});
        require(g[root].width == 300 && g[root].height == 200, "viewport size");
    }

    // Paint properties do not reach the box mapping; properties that would change boxes without a mapping, a
    // shorthand next to its longhands, and boxes out of tree order are diagnostics.
    void input_contract() {
        Tree paint;
        paint.add(none, {{"display", "block"},
                         {"color", "red"},
                         {"opacity", "0.5"},
                         {"box-shadow", std::string(2 * weft::constants::layout::max_style_value_bytes, 'x')}});
        (void)paint.layout();
        Tree overflow;
        overflow.add(none, {{"display", "block"}, {"overflow-x", "hidden"}});
        rejected(overflow, "WEFT_UNSUPPORTED_PROPERTY", "unmapped layout property accepted");
        Tree conflict;
        conflict.add(none, {{"display", "block"}, {"border-width", "0"}, {"border-top-width", "0"}});
        rejected(conflict, "WEFT_UNSUPPORTED_STYLE_CONFLICT", "shorthand beside its longhand accepted");
        Tree order;
        order.add(none, {{"display", "block"}});
        order.add(none, {{"display", "block"}});
        rejected(order, "WEFT_LAYOUT_TREE", "second root accepted");
    }
} // namespace

int main() {
    try {
        block_children_do_not_flex();
        flex_items_shrink_to_their_content();
        wrapping_row_keeps_its_lines();
        text_overflows_available_height();
        grid_places_visible_children();
        grid_diagnostics();
        unknown_sizes_stop_at_px_boxes();
        fixed_blocks_hold_flex_items();
        inline_children_share_a_line();
        svg_sizes_from_attributes_and_ratio();
        transform_forms_containing_block();
        absolute_boxes_shrink_to_fit();
        hidden_subtrees_are_empty();
        viewport_sizes_root();
        input_contract();
        std::cout
            << "PASS shared Yoga adapter: block, flex minimums, grid and its diagnostics, unknown sizes in px boxes, "
               "inline row, svg, containing block, absolute shrink-to-fit, hidden boxes, viewport and input "
               "diagnostics\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
