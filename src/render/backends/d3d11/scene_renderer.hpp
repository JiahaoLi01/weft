#pragma once

#include "constants/render.hpp"
#include "render/backends/d3d11/renderer.hpp"
#include "render/scene_pass.hpp"
#include "scene/scene.hpp"
#include "scene/scene_geometry.hpp"
#include "scene/scene_display_list.hpp"
#include "vector/path_raster.hpp"

#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Native D3D11 pass of a scene display list (scene_display_list): instanced box records drawn by scene.hlsl,
// transparent groups in scratch targets, perspective planes whose content a nested pass draws into an image of its
// own, and text, layer, shadow (render/shadow_image.hpp) and path coverage (vector/path_raster.hpp) images. Text
// without color glyphs is a portable FreeType image (scene_text_image); text with color glyphs and explicit layers come
// from the D2D reference raster. The destination holds encoded premultiplied BGRA, like the D2D scene targets.
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
            // Scratch bytes of the slot's own targets: its groups, and in a plane's slot the plane's image.
            std::size_t group_bytes{};
            // In a plane's slot, the image its pass draws the plane's content into.
            Surface target;
            // The slots of the plane contents the last pass drew, in item order.
            std::vector<std::unique_ptr<Slot>> planes;
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
        std::size_t shadow_builds{};
        std::size_t shadow_reuses{};
        std::size_t path_builds{};
        std::size_t path_reuses{};
        // Plane contents drawn by nested passes into their images.
        std::size_t plane_passes{};
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
        // share of the scratch budget, its planes' slots included.
        void retire(Slot& slot);

        // Draws frame into destination (target_width x target_height device pixels, scale per CSS px) on the
        // immediate context, clipped to crop when given. layers are the nodes drawn as cached layer images.
        bool paint(Slot& slot, const SceneFrame& frame, ID3D11RenderTargetView* destination, float scale,
                   unsigned target_width, unsigned target_height, std::optional<std::array<float, 4>> crop,
                   unsigned view_index, const std::vector<NodeIdentity>& layers);

    private:
        // A pass ready to draw: its plan, the images its items sample, and per plane item the pass of its content.
        struct Prepared {
            ScenePass pass;
            std::vector<std::shared_ptr<const Image>> images;
            std::vector<std::unique_ptr<Prepared>> planes;
            unsigned width{};
            unsigned height{};
        };

        Renderer& gpu_;
        SceneText& text_;
        ComPtr<ID3D11VertexShader> vs_;
        ComPtr<ID3D11PixelShader> ps_;
        ComPtr<ID3D11RasterizerState> raster_;
        SceneRaster reference_;
        SceneGeometry geometry_;
        std::map<std::array<std::uint64_t, 4>, std::shared_ptr<const Image>> images_;
        std::size_t image_bytes_{};
        // Shadow images by value: offset, blur, spread, color, radii, box size and scale.
        std::map<std::array<float, 15>, std::shared_ptr<const Image>> shadows_;
        std::size_t shadow_bytes_{};
        vector::Rasterizer vector_;
        // Path coverage images by value: the path, its stroke, the device map and the mask's pixels.
        std::unordered_map<std::string, std::shared_ptr<const Image>> paths_;
        std::size_t path_bytes_{};
        std::size_t scratch_bytes_{};

        std::shared_ptr<const Image> image(const paint::Item& item, const paint::Raster& raster, float scale,
                                           unsigned view_index);
        // A shadow item's image; throws std::invalid_argument past the shadow bitmap bound.
        std::shared_ptr<const Image> shadow(const paint::Item& item, const paint::Shadow& shadow, float scale);
        // A path item's coverage over mask (device pixels); throws std::invalid_argument past the mask bounds.
        std::shared_ptr<const Image> path(const paint::Item& item, const paint::PathPaint& paint, float scale,
                                          const std::array<std::int32_t, 4>& mask);
        // Plans list for a width x height device pixel target and builds the images it samples, its planes' contents
        // included; throws std::invalid_argument with the reason for a list the pass cannot draw.
        std::unique_ptr<Prepared> prepare(const paint::DisplayList& list, float scale, unsigned width, unsigned height,
                                          std::optional<std::array<float, 4>> crop, unsigned view_index);
        // Draws a prepared pass into destination: its planes' contents first, each into the image of its slot.
        void draw_pass(Slot& slot, const Prepared& prepared, ID3D11RenderTargetView* destination, float scale);
        // Scratch bytes the targets of a prepared pass and of its planes' passes take; a plane's pass also takes its
        // image.
        static std::size_t scratch_of(const Prepared& prepared, bool plane);
        // Scratch bytes a slot and its planes' slots hold.
        static std::size_t held_bytes(const Slot& slot);
        // Fits slot and its planes' slots to prepared, releasing targets of another size.
        static void shape(Slot& slot, const Prepared& prepared, bool plane);
    };
} // namespace weft::render::d3d11
