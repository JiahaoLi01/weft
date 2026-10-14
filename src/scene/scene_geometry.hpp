#pragma once

#include "layout/hit_test.hpp"
#include "scene/scene.hpp"
#include "scene/scene_transform.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Portable scene geometry: SVG path data, element boxes and hit testing, with the transforms of scene_transform.hpp.
// The D2D reference raster replays the same transforms and path commands with its own D2D math.
namespace weft {
    // SVG viewBox/preserveAspectRatio of an svg element as scale and translation.
    struct SceneViewport {
        float sx{1};
        float sy{1};
        float tx{};
        float ty{};
    };

    SceneViewport scene_svg_viewport(const SceneNode& svg);

    // SVG path data in absolute coordinates. Relative, smooth (S/T) and H/V commands are resolved; a drawing
    // command after Z or at the start begins a figure with an explicit Move. Arc keeps the endpoint form
    // {rx, ry, x-axis-rotation, large-arc, sweep, x, y}; Cubic {x1, y1, x2, y2, x, y}; Quadratic {x1, y1, x, y}.
    struct ScenePathCommand {
        enum class Kind {
            Move,
            Line,
            Cubic,
            Quadratic,
            Arc,
            Close
        };

        Kind kind{};
        std::array<float, 7> v{};
    };

    std::vector<ScenePathCommand> parse_scene_path(std::string_view data);
    // Whitespace/comma separated numbers (SVG points, stroke-dasharray).
    std::vector<float> parse_scene_numbers(std::string_view list);

    // Element boxes (getBoundingClientRect-style geometry) and hit testing of a laid-out scene, without any
    // platform graphics API. Owner thread only; text boxes come from SceneText.
    //
    // anchors place anchored elements (data-anchor) as scene_display_list draws them: hidden ones take no hits and
    // measure as empty boxes at the origin, as display: none does; an anchored element has no offset parent and its
    // offset is where its anchor puts its box. For data-anchor-missing: stay each call remembers what the anchored
    // elements took, for the next.
    class SceneGeometry {
    public:
        explicit SceneGeometry(SceneText& text);
        ~SceneGeometry();
        SceneGeometry(const SceneGeometry&) = delete;
        SceneGeometry& operator=(const SceneGeometry&) = delete;

        // A frame's hit areas in paint order (layout/hit_test.hpp), each target an index in targets.
        struct HitMap {
            HitAreas areas;
            std::vector<NodeIdentity> targets;

            // The topmost node at (x, y); a default identity for none.
            NodeIdentity hit(float x, float y) const;
        };

        std::vector<ElementGeometry> geometry(const SceneFrame& frame, bool svg_descendants = false,
                                              const SceneAnchorTable* anchors = nullptr);
        // For many points of one frame, build its hit map once.
        HitMap hit_map(const SceneFrame& frame, const SceneAnchorTable* anchors = nullptr);
        NodeIdentity hit_test(const SceneFrame& frame, float x, float y, const SceneAnchorTable* anchors = nullptr);
        // The node's CSS transform as {m11, m12, m21, m22, dx, dy}, without transform-origin.
        std::array<double, 6> transform_matrix(const SceneNode& node) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace weft
