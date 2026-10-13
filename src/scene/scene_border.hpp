#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

// Dotted and dashed border sides as Chrome paints them (BoxBorderPainter::DrawDashedOrDottedBoxSide over
// StyledStrokeData), in device pixels: the box snapped to whole pixels, the side's width a whole number of them, a
// stroke along the side's centre line with the dash pattern for the side's length (dashes 3:2 or 2:1 of the width with
// the gap that fits a whole number, square dots one width apart, or round dots for widths over 3 px), and for thin
// dots the explicit end dots Blink adds (EnforceDotsAtEndpoints). The display list and the reference raster draw the
// same shapes.
namespace weft {
    enum class SceneBorderStyle : std::uint8_t {
        None,
        Solid,
        Dotted,
        Dashed,
    };

    // A side's style value; other values are not supported (std::nullopt is not used: the caller diagnoses).
    bool scene_border_style(std::string_view value, SceneBorderStyle& out);

    // One shape of a dotted or dashed side in the box's own coordinates: a rectangle, or a round dot filling it.
    struct SceneDash {
        std::array<float, 4> rect{};
        bool round{};
    };

    // The shapes of side `side` (0 top, 1 right, 2 bottom, 3 left) of a box of `size` in its own coordinates, `width`
    // of the box's px wide. `device` maps the box's coordinates to device pixels, axis-aligned: x * device[0] +
    // device[2], y * device[1] + device[3]. The box is snapped to whole device pixels as Chrome snaps it, the shapes laid
    // out there and mapped back.
    std::vector<SceneDash> scene_dashed_side(std::array<float, 2> size, unsigned side, float width,
                                             SceneBorderStyle style, const std::array<double, 4>& device);
} // namespace weft
