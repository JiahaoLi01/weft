#pragma once

#include "paint/rounded_clip.hpp"
#include "style/values.hpp"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// CSS clip-path (CSS Masking 1 §3) of a scene element: the shape that clips the element and its descendants, painting
// and hits alike, in the element's own px (its border box at 0, 0).
namespace weft {
    struct SceneClipPath {
        // A rounded rectangle: inset(), rect(), xywh(), circle(), ellipse() (a rectangle whose corners are the whole
        // ellipse), or a reference box alone with its corners. Its map is the identity.
        std::optional<RoundedClip> rounded;
        // Otherwise a polygon: polygon(), or path() flattened, as RoundedClip holds one (closed contours, each ending
        // on its first point, apart by a point whose x is clip_polygon_break), with its fill rule.
        std::shared_ptr<const std::vector<std::array<float, 2>>> polygon;
        bool evenodd{};
        // x, y, width, height of what the shape can let through.
        std::array<float, 4> bounds{};
        // The shape has no area (an inset past its box, a circle of no radius, a polygon of fewer than three
        // points): the element and its descendants show nothing and take no hits.
        bool empty{};
    };

    // The clip-path of an element of style whose border box is width x height px, or none (clip-path: none, and
    // -webkit-clip-path, which stands in when clip-path is none). Shapes take the reference box they name (border-box
    // when none; fill-box is the content box and stroke-box and view-box the border box, as for an element of CSS
    // layout), its percentages and its corners (a margin box's corners spread as a box shadow's). A path's curves
    // become lines no further than tolerance px off them. std::invalid_argument for a value the scene does not take:
    // url() (an SVG clipPath), lengths other than px, percentages and calc() of the two, a content box under a
    // percentage padding, more points than max_clip_polygon_points.
    std::optional<SceneClipPath> scene_clip_path(const StyleValues& style, float width, float height,
                                                 float tolerance);

    // The text of the element's clip-path that applies (clip-path, else -webkit-clip-path), none when neither does.
    std::string scene_clip_path_value(const StyleValues& style);
} // namespace weft
