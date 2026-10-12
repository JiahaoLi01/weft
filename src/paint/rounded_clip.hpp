#pragma once

#include <array>

namespace weft {
    // A rounded rectangle that clips what is drawn inside it: x, y, width, height with elliptical corners, in the
    // clip's own space. Geometry and coverage are in render/rounded_clip.hpp.
    struct RoundedClip {
        std::array<float, 4> rect{};
        // TL, TR, BR, BL, normalized.
        std::array<float, 4> radius_x{};
        std::array<float, 4> radius_y{};
        // Target px into the clip's space, a row-vector affine {a, b, c, d, e, f} as paint::Affine:
        // (x, y) -> (x a + y c + e, x b + y d + f). The identity for axis-aligned boxes, whose clips lie in target px;
        // the inverse of a rotated or skewed box's transform otherwise.
        std::array<float, 6> to_local{1, 0, 0, 1, 0, 0};
    };

    inline constexpr std::array<float, 6> identity_clip_map{1, 0, 0, 1, 0, 0};
} // namespace weft
