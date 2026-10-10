#pragma once

#include "constants/render.hpp"
#include "render/frame_packet.hpp"
#include "render/resource_lifetime.hpp"

// windows.h must not define min/max macros over std::min/std::max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// D3D11 reference renderer: one device and its immediate queue, ordered quads in linear premultiplied
// color. Uploads and draws are recorded on a deferred context; the host executes, submits and confirms
// completion. GPU resources retire only after an actual completion query (ResourceLifetime).
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
    };

    struct ExternalGpuAsset {
        ComPtr<ID3D11ShaderResourceView> srv;
        bool bottom_left{};
    };

    struct PreparedFrame {
        PreparedFrameId id;
        FramePacket snapshot;
        std::vector<ResourceId> dependencies;
        // Host textures drawn by Draw::asset indices past snapshot.assets.
        std::vector<ExternalGpuAsset> external;
    };

    struct Encoded {
        UseTokenId token;
        ComPtr<ID3D11CommandList> commands;
    };

    // Constant buffer of quad.hlsl, one quad per draw.
    struct alignas(16) QuadConstants {
        std::array<float, 4> rect;
        std::array<float, 2> target;
        UINT material;
        float opacity;
        std::array<float, 4> color;
        std::array<float, 4> uv;
        std::array<float, 4> sdf;
        std::array<float, 4> outline;
    };

    static_assert(sizeof(QuadConstants) == 96);

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
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        ComPtr<ID3D11Buffer> constants;
        ComPtr<ID3D11BlendState> blend;
        ComPtr<ID3D11RasterizerState> raster;
        ComPtr<ID3D11DepthStencilState> depth;
        ComPtr<ID3D11SamplerState> sampler;
        // Offscreen sRGB result, its linear float render-to-texture source and the opacity group target.
        Surface output;
        Surface intermediate;
        Surface group;
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

        struct CachedAsset {
            NativeAsset native;
            std::size_t bytes{};
            std::uint64_t touched{};
        };

        std::map<std::pair<std::uint64_t, std::uint64_t>, CachedAsset> asset_cache;
        std::map<const Asset*, std::pair<std::weak_ptr<const Asset>, std::uint64_t>> asset_identities;
        std::uint64_t next_asset_identity{1};
        std::uint64_t cache_clock{};
        std::size_t cached_bytes{};
        std::size_t peak_cached_bytes{};
        std::size_t cache_hits{};
        std::size_t native_creations{};
        std::size_t cache_resource_limit{weft::constants::render::d3d11::asset_cache_max_resources};
        std::size_t cache_byte_limit{weft::constants::render::d3d11::asset_cache_max_bytes};
        std::string adapter;
        D3D_FEATURE_LEVEL level{};
        bool debug_layer{};
        bool submitted{};
        bool acknowledged{};
        // Serial that complete() reports once host_done signals.
        SubmissionSerial pending_completion{};
        SubmissionSerial window_serial{};
        // Created on first use; destroyed only after the actual GPU drain in ~Renderer.
        Backend* backend{};
        std::size_t uploads{};
        std::size_t native_releases{};

        // A hardware renderer uses the adapter with the most dedicated memory; warp selects WARP. The
        // debug layer is enabled when the SDK layers are installed.
        Renderer(bool warp, UINT w, UINT h);
        ~Renderer();
        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        Surface surface(DXGI_FORMAT storage, DXGI_FORMAT view);
        // Stable identity of a live asset; a reused address never aliases an expired asset.
        std::uint64_t asset_identity(const std::shared_ptr<const Asset>& source);
        // Releases least recently used completed cache entries until `required` more bytes fit; `all`
        // drains the cache and requires every submission complete.
        void evict_assets(std::size_t required = 0, bool all = false);
        // Creates or reuses one leased texture per asset and pins them for the frame. keys are the
        // (asset_id, asset_version) cache keys when cache_assets is set.
        PreparedFrame prepare(FramePacket packet,
                              const std::vector<std::pair<std::uint64_t, std::uint64_t>>& keys = {});
        // Releases native textures whose leases have retired and drops completed upload copies.
        void collect();
        void bind_target(Surface& target);
        void draw(const Draw& d, ID3D11ShaderResourceView* texture, UINT material, float spread = 0);
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
    };
} // namespace weft::render::d3d11
