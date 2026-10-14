#pragma once

#include "render/backend.hpp"
#include "render/backends/d3d11/scene_renderer.hpp"
#include "scene/scene_anchor.hpp"
#include "text/font_catalog.hpp"
#include "weft/render_backend.h"
#include "weft/render_backend_d3d11.h"

#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace weft {
    struct ViewFrame;
} // namespace weft

// The built-in D3D11 backend of View frames (weft_render_d3d11_create, WEFT_RENDER_FEATURE_SCENE) on a host's device:
// each pass draws a View's frame with the native scene pass on the device's immediate context while the host encodes,
// over what the target holds. The immediate context orders every write after the reads before it, so the backend pins
// nothing for completions; the renderer core still tracks the host's submissions. One SceneText, of the fonts of the
// first frame's runtime, and one kept pass and slot per View.
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
        weft_render_result prepare_scene(const weft_render_scene_frame& frame, std::uint64_t& out);

    private:
        struct Prepared {
            std::shared_ptr<const ViewFrame> frame;
            float scale{};
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
        // By the View's anchor channel, which only its View holds: a View's state never passes to another.
        std::map<std::weak_ptr<SceneAnchorChannel>, ViewState, std::owner_less<>> views_;
        std::uint64_t next_ = 1;
        unsigned next_view_{};
    };

    static_assert(SceneFrameBackend<ViewBackend>);
} // namespace weft::render::d3d11
