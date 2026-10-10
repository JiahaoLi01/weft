#pragma once

#include "constants/render.hpp"

#include <cmath>

namespace weft::render {
    // IEC 61966-2-1 sRGB decode of one [0, 1] channel to linear light, as in the quad shader.
    inline float srgb_to_linear(float c) {
        return c <= constants::render::srgb_decode_threshold
                   ? c / constants::render::srgb_linear_slope
                   : std::pow((c + constants::render::srgb_offset) / constants::render::srgb_scale,
                              constants::render::srgb_gamma);
    }
} // namespace weft::render
