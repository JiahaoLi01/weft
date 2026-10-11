#pragma once

#include "base/rect.hpp"
#include "paint/display_list.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

// GPU-free frame input of the renderers: a display list, or ordered quads and the immutable assets they sample. A
// packet owns complete inputs and no device handles, so it can cross from the UI owner thread to a renderer.
namespace weft::render {
    using Encoding = paint::Encoding;
    using Asset = paint::Asset;

    struct Draw {
        geometry::Rect rect;
        // Straight sRGB color; renderers blend in linear premultiplied space.
        std::array<float, 4> srgb{1, 1, 1, 1};
        // Index in FramePacket::assets, or -1 for a solid quad.
        int asset{-1};
        // Diagnostics of the reference color fixture, rejected by the C ABI: an isolated opacity group
        // (red then blue inside the quad) and a scissor inset inside the quad.
        bool group{};
        bool clipped{};
        float opacity{1};
        // Normalized origin and extent of the sampled asset region.
        std::array<float, 4> uv{0, 0, 1, 1};
        // SDF outline width in source texels, not layout metrics.
        float stroke{};
        std::array<float, 4> outline{0, 0, 0, 0};
    };

    // Immutable asset leases and complete inputs; no device handles. Either quads and their assets, the v1 frame of the
    // renderer C ABI, or a display list (draws and assets empty), the frame of the Weft runtime.
    struct FramePacket {
        std::vector<Draw> draws;
        std::vector<std::shared_ptr<const Asset>> assets;
        std::shared_ptr<const paint::DisplayList> list;
    };
} // namespace weft::render
