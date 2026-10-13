#pragma once

#include "base/function_ref.hpp"
#include "scene/scene_text.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Inline formatting contexts of the scene: the lines of a block container whose content is its own text or
// inline-level children (text, inline elements, atomic inlines), laid out as Blink's LayoutNG lays out the profile
// this supports:
// - white-space normal and nowrap: spaces, tabs and newlines collapse across element boundaries, a line drops the
//   spaces it starts and ends with, and <br> ends a line;
// - soft wrap opportunities from UAX 14 (libunibreak) over the whole context, none inside nowrap text; an inline
//   box's start edge goes with the content after it and its end edge with the content before it;
// - greedy line filling in LayoutUnits (1/64 px): each item's part of a line is snapped up to a LayoutUnit, and a
//   line's trailing spaces do not count;
// - line boxes from the root's strut, the strut of each inline box on the line, each text's line box and each atomic
//   inline's margin box, all on one baseline (vertical-align: baseline);
// - text-align left, right and center (and start, end), start-aligned when a line overflows.
// Floats, bidi, justification and the other white-space values are diagnostics.
namespace weft {
    struct SceneNode;

    // An atomic inline (inline-block, inline-flex, inline-grid or a replaced svg) laid out at an available width:
    // its border box, its baseline from the border box top, and its margins (top, right, bottom, left).
    struct SceneAtomicExtent {
        float width{};
        float height{};
        float baseline{};
        std::array<float, 4> margin{};
    };

    // One placed piece of a context's lines, in the root's content box: left, top, width, height.
    struct SceneInlinePiece {
        enum class Kind : std::uint8_t {
            // A node's text on one line, without the spaces the line ends with: the line box of that text alone
            // (single-line metrics), on the line's baseline.
            Text,
            // A fragment of an inline element's border box: its font's content area plus its vertical padding and
            // borders. first and last tell whether the box starts and ends on this line.
            Box,
            // An atomic inline's border box.
            Atomic,
        };

        Kind kind{};
        // The text's owner (the root, a #text node or an inline element), the inline element, or the atomic inline.
        const SceneNode* node{};
        std::string text;
        std::array<float, 4> rect{};
        // Text: the baseline below the rect's top. Atomic: the baseline below the border box's top.
        float baseline{};
        bool first{};
        bool last{};
    };

    struct SceneInlineLayout {
        // The widest line and the summed line heights; no lines for content that is only collapsed spaces.
        float width{};
        float height{};
        // From the top of the first line box to its baseline, and to the last line's baseline.
        float baseline{};
        float last_baseline{};
        std::size_t lines{};
        std::vector<SceneInlinePiece> pieces;
    };

    using SceneAtomicLayout = FunctionRef<SceneAtomicExtent(const SceneNode& atomic, float available)>;

    // Whether an in-flow child of an inline formatting context is an atomic inline rather than an inline box: an
    // inline-block, inline-flex or inline-grid element, or an svg element.
    bool scene_atomic_inline(const SceneNode& n);

    // Whether an element establishes an inline formatting context for its children: an HTML block container (not
    // flex or grid) whose in-flow children are all inline-level, at least one of them.
    bool scene_inline_children(const SceneNode& n);

    // Painting an inline element: a node per fragment of its box, each the element's own values over that fragment
    // (in its parent's coordinates, like the element), with its start edge (left border and corners) only on the first
    // fragment and its end edge only on the last (box-decoration-break: slice).
    std::vector<SceneNode> scene_fragment_boxes(const SceneNode& n);
    // The element without its box (background, borders, shadow, effect): its text and children.
    SceneNode scene_without_box(const SceneNode& n);
    // A single-line text node for line i of a node's text: the line's text over its line box (box in the node's
    // coordinates), with the node's text properties and nothing that would move the text inside that box.
    SceneNode scene_text_line(const SceneNode& n, std::size_t i);

    // The inline content of a context root (its own text, or its in-flow inline-level children), collected, shaped
    // and broken into unbreakable runs once, then laid out at any width. Owner thread; it points into root's subtree
    // and is valid while that is unchanged.
    class SceneInlineContent {
    public:
        SceneInlineContent(SceneText& text, const SceneNode& root);
        ~SceneInlineContent();
        SceneInlineContent(const SceneInlineContent&) = delete;
        SceneInlineContent& operator=(const SceneInlineContent&) = delete;

        // The lines at most `available` wide; infinity lays out unconstrained lines, zero the narrowest. Atomic
        // inlines are laid out by `atomic` at the available width.
        SceneInlineLayout layout(float available, SceneAtomicLayout atomic);
        // Whether the content has a soft wrap opportunity, so that a narrower width can add lines.
        bool breakable() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace weft
