#include "layout/flex.hpp"
#include "constants/layout.hpp"
#include "support/check.hpp"
#include "test_constants/layout.hpp"
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

        std::vector<weft::BoxGeometry> layout(std::optional<std::array<float, 2>> viewport = std::nullopt) const {
            // Text never wraps: one line, as wide as its characters.
            const auto lines = [&](std::size_t i, float) {
                return weft::TextMetrics{float(texts[i].size()) * limits::box_text_char_width_px,
                                         limits::box_text_line_height_px, limits::box_text_baseline_px, 1};
            };
            const auto min_content = [&](std::size_t i) {
                return float(texts[i].size()) * limits::box_text_char_width_px;
            };
            const auto wraps = [](std::size_t) {
                return false;
            };
            weft::LayoutOptions options;
            options.viewport = viewport;
            return weft::layout_boxes(boxes, {lines, min_content, wraps}, options);
        }
    };

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
        inline_children_share_a_line();
        svg_sizes_from_attributes_and_ratio();
        transform_forms_containing_block();
        hidden_subtrees_are_empty();
        viewport_sizes_root();
        input_contract();
        std::cout << "PASS shared Yoga adapter: block, flex minimums, grid, inline row, svg, containing block, hidden "
                     "boxes, viewport and input diagnostics\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
