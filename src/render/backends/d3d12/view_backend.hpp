#pragma once

#include "render/backend.hpp"
#include "render/backends/d3d12/device.hpp"
#include "render/backends/d3d12/scene_renderer.hpp"
#include "scene/scene_anchor.hpp"
#include "text/font_catalog.hpp"
#include "weft/render_backend.h"
#include "weft/render_backend_d3d12.h"
#include "weft/render_external_texture.h"

#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace weft {
    struct ViewFrame;
} // namespace weft

// The built-in D3D12 backend of View frames (weft_render_d3d12_create, WEFT_RENDER_FEATURE_SCENE) on a host's device:
// each pass records a View's frame with the native scene pass into the host's command list, over what the target
// holds. A recording's buffers, scratch textures, heaps and leases stay until the host reports its submission complete;
// a discarded one gives back what it would have uploaded. One SceneText, of the fonts of the first frame's runtime, and
// one kept pass per View. Its external texture extension (weft_render_d3d12_external) imports the host textures frames
// bind to their pages' -weft-texture(name) backgrounds.
namespace weft::render::d3d12 {
    class ViewBackend {
    public:
        ViewBackend(ID3D12Device* device, std::uint64_t queue);
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

        // The Views the backend keeps a pass for: a View's until a frame prepared after the View has gone.
        std::size_t views() const {
            return views_.size();
        }

        // The external texture extension's table (render_external_texture.h).
        weft_render_external_backend external();

        // RenderBackend: quad frames are not this backend's.
        weft_render_result prepare(const weft_render_frame& frame, std::uint64_t& out);
        void release_prepared(std::uint64_t native);
        weft_render_result encode(std::uint64_t native, const weft_render_pass& pass, std::uint64_t& out);
        void discard_encoded(std::uint64_t native);
        void submitted(std::uint64_t native, std::uint64_t serial);
        void completed(std::uint64_t serial);
        void abandon();
        weft_render_result shutdown();
        // SceneFrameBackend.
        weft_render_result prepare_scene(const weft_render_scene_frame& frame,
                                         const weft_render_external_asset* textures, std::uint32_t count,
                                         std::uint64_t& out);
        // ExternalTextureBackend: imports one at a time per resource; external quad frames are not this backend's.
        weft_render_result import_texture(const weft_render_external_texture& description, std::uint64_t& out);
        void release_texture(std::uint64_t native);
        weft_render_result prepare_external(const weft_render_frame& frame, const weft_render_external_asset* textures,
                                            std::uint32_t count, std::uint64_t& out);

    private:
        // An imported host texture: its resource and the view the pass reads it through (free: no resource).
        struct Import {
            ComPtr<ID3D12Resource> resource;
            View view;
        };

        // A View's frame at a scale, with the host textures it binds by name (each SceneTexture::view the address of
        // its import's view, which lives while the import does: the renderer core keeps an import while a frame that
        // binds it does).
        struct Prepared {
            std::shared_ptr<const ViewFrame> frame;
            float scale{};
            std::vector<SceneTexture> textures;
            std::vector<ComPtr<ID3D12Resource>> resources;
        };

        // A recording: its slot, and the serial of its submission once the host made it.
        struct Use {
            std::unique_ptr<SceneRenderer::Slot> slot;
            std::uint64_t serial{};
            bool submitted{};
        };

        std::unique_ptr<Device> device_;
        weft_render_backend table_{};
        std::shared_ptr<const FontCatalog> fonts_;
        std::unique_ptr<SceneText> text_;
        std::unique_ptr<SceneRenderer> renderer_;
        std::unordered_map<std::uint64_t, Prepared> prepared_;
        std::unordered_map<std::uint64_t, Use> uses_;
        std::vector<std::unique_ptr<SceneRenderer::Slot>> idle_;
        // Imported host textures; their views keep their addresses.
        std::vector<Import> imports_;
        // By the View's anchor channel, which only its View holds: a View's index never passes to another.
        std::map<std::weak_ptr<SceneAnchorChannel>, unsigned, std::owner_less<>> views_;
        std::uint64_t next_ = 1;
        unsigned next_view_{};
    };

    static_assert(SceneFrameBackend<ViewBackend> && ExternalTextureBackend<ViewBackend>);
} // namespace weft::render::d3d12
