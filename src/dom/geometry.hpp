#pragma once

#include "dom/node.hpp"

#include <array>
#include <vector>

namespace weft {
    // An element's geometry as layout reports it to script reads (getBoundingClientRect, client and scroll sizes,
    // offsets and the local-to-viewport matrix).
    struct ElementGeometry {
        NodeIdentity identity;
        std::array<double, 4> rect{};
        std::vector<std::array<double, 4>> client_rects;
        double client_width{};
        double client_height{};
        // The left and top border widths (clientLeft, clientTop): where the padding box, and a scroll container's
        // scrollport, starts in the border box.
        double client_left{};
        double client_top{};
        double scroll_width{};
        double scroll_height{};
        double scroll_left{};
        double scroll_top{};
        bool scroll_x{};
        bool scroll_y{};
        bool wheel_x{};
        bool wheel_y{};
        std::array<double, 4> offset{};                 // Untransformed border box relative to offset parent.
        std::array<double, 6> matrix{1, 0, 0, 1, 0, 0}; // SVG/local coordinates to logical viewport.
        NodeIdentity offset_parent{};
    };
} // namespace weft
