#pragma once

#include "paint/display_list.hpp"
#include "scene/scene.hpp"
#include "scene/scene_filter.hpp"
#include "scene/scene_image_cache.hpp"
#include "scene/scene_path.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
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

    // What the visit of one element put in a display list (scene_display_list), and the context it was visited in: its
    // parent's map to target px, the clip rectangle, the run of rounded clips it inherited (list.clips[chain_begin,
    // chain_begin + chain_count)), the depth of opacity groups around it and whether no transform, scroll position or
    // anchor around it moves its text (paint::Raster::still); and the ranges [begin, end) it added to
    // each part of the list (Part: the items, the rounded clips and the payload lists, and with deferred anchors the
    // list's anchors and runs). An element its visit skips (display: none, no box) adds empty ranges.
    struct SceneListSpan {
        enum Part : std::uint8_t {
            Items,
            Clips,
            Boxes,
            Images,
            Rasters,
            Shadows,
            Paths,
            Planes,
            Effects,
            Particles,
            Backdrops,
            PathShadows,
            Filters,
            Anchors,
            Runs,
            Parts
        };

        std::array<float, 6> parent{};
        std::array<float, 4> clip{};
        std::uint32_t chain_begin{};
        std::uint32_t chain_count{};
        unsigned groups{};
        std::array<std::uint32_t, Parts> begin{};
        std::array<std::uint32_t, Parts> end{};
        bool still{};
        // Inside a group (groups above 0): what the element's subtree may paint in its parent's px, left, top, right and
        // bottom (left past right: nothing; none: beyond what layout gives it). The groups around it composite over what
        // their subtrees paint, so a patch of the element leaves them right only while this stays.
        std::optional<std::array<float, 4>> extent;
    };

    // The spans of the elements a list visited, by node: nodes of the frame the list was built from, which must
    // outlive the map.
    using SceneListSpans = std::unordered_map<const SceneNode*, SceneListSpan>;

    // How scene_display_list builds. spans, when given, receives the span of every element visited (an element's
    // pieces drawn on lines are its own). part, when given, is the only subtree visited: the node replaces the element
    // of span in `whole` (the list of an earlier frame) and is visited in that element's context. The part's list
    // starts with the rounded clips it inherits, a copy of whole's, then its own, and its ranges start at zero but for
    // its items' anchors, which are whole's: those of the part's anchored elements count from span's first anchor, and
    // the part's other items take `enclosing` (0 for none), the anchor of the anchored element of whole the part lies
    // in.
    //
    // unclipped: the root is visited without the clip of its box (a filtered element's content, scene_filter_content).
    // filters: what url() filters name; the frame's own elements when null.
    struct SceneListBuild {
        SceneListSpans* spans{};
        const SceneNode* part{};
        const SceneListSpan* span{};
        const paint::DisplayList* whole{};
        std::uint32_t enclosing{};
        bool unclipped{};
        const SceneFilterResolve* filters{};
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
    // (scene_image_cache.hpp). And the extents of subtrees (what each may paint, which a list culls by) by their
    // storage, which the cache holds: equal handles hold equal subtrees (SceneChildren), so a subtree whose handle
    // stays has the extent it had; those of subtrees nothing else holds go as each pass starts.
    //
    // Owner thread of the SceneText the lists are built with.
    class SceneDisplayCache {
    public:
        // Left, top, right and bottom in the subtree's node's own px (left past right: it paints nothing); none when
        // the subtree paints beyond what layout gives it.
        using Extent = std::optional<std::array<float, 4>>;

        explicit SceneDisplayCache(
            std::size_t max_entries = std::size_t(constants::scene::svg_values_cache_max_entries),
            std::size_t max_bytes = std::size_t(constants::scene::svg_values_cache_max_bytes));

        // Starts a pass that lists view's elements: elements earlier passes listed may go.
        void begin_pass(std::uint64_t view);

        // The element's values while its own values are those they came from; null otherwise.
        const SceneSvgValues* find(const SceneNode& element);
        // Keeps values taken from the element's own values now.
        const SceneSvgValues& put(const SceneNode& element, SceneSvgValues values);

        // The extent kept for the subtree of this storage; null when none is.
        const Extent* extent(const SceneNode& subtree);
        // Keeps a subtree's extent with its storage.
        void keep_extent(std::shared_ptr<const SceneNode> subtree, Extent extent);

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

        struct KeptExtent {
            std::shared_ptr<const SceneNode> subtree;
            Extent extent;
        };

        SceneImageCache<std::array<std::uint64_t, 4>, Entry> entries_;
        std::uint64_t view_{};
        std::size_t reuses_{};
        std::size_t builds_{};
        std::unordered_map<const SceneNode*, KeptExtent> extents_;
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
                                        const SceneAnchorInput& anchors = {}, const SceneListBuild& build = {});

    // The sizes of each part of a list (SceneListSpan::Part): its items, rounded clips, payload lists, anchors and runs.
    std::array<std::uint32_t, SceneListSpan::Parts> scene_list_sizes(const SceneDisplayList& list);

    // Whether the lists this thread builds leave out the subtrees whose extent misses their clip (scene_display_list):
    // true, but for a reference that lists every subtree to check the cull against.
    bool& scene_list_culls();

    // A filtered element's content (paint::Filter): the element alone at its origin, without its own transform,
    // opacity, filter, position and anchoring, as its list keys it (source, which keeps the element's identity), and
    // that element's list with an unclipped root, in its own px. filters resolves the url() filters of its descendants.
    // The list's fallback says why it has none; one with anchored descendants or particles has none.
    struct SceneFilterContent {
        std::shared_ptr<SceneNode> source;
        SceneDisplayList listed;
    };

    SceneFilterContent scene_filter_content(const SceneNode& element, double time_ms, SceneText* text, float scale,
                                            SceneDisplayCache* cache, const SceneFilterResolve* filters);

    // The image of a filtered element's content: the device pixels it covers at resolution device px per element px
    // (left, top, right, bottom; whole pixels, empty when the content draws nothing): where the content's items draw
    // within their clips, grown by what its functions spread and offset (blurs constants::scene::filter_kernel_sigmas
    // deviations, drop shadows that and their offset on every side) and a guard of constants::scene::filter_guard_px.
    // resolution is scale, or lower where the image would pass constants::scene::filter_image_max_pixels.
    struct SceneFilterRegion {
        std::array<std::int32_t, 4> pixels{};
        float resolution{};
    };

    SceneFilterRegion scene_filter_region(const paint::DisplayList& content, const std::vector<paint::FilterOp>& ops,
                                          float scale);
} // namespace weft
