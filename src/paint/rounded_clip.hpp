#pragma once

#include <array>

namespace weft {
    // A rounded rectangle that clips what is drawn inside it: x, y, width, height in target px, with elliptical
    // corners. Geometry and coverage are in render/rounded_clip.hpp.
    struct RoundedClip {
        std::array<float, 4> rect{};
        // TL, TR, BR, BL, normalized.
        std::array<float, 4> radius_x{};
        std::array<float, 4> radius_y{};
    };
} // namespace weft
