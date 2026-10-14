#pragma once

#include "paint/display_list.hpp"

#include <array>
#include <cstdint>
#include <vector>

// Box-shadow pixels, prepared on the CPU for any renderer: an outer shadow's box grown by the shadow's spread, or what
// lies outside an inset shadow's box shrunk by it, antialiased, offset, blurred by a separable Gaussian of standard
// deviation blur / 2 (as Chromium blurs it), and kept only outside the box (outer) or inside it (inset). The Direct2D
// reference raster draws the same pixels.
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

    // A w x h mask (rows top-down) blurred by a separable Gaussian of standard deviation sigma px, its kernel
    // constants::scene::shadow_kernel_sigmas deviations wide and read as zero beyond the mask: the blur of the shadows
    // here and of text shadows.
    std::vector<float> gaussian_blur(const std::vector<float>& mask, unsigned w, unsigned h, float sigma);
} // namespace weft::render
