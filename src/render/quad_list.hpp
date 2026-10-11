#pragma once

#include "render/frame_packet.hpp"

#include <cstdint>

// Quads as a display list: renderers draw the v1 quad frames of the C ABI with their one display list pass.
namespace weft::render {
    // The display list of packet's quads, in order, over a width x height px target: a solid quad is a box and a
    // textured quad an image item of the same asset over its uv, with the quad's color as the tint of a coverage or
    // distance asset. Draw::asset indices past the packet's assets are the list's external_textures. The reference
    // fixture's diagnostics become what they check: Draw::group an isolated group of a red and then a blue box that
    // composites at the quad's opacity, Draw::clipped a clip inset into the quad.
    paint::DisplayList quad_list(const FramePacket& packet, std::uint32_t external_textures, unsigned width,
                                 unsigned height);
} // namespace weft::render
