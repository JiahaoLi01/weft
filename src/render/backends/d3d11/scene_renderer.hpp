#pragma once

#include "constants/render.hpp"
#include "render/backends/d3d11/renderer.hpp"
#include "render/scene_pass.hpp"
#include "scene/scene.hpp"
#include "scene/scene_geometry.hpp"
#include "scene/scene_display_list.hpp"

#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Native D3D11 pass of a scene display list (scene_display_list): instanced box records drawn by scene.hlsl,
// transparent groups in scratch targets, and text/layer images. Text without color glyphs is a portable FreeType
// image (scene_text_image); text with color glyphs and explicit layers come from the D2D reference raster. The
// destination holds encoded premultiplied BGRA, like the D2D scene targets.
//
// Owner thread of the SceneText. A plan the pass cannot draw returns false with a counted reason, and the caller
// draws the frame another way.
namespace weft::render::d3d11 {
    class SceneRenderer {
    public:
        // A text or layer image, immutable once built; slots lease the images their last pass sampled.
        struct Image {
            SceneNode key;
            float scale{};
            ComPtr<ID3D11ShaderResourceView> view;
            std::size_t bytes{};
        };

        // Per-destination state. Reuse a slot only after the GPU completed its previous pass.
        struct Slot {
            ComPtr<ID3D11Buffer> records;
            ComPtr<ID3D11Buffer> constants;
            ComPtr<ID3D11ShaderResourceView> view;
            // The pass's rounded clips (t2), grown to the largest pass.
            ComPtr<ID3D11Buffer> clips;
            ComPtr<ID3D11ShaderResourceView> clip_view;
            unsigned clip_capacity{};
            std::array<Surface, constants::render::scene::max_group_depth> groups;
            std::vector<std::shared_ptr<const Image>> leases;
            unsigned width{};
            unsigned height{};
            unsigned capacity{};
            std::size_t group_bytes{};
        };

        std::size_t passes{};
        std::size_t fallbacks{};
        std::size_t draws{};
        std::size_t text_builds{};
        // Text builds from portable FreeType images (the rest are reference raster images with color glyphs).
        std::size_t text_portable_builds{};
        std::size_t text_reuses{};
        std::size_t layer_builds{};
        std::size_t layer_reuses{};
        std::size_t image_evictions{};
        // Fallback counts by reason.
        std::map<std::string, std::size_t> reasons;

        SceneRenderer(Renderer& gpu, SceneText& text);
        SceneRenderer(const SceneRenderer&) = delete;
        SceneRenderer& operator=(const SceneRenderer&) = delete;

        std::size_t scratch_bytes() const {
            return scratch_bytes_;
        }

        // The owning surface confirms the slot's last GPU use complete first. Destroying a backing also retires its
        // share of the scratch budget.
        void retire(Slot& slot);

        // Draws frame into destination (target_width x target_height device pixels, scale per CSS px) on the
        // immediate context, clipped to crop when given. layers are the nodes drawn as cached layer images.
        bool paint(Slot& slot, const SceneFrame& frame, ID3D11RenderTargetView* destination, float scale,
                   unsigned target_width, unsigned target_height, std::optional<std::array<float, 4>> crop,
                   unsigned view_index, const std::vector<NodeIdentity>& layers);

    private:
        Renderer& gpu_;
        SceneText& text_;
        ComPtr<ID3D11VertexShader> vs_;
        ComPtr<ID3D11PixelShader> ps_;
        ComPtr<ID3D11RasterizerState> raster_;
        SceneRaster reference_;
        SceneGeometry geometry_;
        std::map<std::array<std::uint64_t, 4>, std::shared_ptr<const Image>> images_;
        std::size_t image_bytes_{};
        std::size_t scratch_bytes_{};

        std::shared_ptr<const Image> image(const paint::Item& item, const paint::Raster& raster, float scale,
                                           unsigned view_index);
    };
} // namespace weft::render::d3d11
