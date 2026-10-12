#pragma once

#include "paint/display_list.hpp"

#include <array>
#include <cstdint>
#include <vector>

// Outer box-shadow pixels, prepared on the CPU for any renderer: the shadowed box's antialiased shape spread by the
// shadow's spread, blurred by a separable Gaussian of standard deviation blur / 2 (as Chromium blurs it), and kept
// only outside the box. The Direct2D reference raster draws the same pixels.
namespace weft::render {
    // Where a shadow's pixels lie: their rectangle in the item's CSS px (left, top, width, height) and their size.
    struct ShadowGeometry {
        std::array<float, 4> rect{};
        std::uint32_t width{};
        std::uint32_t height{};
    };

    // Premultiplied straight-sRGB RGBA8 rows over geometry.rect.
    struct ShadowImage {
        ShadowGeometry geometry;
        std::vector<std::uint8_t> rgba;
    };

    // The geometry of a shadow of a width x height box at a device scale.
    ShadowGeometry shadow_geometry(const paint::Shadow&, float width, float height, float scale);

    // The pixels of a shadow of a width x height box at a device scale, or Diag::PaintList past the bitmap bound.
    Result<ShadowImage> shadow_image(const paint::Shadow&, float width, float height, float scale);
} // namespace weft::render
