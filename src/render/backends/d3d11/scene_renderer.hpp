#pragma once

#include "constants/render.hpp"
#include "render/backends/d3d11/renderer.hpp"
#include "scene/scene.hpp"
#include "scene/scene_native.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

// Native D3D11 pass of a scene: the passes a ScenePreparer prepares (scene/scene_native.hpp), drawn by scene.hlsl as
// instanced box records, transparent groups in scratch targets, perspective planes and explicit layers whose content a
// nested pass draws into an image of its own, the text, shadow and path images the preparer builds, uploaded once
// each, and UI shader effects in their encoded pass with their textures. Only text whose color glyphs are images (PNG or SVG) comes from the D2D reference raster. The destination
// holds encoded premultiplied BGRA, like the D2D scene targets.
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
        };

        // bounds are the preparer's image caches' (SceneCacheBounds).
        SceneRenderer(Renderer& gpu, SceneText& text, SceneCacheBounds bounds = {});
        SceneRenderer(const SceneRenderer&) = delete;

        // A pass writes its draws' view constants once per chunk of draws and binds each by offset (D3D11.1), where the
        // device can; otherwise, or turned off, each draw updates them.
        bool constant_offsets{};
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
        // The texture of a preparer's image or layer, kept while that lives.
        struct Texture {
            std::weak_ptr<const void> source;
            ComPtr<ID3D11ShaderResourceView> view;
        };

        // The encoded pass of an effect program (its d3d11_encoded bytecode), created on first use. The lease keeps
        // the program alive, so its address names it alone.
        struct EffectShader {
            std::shared_ptr<const paint::EffectProgram> program;
            ComPtr<ID3D11PixelShader> shader;
        };

        Renderer& gpu_;
        ComPtr<ID3D11DeviceContext1> context1_;
        // Whether the device binds constant buffer ranges by offset.
        bool offsets_supported_{};
        ComPtr<ID3D11VertexShader> vs_;
        ComPtr<ID3D11PixelShader> ps_;
        ComPtr<ID3D11RasterizerState> raster_;
        SceneRaster reference_;
        ScenePreparer preparer_;
        std::unordered_map<std::uint64_t, Texture> textures_;
        std::unordered_map<const paint::EffectProgram*, EffectShader> effect_shaders_;
        std::size_t scratch_bytes_{};

        // An image's texture in the image's format, uploaded the first time.
        ID3D11ShaderResourceView* texture(const std::shared_ptr<const SceneImage>& image);
        ID3D11PixelShader* effect_shader(const std::shared_ptr<const paint::EffectProgram>& program);
        // A layer's texture, its content drawn the first time.
        ID3D11ShaderResourceView* texture(const std::shared_ptr<const SceneLayerImage>& layer, float scale);
        // Draws a prepared pass into destination: its planes' contents first, each into the image of its slot.
        void draw_pass(Slot& slot, const ScenePrepared& prepared, ID3D11RenderTargetView* destination, float scale);
        // Scratch bytes a slot and its planes' slots hold.
        static std::size_t held_bytes(const Slot& slot);
        // Fits slot and its planes' slots to prepared, releasing targets of another size.
        static void shape(Slot& slot, const ScenePrepared& prepared, bool plane);
    };
} // namespace weft::render::d3d11
