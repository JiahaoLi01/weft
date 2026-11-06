#pragma once

// Objective-C++ (ARC).
#import <Metal/Metal.h>

#include "render/backend.hpp"
#include "render/backends/metal/scene_renderer.hpp"
#include "scene/scene_anchor.hpp"
#include "text/font_catalog.hpp"
#include "weft/render_backend.h"
#include "weft/render_backend_metal.h"
#include "weft/render_external_texture.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace weft {
    struct ViewFrame;
} // namespace weft

// The built-in Metal backend of View frames (weft_render_metal_create, WEFT_RENDER_FEATURE_SCENE) on a host's device:
// each pass encodes a View's frame with the native scene pass into the host's command buffer, over what the target
// holds. A recording's buffers, scratch textures and leases stay until the host reports its submission complete; a
// discarded one gives back what it would have uploaded. One SceneText, of the fonts of the first frame's runtime, and
// one kept pass per View. Its external texture extension (weft_render_metal_external) imports the host textures frames
// bind to their pages' -weft-texture(name) backgrounds, retaining each until its lease retires. As the Vulkan backend's.
namespace weft::render::metal {
    class ViewBackend {
    public:
        ViewBackend(id<MTLDevice> device, std::uint64_t queue);
        ~ViewBackend();
        ViewBackend(const ViewBackend&) = delete;
        ViewBackend& operator=(const ViewBackend&) = delete;

        const weft_render_backend& table() const {
            return table_;
        }

        // The native pass's counters; null before the first frame.
        const SceneRenderer* renderer() const {
            return renderer_.get();
        }

        weft_render_external_backend external();

        // RenderBackend: quad frames are not this backend's.
        weft_render_result prepare(const weft_render_frame& frame, std::uint64_t& out);
        void release_prepared(std::uint64_t native);
        weft_render_result encode(std::uint64_t native, const weft_render_pass& pass, std::uint64_t& out);

        // FailureBackend: why the last prepare or encode failed.
        std::string_view last_failure() const {
            return failure_;
        }

        void discard_encoded(std::uint64_t native);
        void submitted(std::uint64_t native, std::uint64_t serial);
        void completed(std::uint64_t serial);
        void abandon();
        weft_render_result shutdown();
        // SceneFrameBackend.
        weft_render_result prepare_scene(const weft_render_scene_frame& frame,
                                         const weft_render_external_asset* textures, std::uint32_t count,
                                         std::uint64_t& out);
        // ExternalTextureBackend: one import per texture at a time; external quad frames are not this backend's.
        weft_render_result import_texture(const weft_render_external_texture& description, std::uint64_t& out);
        void release_texture(std::uint64_t native);
        weft_render_result prepare_external(const weft_render_frame& frame, const weft_render_external_asset* textures,
                                            std::uint32_t count, std::uint64_t& out);

    private:
        std::string failure_;

        // A View's frame at a scale, with the host textures it binds by name.
        struct Prepared {
            std::shared_ptr<const ViewFrame> frame;
            float scale{};
            std::vector<SceneTexture> textures;
        };

        // A recording: its slot, and the serial of its submission once the host made it.
        struct Use {
            std::unique_ptr<SceneRenderer::Slot> slot;
            std::uint64_t serial{};
            bool submitted{};
        };

        id<MTLDevice> device_;
        weft_render_backend table_{};
        std::shared_ptr<const FontCatalog> fonts_;
        std::unique_ptr<SceneText> text_;
        std::unique_ptr<SceneRenderer> renderer_;
        std::unordered_map<std::uint64_t, Prepared> prepared_;
        std::unordered_map<std::uint64_t, Use> uses_;
        std::vector<std::unique_ptr<SceneRenderer::Slot>> idle_;
        // Imported host textures (nil: free), retained until their leases retire.
        std::vector<id<MTLTexture>> imports_;
        // By the View's anchor channel, which only its View holds.
        std::map<std::weak_ptr<SceneAnchorChannel>, unsigned, std::owner_less<>> views_;
        std::uint64_t next_ = 1;
        unsigned next_view_{};
    };

    static_assert(SceneFrameBackend<ViewBackend> && ExternalTextureBackend<ViewBackend>);
} // namespace weft::render::metal
