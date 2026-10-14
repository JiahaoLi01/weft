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

    // A scene frame as a display list, or why it has none.
    struct SceneDisplayList {
        paint::DisplayList list;
        // Empty unless the frame needs what the list cannot express yet (SVG text without a text engine, planes
        // behind the viewer or nested too deep, unsupported values): the list is then empty, and the whole frame is
        // drawn another way, so no partly accepted list changes stacking.
        std::string fallback;
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
    // SVG elements' values between lists built with the same text.
    SceneDisplayList scene_display_list(const SceneFrame& frame, const std::vector<NodeIdentity>& layers = {},
                                        SceneText* text = nullptr, float scale = 1, SceneDisplayCache* cache = nullptr);
} // namespace weft
