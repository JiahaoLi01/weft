#pragma once

#include "render/frame_packet.hpp"
#include "weft/render_backend.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>

// A C ABI quad frame (weft_render_frame) as the FramePacket that adapters prepare: copies of its call-scoped draws and
// assets, so that the caller's arrays and pixels may go once prepare returns.
namespace weft::render {
    // copy_bytes(i) false leaves asset i's bytes empty, for a renderer that already holds its texture.
    template <class CopyBytes>
    FramePacket frame_packet(const weft_render_frame& frame, CopyBytes&& copy_bytes) {
        FramePacket packet;
        packet.assets.reserve(frame.asset_count);
        packet.draws.reserve(frame.draw_count);
        for (std::uint32_t i = 0; i < frame.asset_count; ++i) {
            const auto& a = frame.assets[i];
            auto asset =
                std::make_shared<Asset>(Asset{a.width, a.height, static_cast<Encoding>(a.encoding), {}, a.sdf_spread});
            if (copy_bytes(i)) {
                asset->bytes.assign(a.bytes, a.bytes + static_cast<std::size_t>(a.byte_count));
            }
            packet.assets.push_back(std::move(asset));
        }
        for (std::uint32_t i = 0; i < frame.draw_count; ++i) {
            const auto& a = frame.draws[i];
            Draw d;
            d.rect = {a.x, a.y, a.width, a.height};
            std::copy_n(a.srgb, 4, d.srgb.begin());
            d.asset = a.asset_index == WEFT_RENDER_SOLID_ASSET ? -1 : int(a.asset_index);
            d.opacity = a.opacity;
            std::copy_n(a.uv, 4, d.uv.begin());
            d.stroke = a.sdf_stroke;
            std::copy_n(a.outline_srgb, 4, d.outline.begin());
            packet.draws.push_back(d);
        }
        return packet;
    }
} // namespace weft::render
