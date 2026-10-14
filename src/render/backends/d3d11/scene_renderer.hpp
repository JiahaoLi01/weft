#pragma once

#include "constants/render.hpp"
#include "render/backends/d3d11/renderer.hpp"
#include "scene/scene.hpp"
#include "scene/scene_native.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <span>
#include <vector>

// Native D3D11 pass of a scene: the passes a ScenePreparer prepares (scene/scene_native.hpp), drawn by scene.hlsl as
// instanced box records, transparent groups in scratch targets, perspective planes and explicit layers whose content a
// nested pass draws into an image of its own, the text, shadow and path images the preparer builds, uploaded once
// each (small ones into the region the preparer gives them on an atlas page), and UI shader effects in their encoded
// pass with their textures. Only text whose color glyphs are images (PNG or SVG) comes from the D2D reference raster. The destination
// holds encoded premultiplied BGRA, like the D2D scene targets. A view's pass is kept while its frames keep their
// revision (SceneFrame::revision): such a frame draws the kept pass again, its records already on the GPU, with only
// the frame's anchors (ScenePreparer::place) and their record order uploaded.
//
// Owner thread of the SceneText. A frame the pass cannot draw returns false with a counted reason, and the caller
// draws the frame another way.
namespace weft::render::d3d11 {
    class SceneRenderer : public SceneNativeWork {
    public:
        // Per-destination state. Reuse a slot only after the GPU completed its previous pass.
        struct Slot {
            ComPtr<ID3D11Buffer> records;
            ComPtr<ID3D11Buffer> constants;
            // Each draw's view constants, one per constant_buffer_offset_bytes, rewritten a chunk at a time.
            ComPtr<ID3D11Buffer> draw_constants;
            ComPtr<ID3D11ShaderResourceView> view;
            // The pass's rounded clips (t2), grown to the largest pass.
            ComPtr<ID3D11Buffer> clips;
            ComPtr<ID3D11ShaderResourceView> clip_view;
            unsigned clip_capacity{};
            std::array<Surface, constants::render::scene::max_group_depth> groups;
            // The images and layers the last pass sampled: their textures stay while the slot holds them.
            std::vector<std::shared_ptr<const void>> leases;
            unsigned width{};
            unsigned height{};
            unsigned capacity{};
            // Scratch bytes of the slot's own targets: its groups, and in a plane's slot the plane's image.
            std::size_t group_bytes{};
            // In a plane's slot, the image its pass draws the plane's content into.
            Surface target;
            // The slots of the plane contents the last pass drew, in item order.
            std::vector<std::unique_ptr<Slot>> planes;
            // The serial of the kept pass whose records, rounded clips and plane contents the slot holds (0: none), and
            // the buffers of a frame's anchors and record order (t8 and t7), grown to the largest frame.
            std::uint64_t kept{};
            ComPtr<ID3D11Buffer> anchors;
            ComPtr<ID3D11ShaderResourceView> anchor_view;
            unsigned anchor_capacity{};
            ComPtr<ID3D11Buffer> order;
            ComPtr<ID3D11ShaderResourceView> order_view;
            unsigned order_capacity{};
        };

        // bounds are the preparer's image caches' (SceneCacheBounds). On gpu's device, or on a host's device: either
        // way the passes draw on the device's immediate context.
        SceneRenderer(Renderer& gpu, SceneText& text, SceneCacheBounds bounds = {});
        SceneRenderer(ID3D11Device* device, SceneText& text, SceneCacheBounds bounds = {});
        SceneRenderer(const SceneRenderer&) = delete;

        // A pass writes its draws' view constants once per chunk of draws and binds each by offset (D3D11.1), where the
        // device can; otherwise, or turned off, each draw updates them.
        bool constant_offsets{};
        SceneRenderer& operator=(const SceneRenderer&) = delete;

        std::size_t scratch_bytes() const {
            return scratch_bytes_;
        }

        // The views the renderer keeps a pass for, until it forgets them.
        std::size_t kept_views() const {
            return kept_.size();
        }

        // The owning surface confirms the slot's last GPU use complete first. Destroying a backing also retires its
        // share of the scratch budget, its planes' slots included.
        void retire(Slot& slot);

        // Draws frame into destination (target_width x target_height device pixels, scale per CSS px) on the
        // immediate context, clipped to crop when given. layers are the nodes drawn as cached layer images; anchors
        // place the anchored elements (ScenePreparer::prepare), deferred to the pass when the frame has a revision.
        // start: whether the pass replaces what destination holds or draws over it (SceneTargetStart). textures: the
        // host textures of the page's -weft-texture(name) backgrounds, their views ID3D11ShaderResourceView pointers;
        // an unbound name draws nothing.
        bool paint(Slot& slot, const SceneFrame& frame, ID3D11RenderTargetView* destination, float scale,
                   unsigned target_width, unsigned target_height, std::optional<std::array<float, 4>> crop,
                   unsigned view_index, const std::vector<NodeIdentity>& layers,
                   const SceneAnchorTable* anchors = nullptr, const SceneTargetStart& start = {},
                   std::span<const SceneTexture> textures = {});

        // Drops what the renderer and its preparer keep for a view: its kept pass and its anchored elements' last
        // anchors. The view's slots are the caller's to retire.
        void forget(unsigned view_index) {
            kept_.erase(view_index);
            preparer_.forget(view_index);
        }

    private:
        // The texture of a preparer's image or layer, kept while that lives.
        struct Texture {
            std::weak_ptr<const void> source;
            ComPtr<ID3D11ShaderResourceView> view;
        };

        // An atlas page of the preparer's: the images on it are written into their regions when first drawn.
        struct Page {
            ComPtr<ID3D11Texture2D> texture;
            ComPtr<ID3D11ShaderResourceView> view;
        };

        // The encoded pass of an effect program (its d3d11_encoded bytecode), created on first use. The lease keeps
        // the program alive, so its address names it alone.
        struct EffectShader {
            std::shared_ptr<const paint::EffectProgram> program;
            ComPtr<ID3D11PixelShader> shader;
        };

        // The device, its immediate context, and the states its passes share: premultiplied over blending, the
        // clamping linear sampler and the repeating one effects sample.
        ComPtr<ID3D11Device> device_;
        ComPtr<ID3D11DeviceContext> immediate_;
        ComPtr<ID3D11BlendState> blend_;
        ComPtr<ID3D11SamplerState> sampler_;
        ComPtr<ID3D11SamplerState> effect_sampler_;
        ComPtr<ID3D11DeviceContext1> context1_;
        // Whether the device binds constant buffer ranges by offset.
        bool offsets_supported_{};
        ComPtr<ID3D11VertexShader> vs_;
        ComPtr<ID3D11PixelShader> ps_;
        // The particle pass: both stages of scene.hlsl's WEFT_PARTICLES variant.
        ComPtr<ID3D11VertexShader> particle_vs_;
        ComPtr<ID3D11PixelShader> particle_ps_;
        // The backdrop passes (backdrop.hlsl): a fullscreen triangle, the area downsample, the Gaussian pass and the
        // color matrix; the sampler that mirrors addresses past an image's edge, destination-over blending (a backdrop
        // beneath its group's content) and the passes' constants.
        ComPtr<ID3D11VertexShader> backdrop_vs_;
        ComPtr<ID3D11PixelShader> area_ps_;
        ComPtr<ID3D11PixelShader> blur_ps_;
        ComPtr<ID3D11PixelShader> matrix_ps_;
        ComPtr<ID3D11SamplerState> mirror_;
        ComPtr<ID3D11BlendState> under_;
        ComPtr<ID3D11Buffer> backdrop_constants_;

        // The backdrop passes' targets, by format, width, height and place in a backdrop's chain, kept for later
        // backdrops (the immediate context orders their uses) while a frame or the one before used them.
        struct BackdropTarget {
            Surface surface;
            std::uint64_t used{};
        };

        std::map<std::array<unsigned, 4>, BackdropTarget> backdrop_targets_;
        std::uint64_t backdrop_frame_{};
        ComPtr<ID3D11RasterizerState> raster_;
        SceneRaster reference_;
        ScenePreparer preparer_;
        std::unordered_map<std::uint64_t, Texture> textures_;
        // By format and page number.
        std::map<std::array<std::uint32_t, 2>, Page> pages_;
        std::unordered_map<const paint::EffectProgram*, EffectShader> effect_shaders_;
        std::size_t scratch_bytes_{};

        // The pass kept for a view: what its frames' revision and the pass's target were, the pass, its serial, and the
        // anchors of the frame drawn last.
        struct Kept {
            std::uint64_t revision{};
            float scale{};
            unsigned width{};
            unsigned height{};
            std::vector<NodeIdentity> layers;
            std::shared_ptr<const ScenePrepared> prepared;
            std::uint64_t serial{};
            SceneAnchorFrame frame;
            // Which of its host textures had rows from the bottom: its records sample those flipped.
            std::vector<bool> flips;
        };

        std::unordered_map<unsigned, Kept> kept_;
        std::uint64_t serial_{};
        // Every record in its own place, for passes drawn in planned order (grown to the largest pass), and no anchors.
        ComPtr<ID3D11Buffer> in_order_;
        ComPtr<ID3D11ShaderResourceView> in_order_view_;
        unsigned in_order_capacity_{};
        ComPtr<ID3D11Buffer> no_anchors_;
        ComPtr<ID3D11ShaderResourceView> no_anchors_view_;

        // An image's texture in the image's format, uploaded the first time: its atlas page, or one of its own.
        ID3D11ShaderResourceView* texture(const std::shared_ptr<const SceneImage>& image);
        // An atlas page's texture, created empty the first time.
        Page& page(const SceneAtlasPage& page);
        ID3D11PixelShader* effect_shader(const std::shared_ptr<const paint::EffectProgram>& program);
        // A layer's texture, its content drawn the first time.
        ID3D11ShaderResourceView* texture(const std::shared_ptr<const SceneLayerImage>& layer, float scale);
        // Copies a backdrop's region of from's texture and filters it as it says; the result's view. The context's
        // state is the passes' afterwards.
        ID3D11ShaderResourceView* backdrop(const ScenePass::Backdrop& backdrop, ID3D11RenderTargetView* from);
        // Draws a prepared pass into destination: its planes' contents first, each into the image of its slot. anchors
        // are the frame's (none for a pass whose anchors are not deferred); serial names a kept pass, whose records the
        // slot may hold already (0 for one drawn once).
        // clear, for a pass drawn over its target: the color the target is cleared to first (none: kept).
        // host_textures: the host textures bound to the names of the pass and its planes.
        void draw_pass(Slot& slot, const ScenePrepared& prepared, ID3D11RenderTargetView* destination, float scale,
                       const SceneAnchorFrame* anchors = nullptr, std::uint64_t serial = 0,
                       const paint::Color* clear = nullptr, std::span<const SceneTexture> host_textures = {});
        // A structured buffer of count elements of stride bytes and its view, grown to at least count (by doubling).
        void structured(ComPtr<ID3D11Buffer>& buffer, ComPtr<ID3D11ShaderResourceView>& view, unsigned& capacity,
                        std::size_t count, unsigned stride, const char* what);
        // Scratch bytes a slot and its planes' slots hold.
        static std::size_t held_bytes(const Slot& slot);
        // Fits slot and its planes' slots to prepared, releasing targets of another size.
        static void shape(Slot& slot, const ScenePrepared& prepared, bool plane);
    };
} // namespace weft::render::d3d11
