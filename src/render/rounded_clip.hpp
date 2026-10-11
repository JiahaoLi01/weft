#pragma once
#include "paint/rounded_clip.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace weft {
    RoundedClip normalize_rounded(RoundedClip);
    bool contains_rounded(const RoundedClip&, float x, float y);
    // Backend-independent coverage preparation. No DOM, JS, or GPU references.
    std::vector<std::uint8_t> rounded_coverage(const std::vector<RoundedClip>&, std::array<float, 4> rect,
                                               std::uint32_t width, std::uint32_t height);
} // namespace weft
