#pragma once

#include "paint/display_list.hpp"
#include "style/values.hpp"

#include <optional>

// The gradient of a scene element's background-image (CSS Images 3), one parse for the native passes and the
// reference raster:
// - linear-gradient() at an angle in deg or towards a side (to top, right, bottom or left), 180deg without either;
// - radial-gradient() of a circle or an ellipse (the default, or a circle when one length sizes it), sized by
//   closest-side, farthest-side, closest-corner, farthest-corner (the default) or lengths (one for a circle, two
//   lengths or percentages for an ellipse), at a position of one or two values (the tile's center without one);
// - conic-gradient() from an angle in deg, about the tile's center;
// - repeating-linear-gradient() and repeating-radial-gradient(), whose stops repeat along the gradient line;
// over one background tile: background-size as two lengths (the box without them) at background-position. Color
// stops take a color and up to two positions, percentages or lengths along the gradient line (a linear gradient's
// length through the tile at its angle, a radial gradient's horizontal radius); a position before an earlier one
// takes that one, and stops without one spread evenly between their neighbours.
namespace weft {
    // Nothing for background-image none, -weft-effect(), -weft-texture() and url(), or when the tile is empty
    // (nothing to paint). std::invalid_argument for a gradient outside this profile.
    std::optional<paint::Gradient> scene_gradient(const StyleValues& style, float width, float height);

    // The straight color of a gradient at a point of its item, as the scene shader evaluates it: in the tile the
    // point repeats into, between the stops its gradient line reaches there, interpolated premultiplied.
    paint::Color scene_gradient_color(const paint::Gradient& gradient, float x, float y);
} // namespace weft
