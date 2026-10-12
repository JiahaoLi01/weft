#pragma once

#include "constants/render.hpp"
#include "render/atlas_packer.hpp"
#include "render/frame_packet.hpp"
#include "render/resource_lifetime.hpp"
#include "render/scene_pass.hpp"
#include "render/texture_cache.hpp"

// windows.h must not define min/max macros over std::min/std::max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11_1.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// D3D11 reference renderer: one device and its immediate queue, display lists in linear premultiplied color
// (quad packets as their display lists, render/quad_list.hpp). Uploads and draws are recorded on a deferred
// context; the host executes, submits and confirms completion. GPU resources retire only after an actual completion
// query (ResourceLifetime).
//
// Failures throw std::runtime_error. The C ABI adapter (backend.hpp) turns them into weft_render_result
// values, so no exception crosses the ABI. The state stays public: probes and tests assert on upload,
// cache and retirement counters and drive the device directly as the host.
namespace weft::render::d3d11 {
    using Microsoft::WRL::ComPtr;

    // Throws std::runtime_error(message) unless condition holds.
    void ensure(bool condition, std::string_view message);
    // Throws std::runtime_error("<operation> HRESULT=0x<hex>") for a failed HRESULT.
    void throw_if_failed(HRESULT result, const char* operation);

    struct Surface {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11ShaderResourceView> srv;
    };

    struct NativeAsset {
        ResourceId id;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> srv;
        // Retained until the upload is submitted, so a cancelled recording can upload again.
        std::vector<std::uint8_t> upload;
        UINT pitch{};
        bool cache_copy{};
        // Translucent encoded sRGB inputs: the upload goes to source (RGBA8 UINT) and srgb_convert.hlsl renders the
        // linear premultiplied float texels into texture through rtv. Released once the resource is ready.
        ComPtr<ID3D11Texture2D> source;
        ComPtr<ID3D11ShaderResourceView> source_srv;
        ComPtr<ID3D11RenderTargetView> rtv;
        // A coverage image on a glyph atlas page instead of its own texture: its region there, including the border.
        // texture and srv are the page's, and upload holds the bordered image.
        std::optional<AtlasPacker::Region> atlas;
    };

    struct ExternalGpuAsset {
        ComPtr<ID3D11ShaderResourceView> srv;
        bool bottom_left{};
    };

    struct PreparedFrame {
        PreparedFrameId id;
        // The immutable list the frame draws, in a linear pass of the shared scene shader.
        std::shared_ptr<const paint::DisplayList> list;
        std::vector<ResourceId> dependencies;
        // The list's external textures.
        std::vector<ExternalGpuAsset> external;
    };

    struct Encoded {
        UseTokenId token;
        ComPtr<ID3D11CommandList> commands;
    };

    // A structured buffer of per-frame pass data that grows to the largest frame. With command lists the driver records
    // itself it is written in command order by UpdateSubresource1: renaming a buffer mapped with discard stalled the GPU
    // for milliseconds on an RTX 5090. Where the runtime emulates command lists (WARP) it is mapped with discard.
    struct PassBuffer {
        ComPtr<ID3D11Buffer> buffer;
        ComPtr<ID3D11ShaderResourceView> view;
        std::size_t capacity{};
    };

    // A texture that image items sample: an asset's or an external texture's view, whose rows may start at the bottom.
    struct ImageView {
        ID3D11ShaderResourceView* view{};
        bool bottom_up{};
        // The part of the texture that holds the image (an atlas region), normalized origin and extent.
        std::array<float, 4> region{0, 0, 1, 1};
    };

    // A glyph atlas page.
    struct AtlasPage {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> srv;
    };

    class Backend;

    class Renderer {
    public:
        const UINT width;
        const UINT height;
        ResourceLifetime tracker;
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> immediate;
        ComPtr<ID3D11DeviceContext> deferred;
        ComPtr<ID3D11InfoQueue> info;
        // The linear display list pass (scene.hlsl, SPACE_LINEAR) and its View constants.
        ComPtr<ID3D11VertexShader> display_vs;
        ComPtr<ID3D11PixelShader> display_ps;
        ComPtr<ID3D11Buffer> display_constants;
        PassBuffer display_records;
        PassBuffer display_clips;
        ComPtr<ID3D11BlendState> blend;
        ComPtr<ID3D11RasterizerState> raster;
        ComPtr<ID3D11DepthStencilState> depth;
        ComPtr<ID3D11SamplerState> sampler;
        // The linear sampler that repeats the textures effects sample (scene.hlsl, effectRepeat).
        ComPtr<ID3D11SamplerState> effect_sampler;
        // GPU conversion of translucent encoded sRGB inputs and its decode/alpha tables, and of encoded premultiplied
        // textures (Direct2D targets): convert_vs draws one triangle over the viewport.
        ComPtr<ID3D11VertexShader> convert_vs;
        ComPtr<ID3D11PixelShader> convert_ps;
        ComPtr<ID3D11ShaderResourceView> convert_tables;
        ComPtr<ID3D11PixelShader> encoded_convert_ps;
        // Offscreen sRGB result, its linear float render-to-texture source and the linear alpha check target.
        Surface output;
        Surface intermediate;
        Surface group;
        // Linear float targets of nested isolated groups, created on first use at the renderer's size.
        std::vector<Surface> group_layers;
        // CPU readback copies of output (RGBA8) and group (linear float).
        ComPtr<ID3D11Texture2D> staging;
        ComPtr<ID3D11Texture2D> alpha_staging;
        // UI work, then the host's completion marker after everything it submitted on the queue.
        ComPtr<ID3D11Query> ui_done;
        ComPtr<ID3D11Query> host_done;
        ComPtr<ID3D11Query> timing_disjoint;
        ComPtr<ID3D11Query> timing_start;
        ComPtr<ID3D11Query> timing_end;
        double prepare_ms{};
        double encode_ms{};
        double present_ms{};
        double wait_ms{};
        double gpu_ui_ms{};
        std::vector<NativeAsset> assets;
        bool cache_assets{};
        bool pipeline_enabled{};
        std::size_t async_in_flight{};

        // The texture cache of a caching renderer (render/texture_cache.hpp) and the identities of the shared assets of
        // display lists, its keys.
        TextureCache<NativeAsset> cache{weft::constants::render::texture_cache::max_resources,
                                        weft::constants::render::texture_cache::max_bytes};
        AssetIdentities asset_identities;
        std::size_t native_creations{};
        // Glyph atlas of a caching renderer: small coverage assets share R8 pages, so that consecutive glyphs draw
        // together. A full atlas leaves an asset its own texture; a page whose regions were all evicted starts empty.
        AtlasPacker atlas{weft::constants::render::texture_cache::atlas_page_size,
                          weft::constants::render::texture_cache::atlas_max_pages};
        std::vector<AtlasPage> atlas_pages;
        std::size_t atlas_placements{};
        // Instanced draws and the records they drew, across all display list passes.
        std::size_t display_draws{};
        std::size_t display_instances{};
        std::string adapter;
        D3D_FEATURE_LEVEL level{};
        bool debug_layer{};
        // Whether the driver records command lists itself rather than the runtime emulating them.
        bool driver_command_lists{};
        bool submitted{};
        bool acknowledged{};
        // Serial that complete() reports once host_done signals.
        SubmissionSerial pending_completion{};
        SubmissionSerial window_serial{};
        // Created on first use; destroyed only after the actual GPU drain in ~Renderer.
        Backend* backend{};
        std::size_t uploads{};
        std::size_t gpu_conversions{};
        std::size_t native_releases{};

        // A hardware renderer uses the adapter with the most dedicated memory; warp selects WARP. With with_debug_layer
        // the device takes the D3D11 debug layer where the SDK layers are installed (debug_layer tells whether it did);
        // without, never, as a shipped host runs it.
        Renderer(bool warp, UINT w, UINT h, bool with_debug_layer = true);
        ~Renderer();
        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        Surface surface(DXGI_FORMAT storage, DXGI_FORMAT view);
        // Stable identity of a live asset; a reused address never aliases an expired asset.
        std::uint64_t asset_identity(const std::shared_ptr<const Asset>& source);
        // Releases least recently used completed cache entries until `required` more bytes fit; `all`
        // drains the cache and requires every submission complete.
        void evict_assets(std::size_t required = 0, bool all = false);
        // Creates or reuses one leased texture per asset of the list and pins them for the frame. keys are the
        // (asset_id, asset_version) cache keys when cache_assets is set. The list draws boxes, images, effects and
        // isolated groups (no scene rasters) at one device px per CSS px. An asset an effect samples has a texture of
        // its own, never an atlas region.
        PreparedFrame prepare(std::shared_ptr<const paint::DisplayList> list,
                              const std::vector<std::pair<std::uint64_t, std::uint64_t>>& keys = {});
        PreparedFrame prepare(paint::DisplayList list,
                              const std::vector<std::pair<std::uint64_t, std::uint64_t>>& keys = {});
        // A list packet prepares its list; a quad packet its quad_list, whose asset indices past the packet's assets
        // name external_textures.
        PreparedFrame prepare(FramePacket packet, const std::vector<std::pair<std::uint64_t, std::uint64_t>>& keys = {},
                              std::uint32_t external_textures = 0);
        // Releases native textures whose leases have retired and drops completed upload copies.
        void collect();
        // Records list into target (target_width x target_height px) on context: image asset i samples images[i]
        // (the list's assets, then its external textures). Groups need a target of the renderer's size.
        void record(ID3D11DeviceContext* context, const paint::DisplayList& list, ID3D11RenderTargetView* target,
                    UINT target_width, UINT target_height, const std::vector<ImageView>& images);
        // Records uploads and draws into a command list. With native_context and native_target the host's
        // deferred context and render target are used; otherwise the offscreen output (rtt draws through
        // the linear intermediate). fail_after_upload injects a recording failure for tests.
        Encoded encode(const PreparedFrame& frame, bool rtt, bool fail_after_upload = false,
                       ID3D11DeviceContext* native_context = nullptr, ID3D11RenderTargetView* native_target = nullptr,
                       bool clear_native_target = true, const float* native_clear = nullptr);
        // Waits (bounded) for host_done, then reports pending_completion to the tracker or backend.
        void complete();
        void enable_timing();
        // Throws on the first stored debug layer warning or error.
        void check_debug_messages();
        Backend& ensure_backend();
        void complete_backend();

    private:
        // The linear pass of an effect program (its d3d11_linear bytecode), created on first use. The lease keeps the
        // program alive, so its address names it alone.
        struct EffectShader {
            std::shared_ptr<const paint::EffectProgram> program;
            ComPtr<ID3D11PixelShader> shader;
        };

        std::unordered_map<const paint::EffectProgram*, EffectShader> effect_shaders_;
        ID3D11PixelShader* effect_shader(const std::shared_ptr<const paint::EffectProgram>& program);
        // Places a coverage asset on the glyph atlas and makes upload its bordered image; nothing when it does not fit.
        std::optional<AtlasPacker::Region> place_on_atlas(const Asset& a, std::vector<std::uint8_t>& upload);
        // The normalized part of an asset's texture that holds it: an atlas region, or the whole texture.
        std::array<float, 4> atlas_region(const NativeAsset& a) const;
    };
} // namespace weft::render::d3d11
