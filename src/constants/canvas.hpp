#pragma once

#include "constants/common.hpp"

// Budgets and fixed parameters of the Canvas2D probe (src/canvas/). The backing dimension and
// pending command limits are shared with the DOM binding and live in common.hpp. Each constant
// has the type of the literal it replaced.
namespace weft::constants::canvas {
    // Longest color string accepted by fillStyle parsing.
    inline constexpr int max_color_length = 512;
    // Live 2D contexts per owner budget.
    inline constexpr int max_live_contexts = 8;
    // Backing bytes per owner budget, including the overlap while a canvas resizes.
    inline constexpr int max_backing_bytes = 32 * constants::bytes_per_mib;
    // Largest coordinate, size or radius magnitude accepted by drawing calls.
    inline constexpr double max_geometry_magnitude = 1e6;
    // Initial and reset fillStyle of a 2D context.
    inline constexpr const char* default_fill_style = "#000000";
    // Direct2D target DPI at which one DIP is one pixel.
    inline constexpr int d2d_pixel_dpi = 96;
} // namespace weft::constants::canvas
