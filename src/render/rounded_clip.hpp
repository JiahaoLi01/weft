#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace weft {
    struct RoundedClip {
        std::array<float, 4> rect{};
        // TL, TR, BR, BL, normalized.
        std::array<float, 4> radius_x{};
        std::array<float, 4> radius_y{};
    };

    RoundedClip normalize_rounded(RoundedClip);
    bool contains_rounded(const RoundedClip&, float x, float y);
    // Backend-independent coverage preparation. No DOM, JS, or GPU references.
    std::vector<std::uint8_t> rounded_coverage(const std::vector<RoundedClip>&, std::array<float, 4> rect,
                                               std::uint32_t width, std::uint32_t height);
} // namespace weft
