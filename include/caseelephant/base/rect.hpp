#pragma once

#include <algorithm>
#include <array>

// Axis-aligned layout rectangle in logical CSS pixels, shared by the page builders.
namespace caseelephant::geometry {
    struct Rect {
        float x{};
        float y{};
        float w{};
        float h{};

        std::array<float, 4> array() const noexcept {
            return {x, y, w, h};
        }
    };

    // Overlap of a and b. Disjoint rectangles give an empty rectangle at the larger origin.
    inline Rect intersect(Rect a, Rect b) noexcept {
        const float x = std::max(a.x, b.x);
        const float y = std::max(a.y, b.y);
        return {x, y, std::max(0.f, std::min(a.x + a.w, b.x + b.w) - x),
                std::max(0.f, std::min(a.y + a.h, b.y + b.h) - y)};
    }
} // namespace caseelephant::geometry
