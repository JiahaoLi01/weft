#pragma once

#include "constants/common.hpp"

// Pointer input routing limits. Each constant has the type of the literal it replaced.
namespace weft::constants::input {
    // MouseEvent.buttons bit of the primary (usually left) button.
    inline constexpr int primary_button_mask = 1;
    // Largest absolute wheel delta, in logical pixels, accepted by the router.
    inline constexpr int max_wheel_delta_px = constants::max_logical_px_magnitude;
} // namespace weft::constants::input
