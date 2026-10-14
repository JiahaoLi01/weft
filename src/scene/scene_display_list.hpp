#pragma once

#include "paint/display_list.hpp"
#include "scene/scene.hpp"
#include "scene/scene_image_cache.hpp"
#include "scene/scene_path.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace weft {
    class SceneText;

    // An anchored element of a list with deferred anchors: its anchoring, the size of its border box and its own
    // transform about its origin ({m11, m12, m21, m22, dx, dy}, which data-anchor-offscreen: clamp bounds), and the
    // items [first, end) of its subtree, which its anchor moves (paint::Item::anchor).
    struct SceneListAnchor {
        NodeIdentity element;
        SceneAnchoring anchoring;
        std::array<float, 2> box{};
        std::array<float, 6> own{};
        std::uint32_t first{};
        std::uint32_t end{};
    };

    // Siblings of one z-index, at least two of them anchored, in the order a list with deferred anchors holds them:
    // each sibling's items [first, end), its anchored element (1 + its index in the list's anchors; 0 for a sibling
    // that keeps its place) and its place among its parent's children. A pass orders the anchored ones by depth each
    // frame, as scene_anchor_order does.
    struct SceneListRun {
        struct Member {
            std::uint32_t first{};
            std::uint32_t end{};
            std::uint32_t anchor{};
            std::uint32_t child{};
        };

        std::vector<Member> members;
        // Whether a pass may draw the run's items in another order without planning it again: they lie in one batch
        // that draws its records in any order (set by the preparer).
        bool reorderable{};
    };

    // A scene frame as a display list, or why it has none.
    struct SceneDisplayList {
        paint::DisplayList list;
        // Empty unless the frame needs what the list cannot express yet (SVG text without a text engine, planes
        // behind the viewer or nested too deep, unsupported values): the list is then empty, and the whole frame is
        // drawn another way, so no partly accepted list changes stacking.
        std::string fallback;
        // The anchors its anchored elements took, by element (scene_anchor_resolve): the next list's
        // SceneAnchorInput::last. Empty with deferred anchors.
        std::vector<ScenePlacedAnchor> placed;
        // With deferred anchors: whether the list could defer them (anchored subtrees hold no backdrop, particles,
        // plane or effect, which a pass cannot move; the list is otherwise empty, to be built placed), its anchored
        // elements in visiting order, outer ones first, and its runs of anchored siblings, inner ones first.
        bool deferred{};
        std::vector<SceneListAnchor> anchors;
        std::vector<SceneListRun> runs;
    };

    // What a display list takes from an SVG element's own values: its transform in its parent's user units and its
    // opacity; its text's line box origin in user units, size, fill and glyph outlines (null when the text draws as an
    // image); its path with the fill and stroke colors (alpha 0 when not drawn) and the stroke style, and its shape
    // when it is a rect, circle, ellipse or line (which may draw as boxes).
    struct SceneSvgValues {
        std::array<float, 6> local{};
        float opacity{};
        std::array<float, 2> text_at{};
        SceneTextMetrics text_metrics;
        std::array<float, 4> text_fill{};
        std::shared_ptr<const paint::Path> outline;
        std::shared_ptr<const paint::Path> path;
        std::array<float, 4> fill{};
        std::array<float, 4> stroke{};
        paint::Stroke stroke_style;
        std::optional<SceneSvgShape> shape;
    };

    // SVG elements' values (SceneSvgValues) by view and element identity, while the element's tag, attributes, style and
    // text stay those they came from: a frame lists an unchanged element without parsing it again, and the list equals
    // one built without the cache. Past its bounds the least recently used elements of earlier passes go
    // (scene_image_cache.hpp).
    //
    // Owner thread of the SceneText the lists are built with.
    class SceneDisplayCache {
    public:
        explicit SceneDisplayCache(
            std::size_t max_entries = std::size_t(constants::scene::svg_values_cache_max_entries),
            std::size_t max_bytes = std::size_t(constants::scene::svg_values_cache_max_bytes));

        // Starts a pass that lists view's elements: elements earlier passes listed may go.
        void begin_pass(std::uint64_t view) {
            view_ = view;
            entries_.begin_pass();
        }

        // The element's values while its own values are those they came from; null otherwise.
        const SceneSvgValues* find(const SceneNode& element);
        // Keeps values taken from the element's own values now.
        const SceneSvgValues& put(const SceneNode& element, SceneSvgValues values);

        // Elements listed from their kept values, and values taken anew.
        std::size_t reuses() const {
            return reuses_;
        }

        std::size_t builds() const {
            return builds_;
        }

    private:
        struct Entry {
            std::string tag;
            std::string space;
            std::string text;
            AttributeMap attributes;
            SceneStyle style;
            SceneSvgValues values;
        };

        SceneImageCache<std::array<std::uint64_t, 4>, Entry> entries_;
        std::uint64_t view_{};
        std::size_t reuses_{};
        std::size_t builds_{};
    };

    // The frame's boxes, shadows, text, SVG paths, perspective planes and opacity groups in paint order. layers are the
    // nodes drawn as cached layer images; text measures SVG text, which needs it to be placed. scale is the pass's
    // device pixels per px, on which dotted and dashed borders lay out their dots and dashes. cache, when given, keeps
    // SVG elements' values between lists built with the same text. anchors place the anchored elements (data-anchor):
    // each where its anchor puts it in the frame's root box, beyond its ancestors' transforms and overflow clips (as
    // position: fixed is), its subtree with it; anchored siblings of equal z-index draw far to near
    // (scene_anchor_order). An anchored element without an anchor, or hidden by its anchor, draws nothing. Deferred,
    // every anchored subtree lies at its element's origin, unclipped but by its own clips, with its items' anchor set,
    // for the pass to move by the anchor of each frame; its groups composite over the root's box; siblings are ordered
    // by the anchors given.
    SceneDisplayList scene_display_list(const SceneFrame& frame, const std::vector<NodeIdentity>& layers = {},
                                        SceneText* text = nullptr, float scale = 1, SceneDisplayCache* cache = nullptr,
                                        const SceneAnchorInput& anchors = {});
} // namespace weft
