#import "render/backends/metal/scene_renderer.hpp"

#import "render/backends/metal/scene_internal.hpp"

#include "constants/common.hpp"

#include <cstring>

// The Metal scene renderer's frames: encode() with the kept passes, the slots' submission, discard and retirement, and
// draw() on the renderer's own queue.
namespace weft::render::metal {
    using namespace detail;

    bool SceneRenderer::encode(Slot& slot, const SceneFrame& frame, id<MTLCommandBuffer> command,
                               const Target& target, float scale, std::optional<std::array<float, 4>> crop,
                               unsigned view_index, const std::vector<NodeIdentity>& layers,
                               const SceneAnchorTable* anchors, const SceneTargetStart& start,
                               std::span<const SceneTexture> textures) {
        if (slot.state == Slot::State::Submitted) {
            retire(slot);
        }
        ensure(slot.state == Slot::State::Idle, "an idle or completed scene slot");
        ensure(command != nil && target.texture != nil && target.width && target.height &&
                   target.width <= unsigned(limits::max_surface_dimension) &&
                   target.height <= unsigned(limits::max_surface_dimension) &&
                   target.texture.width >= target.width && target.texture.height >= target.height &&
                   target.texture.pixelFormat == target_format,
               "a bounded BGRA8 Metal scene target");
        // Textures of images and layers that neither the preparer nor a slot holds any more: every recording that
        // sampled them completed.
        for (auto it = textures_.begin(); it != textures_.end();) {
            if (it->second.source.expired()) {
                it = textures_.erase(it);
            } else {
                ++it;
            }
        }
        // A frame of the revision the view's kept pass was prepared for draws it again with the frame's anchors; a
        // frame of another revision patches it where the pass stands (ScenePreparer::patch).
        const bool keep = frame.revision && !crop;
        auto& kept = kept_[view_index];
        std::shared_ptr<ScenePrepared> prepared;
        if (keep && kept.prepared && kept.scale == scale && kept.width == target.width &&
            kept.height == target.height && kept.layers == layers && kept.prepared->over == start.over &&
            scene_texture_flips(*kept.prepared, textures) == kept.flips) {
            bool drawable = kept.revision == frame.revision;
            if (!drawable) {
                if (const auto patched = preparer_.patch(*kept.prepared, frame, view_index, anchors)) {
                    const auto from = kept.serial;
                    kept.serial = ++serial_;
                    kept.revision = frame.revision;
                    kept.history.add(from, kept.serial, *patched);
                    ++kept_patches;
                    for (const auto& [b, e] : patched->records) {
                        patched_records += e - b;
                    }
                    drawable = true;
                }
            }
            if (drawable && preparer_.place(*kept.prepared, view_index, anchors, kept.frame)) {
                prepared = kept.prepared;
                ++kept_passes;
            } else if (drawable) {
                ++kept_rebuilds;
            }
        }
        if (!prepared) {
            kept = {};
            prepared = preparer_.prepare(frame, scale, target.width, target.height, crop, view_index, layers, anchors,
                                         keep, start.over);
            if (!prepared) {
                last_failure_ = preparer_.last_fallback();
                return false;
            }
            if (prepared->keepable) {
                kept = {frame.revision,
                        scale,
                        target.width,
                        target.height,
                        layers,
                        prepared,
                        ++serial_,
                        {},
                        scene_texture_flips(*prepared, textures),
                        {}};
                ensure(preparer_.place(*prepared, view_index, anchors, kept.frame), "a pass placed as it was planned");
            }
        }
        const auto* placed = kept.prepared == prepared ? &kept.frame : nullptr;
        const auto serial = kept.prepared == prepared ? kept.serial : 0;
        // An effect's or a url() image's translucent color texture is linear floats, which not every device filters.
        const auto linear = [](auto&& self, const ScenePrepared& p) -> bool {
            for (const auto& effect : p.effects) {
                for (const auto& texture : effect.textures) {
                    if (texture->format == SceneImage::Format::Linear) {
                        return true;
                    }
                }
            }
            for (const auto& image : p.images) {
                if (image && image->format == SceneImage::Format::Linear) {
                    return true;
                }
            }
            for (std::size_t i = 0; i < p.planes.size(); ++i) {
                if ((p.planes[i] && self(self, *p.planes[i])) || (p.layers[i] && self(self, *p.layers[i]->content)) ||
                    (p.filters[i] && self(self, *p.filters[i]->content))) {
                    return true;
                }
            }
            return false;
        };
        if (!float_filtering_ && linear(linear, *prepared)) {
            ++fallbacks;
            ++reasons["effect float texture filtering"];
            last_failure_ = "effect float texture filtering";
            return false;
        }
        slot_ = &slot;
        command_buffer_ = command;
        slot.recording = ++next_recording_;
        try {
            check_pending(*prepared);
            upload(*prepared);
            record(*prepared, target.texture, target.readable, scale, placed, serial,
                   start.clear ? &*start.clear : nullptr, textures, serial ? &kept.history : nullptr);
        } catch (...) {
            // The host discards the command buffer: what it would upload or draw leaves the cache.
            for (const auto id : slot.created) {
                textures_.erase(id);
            }
            release(slot);
            slot_ = nullptr;
            command_buffer_ = nil;
            throw;
        }
        slot_ = nullptr;
        command_buffer_ = nil;
        slot.state = Slot::State::Recorded;
        ++passes;
        ++recordings;
        return true;
    }

    void SceneRenderer::submitted(Slot& slot) {
        ensure(slot.state == Slot::State::Recorded, "a recorded scene slot");
        for (const auto id : slot.created) {
            if (const auto it = textures_.find(id); it != textures_.end() && it->second.pending == slot.recording) {
                it->second.pending = 0;
            }
        }
        slot.state = Slot::State::Submitted;
    }

    void SceneRenderer::discard(Slot& slot) noexcept {
        if (slot.state != Slot::State::Recorded) {
            return;
        }
        // No other recording samples these: one that would have was refused as Busy.
        for (const auto id : slot.created) {
            if (const auto it = textures_.find(id); it != textures_.end() && it->second.pending == slot.recording) {
                textures_.erase(it);
            }
        }
        release(slot);
    }

    void SceneRenderer::retire(Slot& slot) noexcept {
        if (slot.state == Slot::State::Recorded) {
            return;
        }
        release(slot);
    }

    std::optional<std::vector<std::uint8_t>>
    SceneRenderer::draw(const SceneFrame& frame, float scale, unsigned width, unsigned height,
                        std::optional<std::array<float, 4>> crop, unsigned view_index,
                        const std::vector<NodeIdentity>& layers, bool readable, const SceneAnchorTable* anchors) {
        ensure(width && height && width <= unsigned(limits::max_surface_dimension) &&
                   height <= unsigned(limits::max_surface_dimension),
               "bounded Metal scene target");
        const auto bytes = std::size_t(width) * height * weft::constants::rgba8_bytes_per_pixel;
        if (target_.width != width || target_.height != height) {
            target_ = {make_texture(device, target_format, width, height,
                                    MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead),
                       width, height};
            readback_ = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
            ensure(readback_ != nil, "the readback buffer");
        }
        auto& slot = draw_slot_;
        id<MTLCommandBuffer> command = [queue_ commandBuffer];
        ensure(command != nil, "a command buffer");
        if (!encode(slot, frame, command, {target_.texture, width, height, readable}, scale, crop, view_index, layers,
                    anchors)) {
            return std::nullopt;
        }
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        [blit copyFromTexture:target_.texture
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(width, height, 1)
                            toBuffer:readback_
                   destinationOffset:0
              destinationBytesPerRow:std::size_t(width) * weft::constants::rgba8_bytes_per_pixel
            destinationBytesPerImage:bytes];
        [blit endEncoding];
        [command commit];
        submitted(slot);
        [command waitUntilCompleted];
        retire(slot);
        ensure(command.status == MTLCommandBufferStatusCompleted,
               "the frame's command buffer completed: " + describe(command.error));
        std::vector<std::uint8_t> out(bytes);
        std::memcpy(out.data(), readback_.contents, bytes);
        return out;
    }
} // namespace weft::render::metal
