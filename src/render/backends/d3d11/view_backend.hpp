#pragma once

#include "render/backend.hpp"
#include "render/backends/d3d11/backend.hpp"
#include "render/backends/d3d11/scene_renderer.hpp"
#include "scene/scene_anchor.hpp"
#include "text/font_catalog.hpp"
#include "weft/render_backend.h"
#include "weft/render_backend_d3d11.h"
#include "weft/render_external_texture.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace weft {
    struct ViewFrame;
} // namespace weft

// The built-in D3D11 backend of View frames (weft_render_d3d11_create, WEFT_RENDER_FEATURE_SCENE) on a host's device:
// each pass draws a View's frame with the native scene pass on the device's immediate context while the host encodes,
// over what the target holds. The immediate context orders every write after the reads before it, so the backend pins
// nothing for completions; the renderer core still tracks the host's submissions. One SceneText, of the fonts of the
// first frame's runtime, and one kept pass and slot per View. Its external texture extension (weft_render_d3d11_external)
// imports the host textures frames bind to their pages' -weft-texture(name) backgrounds.
namespace weft::render::d3d11 {
    class ViewBackend {
    public:
        ViewBackend(ID3D11Device* device, std::uint64_t queue);
        ~ViewBackend();
        ViewBackend(const ViewBackend&) = delete;
        ViewBackend& operator=(const ViewBackend&) = delete;

        const weft_render_backend& table() const {
            return table_;
        }

        // The native pass's counters (passes, kept passes, fallbacks and their reasons); null before the first frame.
        const SceneRenderer* renderer() const {
            return renderer_.get();
        }

        // The Views the backend keeps a pass and slot for: a View's until a frame prepared after the View has gone.
        std::size_t views() const {
            return views_.size();
        }

        // The external texture extension's table (render_external_texture.h).
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
        // ExternalTextureBackend: imports one at a time per resource; external quad frames are not this backend's.
        weft_render_result import_texture(const weft_render_external_texture& description, std::uint64_t& out);
        void release_texture(std::uint64_t native);
        weft_render_result prepare_external(const weft_render_frame& frame, const weft_render_external_asset* textures,
                                            std::uint32_t count, std::uint64_t& out);

    private:
        // Why the last prepare or encode failed.
        std::string failure_;

        // A View's frame at a scale, with the host textures it binds by name and the resources they view (no pass
        // draws into one of them).
        struct Prepared {
            std::shared_ptr<const ViewFrame> frame;
            float scale{};
            std::vector<SceneTexture> textures;
            std::vector<ID3D11Resource*> resources;
        };

        // An imported host texture: its view and the resource it views.
        struct Import {
            ComPtr<ID3D11ShaderResourceView> view;
            ComPtr<ID3D11Resource> resource;
        };

        // A View's kept pass and the slot its passes draw with.
        struct ViewState {
            unsigned index{};
            SceneRenderer::Slot slot;
        };

        ComPtr<ID3D11Device> device_;
        weft_render_backend table_{};
        std::shared_ptr<const FontCatalog> fonts_;
        std::unique_ptr<SceneText> text_;
        std::unique_ptr<SceneRenderer> renderer_;
        std::unordered_map<std::uint64_t, Prepared> prepared_;
        std::unordered_set<std::uint64_t> uses_;
        std::vector<Import> imports_;
        // By the View's anchor channel, which only its View holds: a View's state never passes to another.
        std::map<std::weak_ptr<SceneAnchorChannel>, ViewState, std::owner_less<>> views_;
        std::uint64_t next_ = 1;
        unsigned next_view_{};
    };

    static_assert(SceneFrameBackend<ViewBackend> && ExternalTextureBackend<ViewBackend>);
} // namespace weft::render::d3d11
