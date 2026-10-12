#pragma once

#include "base/result.hpp"
#include "paint/display_list.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

// Paths as coverage masks on every platform: a path filled by its rule, or stroked, under a map into device pixels.
// FreeType's rasterizer computes exact area coverage and its stroker builds the stroke outline (in the path's own
// space, so a non-uniform map stretches the stroke as SVG does); dashes are cut here from the path flattened within
// a tenth of a device pixel.
namespace weft::vector {
    // 8-bit coverage rows over the device pixels [x, x + width) x [y, y + height).
    struct Mask {
        std::int32_t x{};
        std::int32_t y{};
        std::uint32_t width{};
        std::uint32_t height{};
        std::vector<std::uint8_t> coverage;
    };

    // Device pixels (left, top, right, bottom) a path's paint can reach under device (item px to device px): the
    // bounds of its points, grown by a stroke's farthest reach and a pixel for antialiasing, then cut to clip. Empty
    // (right <= left or bottom <= top) when nothing remains.
    std::array<std::int32_t, 4> mask_bounds(const paint::PathPaint& paint, const paint::Affine& device,
                                            const std::array<std::int32_t, 4>& clip);

    // Owns a FreeType library; use one rasterizer on one thread.
    class Rasterizer {
    public:
        Rasterizer();
        ~Rasterizer();
        Rasterizer(const Rasterizer&) = delete;
        Rasterizer& operator=(const Rasterizer&) = delete;

        // The coverage of paint under device over bounds (from mask_bounds). Diag::PaintList past
        // constants::render::path::max_mask_pixels or FreeType's outline bounds.
        Result<Mask> rasterize(const paint::PathPaint& paint, const paint::Affine& device,
                               const std::array<std::int32_t, 4>& bounds);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace weft::vector
