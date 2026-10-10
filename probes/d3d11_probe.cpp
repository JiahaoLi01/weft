#include "caseelephant/auto_page_probe.hpp"
#include "caseelephant/script_platform.hpp"
#include "caseelephant/flow_page_probe.hpp"
#include "caseelephant/scene_probe.hpp"
#include "caseelephant/scene_native_probe.hpp"
#include "caseelephant/owner_executor_probe.hpp"
#define NOMINMAX
#include "caseelephant/resource_lifetime.hpp"
#include "caseelephant/text_probe.hpp"
#include "caseelephant/page_probe.hpp"
#include "caseelephant/positioned_page_probe.hpp"
#include "caseelephant/input_probe.hpp"
#include "caseelephant/render_backend_d3d11.h"
// Constants and test-support headers must be seen here, at global scope: probe headers included
// inside the anonymous namespace below would otherwise declare (anonymous)::caseelephant.
#include "caseelephant/constants/common.hpp"
#include "probe_constants.hpp"
#include "gallery_constants.hpp"
#include "../tests/test_constants/scene.hpp"
#include "../tests/test_constants/support.hpp"
#include "../tests/support/check.hpp"
#include "../tests/support/files.hpp"
#include "../tests/support/frame_rate.hpp"
#include "../tests/support/host_time.hpp"
#include "../tests/support/json.hpp"
#include "../tests/support/owner_trace.hpp"
#include "../tests/support/png.hpp"
#include <windowsx.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <psapi.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <map>
#include <span>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>

// An independent M0 host/renderer experiment. No DOM or stable renderer API.
namespace {
    using Microsoft::WRL::ComPtr;
    using namespace caseelephant::render;
    constexpr UINT width = caseelephant::probe_constants::color_fixture_width;
    constexpr UINT height = caseelephant::probe_constants::color_fixture_height;

    using caseelephant::testing::check_hresult;
    using caseelephant::testing::format_host_time;
    using caseelephant::testing::json_string;
    using caseelephant::testing::OwnerTrace;
    using caseelephant::testing::read_file;
    using caseelephant::testing::require;
    using caseelephant::testing::TraceControl;
    using caseelephant::testing::WindowFrameRate;

    void lifetime(LifetimeResult r, const char* operation) {
        require(r == LifetimeResult::Ok, operation);
    }

    float decode(float c) {
        return c <= caseelephant::probe_constants::srgb_decode_threshold
                   ? c / caseelephant::probe_constants::srgb_linear_slope
                   : std::pow((c + caseelephant::probe_constants::srgb_offset) /
                                  caseelephant::probe_constants::srgb_scale,
                              caseelephant::probe_constants::srgb_gamma);
    }

    std::string utf8(const wchar_t* text) {
        const auto size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        require(size > 0, "adapter name conversion");
        std::string result(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
        result.pop_back();
        return result;
    }

    using Pixel = std::array<std::uint8_t, 4>;
    using Color = std::array<float, 4>;
    enum class Encoding {
        SrgbStraight,
        Coverage,
        SignedDistance
    };

    struct Asset {
        UINT width{};
        UINT height{};
        Encoding encoding{};
        std::vector<std::uint8_t> bytes;
        float spread{};
    };

    struct Rect {
        float x{};
        float y{};
        float w{};
        float h{};
    };

    struct Draw {
        Rect rect;
        Color srgb{1, 1, 1, 1};
        int asset{-1};
        bool group{};
        bool clipped{};
        float opacity{1};
        std::array<float, 4> uv{0, 0, 1, 1};
        float stroke{}; // source texels, not layout metrics
        Color outline{0, 0, 0, 0};
    };

    // GPU-free snapshot; immutable asset leases, complete inputs, no device handles.
    struct FramePacket {
        std::vector<Draw> draws;
        std::vector<std::shared_ptr<const Asset>> assets;
    };

    FramePacket scene() {
        FramePacket p;
        p.assets.push_back(std::make_shared<const Asset>(Asset{1, 1, Encoding::Coverage, {128}}));
        // Fully transparent blue must not pollute filtered opaque red.
        p.assets.push_back(
            std::make_shared<const Asset>(Asset{2, 1, Encoding::SrgbStraight, {255, 0, 0, 255, 0, 0, 255, 0}}));
        caseelephant::TextProbeFont font(CE_SOURCE_DIR "/third_party/ahem/Ahem.ttf");
        const auto shaped = font.shape("X", 24);
        const auto glyph = font.rasterize(shaped.glyphs[0].glyph_id, 24);
        p.assets.push_back(
            std::make_shared<const Asset>(Asset{glyph.width, glyph.height, Encoding::Coverage, glyph.coverage}));
        p.draws.push_back({{0, 0, caseelephant::probe_constants::color_fixture_width,
                            caseelephant::probe_constants::color_fixture_height},
                           {0, 0, 0, 1}});
        p.draws.push_back({{0, 0, 64, 64}, {1, 1, 1, 0.5f}});
        p.draws.push_back({{64, 0, 64, 64}, {1, 0, 0, 0.5f}});
        p.draws.push_back({{64, 0, 64, 64}, {0, 0, 1, 0.5f}});
        p.draws.push_back({{128, 0, 64, 64}, {128 / 255.0f, 128 / 255.0f, 128 / 255.0f, 1}});
        p.draws.push_back({{192, 0, 64, 64}, {1, 1, 1, 1}, 0});
        p.draws.push_back({{0, 64, 64, 64}, {1, 1, 1, 1}, -1, true, false, 0.5f});
        p.draws.push_back({{64, 64, 64, 64}, {1, 1, 1, 1}, 1, false, false, 1, {0.5f, 0.5f, 0, 0}});
        p.draws.push_back({{148, 84, 24, 24}, {1, 1, 1, 1}, 2});
        p.draws.push_back({{192, 64, 64, 64}, {1, 0, 0, 1}, -1, false, true});
        return p;
    }

    struct SdfScene {
        FramePacket packet;
        std::array<Rect, 3> squares{{{16.25f, 14.25f, 20, 20}, {56.25f, 14.25f, 40, 40}, {128.25f, 14.25f, 60, 60}}};
        Rect stroked{212.25f, 14.25f, 20, 20};
        std::size_t generated_glyphs{};
        std::size_t distance_bytes{};
    };

    SdfScene sdf_scene() {
        SdfScene scene;
        auto atlas =
            std::make_shared<Asset>(Asset{caseelephant::probe_constants::sdf_atlas_width,
                                          caseelephant::probe_constants::sdf_atlas_height, Encoding::SignedDistance,
                                          std::vector<std::uint8_t>(caseelephant::probe_constants::sdf_atlas_width *
                                                                    caseelephant::probe_constants::sdf_atlas_height),
                                          caseelephant::constants::text::sdf_default_spread});

        struct Cell {
            caseelephant::ProbeSdfBitmap bitmap;
            UINT x{};
            UINT y{};
        };

        const auto pack = [&](caseelephant::ProbeSdfBitmap bitmap, UINT x, UINT y) {
            require(x + bitmap.width + 1 < atlas->width && y + bitmap.height + 1 < atlas->height,
                    "fixed probe atlas capacity");
            for (UINT row = 0; row < bitmap.height; ++row) {
                std::copy_n(bitmap.distances.data() + static_cast<std::size_t>(row) * bitmap.width, bitmap.width,
                            atlas->bytes.data() + static_cast<std::size_t>(y + row) * atlas->width + x);
            }
            ++scene.generated_glyphs;
            scene.distance_bytes += bitmap.distances.size();
            return Cell{std::move(bitmap), x, y};
        };
        caseelephant::TextProbeFont ahem(CE_SOURCE_DIR "/third_party/ahem/Ahem.ttf");
        const auto shape = ahem.shape("X", caseelephant::constants::text::sdf_default_base_ppem);
        const auto square =
            pack(ahem.rasterize_sdf(shape.glyphs[0].glyph_id, caseelephant::constants::text::sdf_default_base_ppem,
                                    caseelephant::constants::text::sdf_default_spread),
                 1, 1);
        scene.packet.draws.push_back({{0, 0, caseelephant::probe_constants::color_fixture_width,
                                       caseelephant::probe_constants::color_fixture_height},
                                      {0, 0, 0, 1}});
        const auto glyph_draw = [&](const Cell& cell, float pen, float baseline, float scale) {
            const auto& b = cell.bitmap;
            Draw d{
                {pen + b.left * scale, baseline - b.top * scale, b.width * scale, b.height * scale}, {1, 1, 1, 1}, 0};
            d.uv = {cell.x / static_cast<float>(caseelephant::probe_constants::sdf_atlas_width),
                    cell.y / static_cast<float>(caseelephant::probe_constants::sdf_atlas_height),
                    b.width / static_cast<float>(caseelephant::probe_constants::sdf_atlas_width),
                    b.height / static_cast<float>(caseelephant::probe_constants::sdf_atlas_height)};
            return d;
        };
        // Same generated glyph, unchanged logical shape; only device quad scale varies.
        for (const auto& body : scene.squares) {
            const float scale = body.w / caseelephant::constants::text::sdf_default_base_ppem;
            scene.packet.draws.push_back(glyph_draw(square, body.x, body.y + 32 * scale, scale));
        }
        auto stroke = glyph_draw(square, scene.stroked.x, scene.stroked.y + 16, 0.5f);
        stroke.stroke = 2;
        stroke.outline = {0, 0, 1, 1};
        scene.packet.draws.push_back(stroke);
        caseelephant::TextProbeFont roboto(CE_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf");
        const auto latin = roboto.shape("SDF", caseelephant::constants::text::sdf_default_base_ppem);
        require(!latin.missing_glyph && latin.glyphs.size() == 3, "actual Latin glyph shape");
        UINT cursor = 70;
        std::vector<Cell> letters;
        for (const auto& glyph : latin.glyphs) {
            auto cell = pack(roboto.rasterize_sdf(glyph.glyph_id, caseelephant::constants::text::sdf_default_base_ppem,
                                                  caseelephant::constants::text::sdf_default_spread),
                             cursor, 1);
            cursor += cell.bitmap.width + 2;
            letters.push_back(std::move(cell));
        }
        for (const auto scale : {0.6f, 1.2f}) {
            float pen = scale < 1 ? 12.0f : 88.0f;
            const float baseline = scale < 1 ? 112.0f : 117.0f;
            for (std::size_t i = 0; i < letters.size(); ++i) {
                const auto& glyph = latin.glyphs[i];
                scene.packet.draws.push_back(
                    glyph_draw(letters[i], pen + glyph.offset_x * scale, baseline - glyph.offset_y * scale, scale));
                pen += glyph.advance_x * scale;
            }
        }
        // Poison immediately beyond each one-texel guard. Any atlas UV bleed is visible.
        for (UINT y = 0; y < 65; ++y) {
            for (UINT x = 58; x < 64; ++x) {
                atlas->bytes[y * caseelephant::probe_constants::sdf_atlas_width + x] = 255;
            }
        }
        for (UINT y = 70; y < 74; ++y) {
            std::fill_n(atlas->bytes.data() + y * caseelephant::probe_constants::sdf_atlas_width,
                        caseelephant::probe_constants::sdf_atlas_width, std::uint8_t{255});
        }
        scene.packet.assets.push_back(std::move(atlas));
        return scene;
    }

    struct Surface {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11ShaderResourceView> srv;
    };

    struct NativeAsset {
        ResourceId id;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> srv;
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
        std::vector<ExternalGpuAsset> external;
    };

    struct Encoded {
        UseTokenId token;
        ComPtr<ID3D11CommandList> commands;
    };

    struct alignas(16) Constants {
        std::array<float, 4> rect;
        std::array<float, 2> target;
        UINT material;
        float opacity;
        Color color;
        std::array<float, 4> uv;
        std::array<float, 4> sdf;
        Color outline;
    };

    static_assert(sizeof(Constants) == 96);
    class ProbeBackend;

    class Experiment {
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
        Surface output;
        Surface intermediate;
        Surface group;
        ComPtr<ID3D11Texture2D> staging;
        ComPtr<ID3D11Texture2D> alpha_staging;
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
        std::size_t cache_resource_limit{caseelephant::probe_constants::gpu_cache_max_resources};
        std::size_t cache_byte_limit{caseelephant::probe_constants::gpu_cache_max_bytes};
        std::string adapter;
        D3D_FEATURE_LEVEL level{};
        bool debug_layer{};
        bool submitted{};
        bool acknowledged{};
        SubmissionSerial pending_completion{caseelephant::probe_constants::fixture_host_serial};
        SubmissionSerial window_serial{};
        ProbeBackend* backend{}; // Owned; destroyed only after actual GPU drain.
        ProbeBackend& ensure_backend();
        void complete_backend();
        std::size_t assertions{};
        std::size_t uploads{};
        std::size_t native_releases{};
        Color transparent_sample{};

        explicit Experiment(bool warp, UINT w = caseelephant::probe_constants::color_fixture_width,
                            UINT h = caseelephant::probe_constants::color_fixture_height)
            : width(w), height(h) {
            require(w && h && w <= caseelephant::probe_constants::max_surface_dimension &&
                        h <= caseelephant::probe_constants::max_surface_dimension,
                    "bounded probe surface");
            ComPtr<IDXGIFactory1> factory;
            check_hresult(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
            ComPtr<IDXGIAdapter1> selected;
            if (!warp) {
                SIZE_T largest{};
                for (UINT i = 0;; ++i) {
                    ComPtr<IDXGIAdapter1> candidate;
                    const auto hr = factory->EnumAdapters1(i, &candidate);
                    if (hr == DXGI_ERROR_NOT_FOUND) {
                        break;
                    }
                    check_hresult(hr, "EnumAdapters1");
                    DXGI_ADAPTER_DESC1 desc{};
                    check_hresult(candidate->GetDesc1(&desc), "GetDesc1");
                    if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                        (!selected || desc.DedicatedVideoMemory > largest)) {
                        selected = candidate;
                        largest = desc.DedicatedVideoMemory;
                    }
                }
                require(selected != nullptr, "No hardware adapter; use explicit --warp instead");
            }
            const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
            const auto create = [&](UINT flags) {
                return D3D11CreateDevice(selected.Get(), warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                         flags | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1, D3D11_SDK_VERSION,
                                         &device, &level, &immediate);
            };
            auto hr = create(D3D11_CREATE_DEVICE_DEBUG);
            if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING) {
                hr = create(0);
            } else if (SUCCEEDED(hr)) {
                debug_layer = true;
            }
            check_hresult(hr, "D3D11CreateDevice");
            ComPtr<IDXGIDevice> dxgi;
            check_hresult(device.As(&dxgi), "IDXGIDevice");
            ComPtr<IDXGIAdapter> actual;
            check_hresult(dxgi->GetAdapter(&actual), "GetAdapter");
            DXGI_ADAPTER_DESC desc{};
            check_hresult(actual->GetDesc(&desc), "GetDesc");
            adapter = utf8(desc.Description);
            if (debug_layer) {
                check_hresult(device.As(&info), "ID3D11InfoQueue");
            }
            check_hresult(device->CreateDeferredContext(0, &deferred), "CreateDeferredContext");
            ComPtr<ID3DBlob> vertex_code;
            ComPtr<ID3DBlob> pixel_code;
            ComPtr<ID3DBlob> errors;
            const auto shader = std::filesystem::path(CE_SOURCE_DIR) / "shaders/m0_probe.hlsl";
            UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS;
#ifndef NDEBUG
            flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
            flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
            const auto compile = [&](const char* entry, const char* profile, ComPtr<ID3DBlob>& result) {
                const auto status =
                    D3DCompileFromFile(shader.c_str(), nullptr, nullptr, entry, profile, flags, 0, &result, &errors);
                if (FAILED(status) && errors) {
                    std::cerr.write(static_cast<const char*>(errors->GetBufferPointer()),
                                    static_cast<std::streamsize>(errors->GetBufferSize()));
                }
                check_hresult(status, "D3DCompileFromFile");
            };
            compile("vs_main", "vs_5_0", vertex_code);
            compile("ps_main", "ps_5_0", pixel_code);
            check_hresult(
                device->CreateVertexShader(vertex_code->GetBufferPointer(), vertex_code->GetBufferSize(), nullptr, &vs),
                "CreateVertexShader");
            check_hresult(
                device->CreatePixelShader(pixel_code->GetBufferPointer(), pixel_code->GetBufferSize(), nullptr, &ps),
                "CreatePixelShader");
            D3D11_BUFFER_DESC cb{};
            cb.ByteWidth = sizeof(Constants);
            cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            check_hresult(device->CreateBuffer(&cb, nullptr, &constants), "CreateBuffer");
            D3D11_BLEND_DESC b{};
            auto& rt = b.RenderTarget[0];
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D11_BLEND_ONE;
            rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            rt.BlendOp = D3D11_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D11_BLEND_ONE;
            rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            check_hresult(device->CreateBlendState(&b, &blend), "CreateBlendState");
            D3D11_RASTERIZER_DESC r{};
            r.FillMode = D3D11_FILL_SOLID;
            r.CullMode = D3D11_CULL_NONE;
            r.DepthClipEnable = TRUE;
            r.ScissorEnable = TRUE;
            check_hresult(device->CreateRasterizerState(&r, &raster), "CreateRasterizerState");
            D3D11_DEPTH_STENCIL_DESC d{};
            check_hresult(device->CreateDepthStencilState(&d, &depth), "CreateDepthStencilState");
            D3D11_SAMPLER_DESC s{};
            s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            s.MaxLOD = D3D11_FLOAT32_MAX;
            check_hresult(device->CreateSamplerState(&s, &sampler), "CreateSamplerState");
            output = surface(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
            intermediate = surface(DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT);
            group = surface(DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT);
            D3D11_TEXTURE2D_DESC sd{};
            output.texture->GetDesc(&sd);
            sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            check_hresult(device->CreateTexture2D(&sd, nullptr, &staging), "Create staging");
            sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            check_hresult(device->CreateTexture2D(&sd, nullptr, &alpha_staging), "Create alpha staging");
            const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
            check_hresult(device->CreateQuery(&query, &ui_done), "UI completion query");
            check_hresult(device->CreateQuery(&query, &host_done), "Host completion query");
        }

        ~Experiment();

        Surface surface(DXGI_FORMAT storage, DXGI_FORMAT view) {
            Surface result;
            D3D11_TEXTURE2D_DESC d{};
            d.Width = width;
            d.Height = height;
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.SampleDesc.Count = 1;
            d.Format = storage;
            d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            check_hresult(device->CreateTexture2D(&d, nullptr, &result.texture), "Create surface");
            D3D11_RENDER_TARGET_VIEW_DESC r{};
            r.Format = view;
            r.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            check_hresult(device->CreateRenderTargetView(result.texture.Get(), &r, &result.rtv), "Create RTV");
            D3D11_SHADER_RESOURCE_VIEW_DESC s{};
            s.Format = view;
            s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            s.Texture2D.MipLevels = 1;
            check_hresult(device->CreateShaderResourceView(result.texture.Get(), &s, &result.srv), "Create SRV");
            return result;
        }

        std::uint64_t asset_identity(const std::shared_ptr<const Asset>& source) {
            if (const auto i = asset_identities.find(source.get()); i != asset_identities.end()) {
                if (auto previous = i->second.first.lock(); previous && previous == source) {
                    return i->second.second;
                }
                asset_identities.erase(i); // Address reuse cannot alias an old version.
            }
            // Weak metadata retains no CPU bitmap and keeps all live identities stable.
            if (asset_identities.size() >= caseelephant::probe_constants::asset_identity_prune_threshold) {
                std::erase_if(asset_identities, [](const auto& entry) {
                    return entry.second.first.expired();
                });
            }
            require(asset_identities.size() < caseelephant::probe_constants::max_asset_identities,
                    "live asset identity metadata bound");
            require(next_asset_identity != UINT64_MAX, "asset identity generation bound");
            const auto id = next_asset_identity++;
            asset_identities.emplace(source.get(), std::pair{std::weak_ptr<const Asset>(source), id});
            return id;
        }

        void evict_assets(std::size_t required = 0, bool all = false) {
            require(!submitted || acknowledged, "GPU cache retirement requires actual completion");
            require(!all || !async_in_flight, "GPU cache drain requires all asynchronous host completions");
            require(required <= cache_byte_limit, "GPU cache resource byte bound");
            while (!asset_cache.empty() &&
                   (all || asset_cache.size() >= cache_resource_limit || required > cache_byte_limit - cached_bytes)) {
                auto victim = asset_cache.end();
                for (auto i = asset_cache.begin(); i != asset_cache.end(); ++i) {
                    ResourceInfo resource_state;
                    lifetime(tracker.InspectResource(i->second.native.id, resource_state), "inspect cache eviction");
                    if (resource_state.cpu_leases == 1 && !resource_state.prepared_pins &&
                        !resource_state.encoded_pins && !resource_state.submitted_pins &&
                        (victim == asset_cache.end() || i->second.touched < victim->second.touched)) {
                        victim = i;
                    }
                }
                require(victim != asset_cache.end(), "bounded cache cannot evict resources in current full packet");
                lifetime(tracker.ReleaseResource(victim->second.native.id), "release completed GPU cache lease");
                cached_bytes -= victim->second.bytes;
                asset_cache.erase(victim);
                ++native_releases;
            }
            if (all) {
                collect();
                assets.clear();
                asset_identities.clear();
            }
        }

        PreparedFrame prepare(FramePacket packet,
                              const std::vector<std::pair<std::uint64_t, std::uint64_t>>& keys = {}) {
            PreparedFrame frame;
            frame.snapshot = std::move(packet);
            frame.dependencies.reserve(frame.snapshot.assets.size());
            assets.reserve(frame.snapshot.assets.size());

            struct ExistingLeases {
                Experiment& renderer;
                std::vector<std::optional<ResourceId>> values;

                ~ExistingLeases() {
                    for (const auto& id : values) {
                        if (id && renderer.tracker.ReleaseResource(*id) != LifetimeResult::Ok) {
                            std::terminate();
                        }
                    }
                }
            } existing{*this, std::vector<std::optional<ResourceId>>(cache_assets ? keys.size() : 0)};

            // Protect the ENTIRE incoming dependency set before creating anything.
            // LRU cannot evict a later draw's hit while processing an earlier miss.
            if (cache_assets) {
                for (std::size_t i = 0; i < keys.size(); ++i) {
                    if (const auto found = asset_cache.find(keys[i]); found != asset_cache.end()) {
                        lifetime(tracker.RetainResource(found->second.native.id),
                                 "pin all incoming cached dependencies");
                        existing.values[i] = found->second.native.id;
                    }
                }
            }
            for (std::size_t i = 0; i < frame.snapshot.assets.size(); ++i) {
                const auto& a = *frame.snapshot.assets[i];
                NativeAsset n;
                const bool r8 = a.encoding != Encoding::SrgbStraight;
                const auto key =
                    cache_assets ? keys.at(i) : std::pair<std::uint64_t, std::uint64_t>{i + 1, window_serial + 1};
                if (cache_assets) {
                    if (auto found = asset_cache.find(key); found != asset_cache.end()) {
                        auto& cached = found->second;
                        ResourceInfo resource_state;
                        lifetime(tracker.InspectResource(cached.native.id, resource_state),
                                 "inspect cached immutable asset");
                        n.id = cached.native.id;
                        n.texture = cached.native.texture;
                        n.srv = cached.native.srv;
                        n.pitch = cached.native.pitch;
                        n.cache_copy = true;
                        if (resource_state.phase != ResourcePhase::Ready &&
                            resource_state.phase != ResourcePhase::Submitted) {
                            n.upload = cached.native.upload;
                        }
                        require(existing.values[i] && *existing.values[i] == n.id, "complete cached dependency lease");
                        cached.touched = ++cache_clock;
                        ++cache_hits;
                        frame.dependencies.push_back(n.id);
                        assets.push_back(std::move(n));
                        existing.values[i].reset();
                        continue;
                    }
                }
                require(a.width && a.height &&
                            a.bytes.size() == static_cast<std::size_t>(a.width) * a.height *
                                                  (r8 ? 1 : caseelephant::constants::rgba8_bytes_per_pixel),
                        "asset dimensions/encoding");
                if (a.encoding == Encoding::SignedDistance) {
                    require(a.spread >= caseelephant::constants::render::min_sdf_spread &&
                                a.spread <= caseelephant::constants::render::max_sdf_spread,
                            "distance spread metadata");
                }
                // sRGB textures decode before filtering; exact for opaque inputs.
                // Translucent inputs keep linear premultiplied floats to avoid halos.
                bool opaque = !r8;
                for (std::size_t p = 3; opaque && p < a.bytes.size(); p += 4) {
                    opaque = a.bytes[p] == caseelephant::constants::color_channel_max;
                }
                if (r8) {
                    n.upload = a.bytes;
                    n.pitch = a.width;
                } else if (opaque) {
                    n.upload = a.bytes;
                    n.pitch = a.width * caseelephant::constants::rgba8_bytes_per_pixel;
                } else {
                    std::vector<Color> pixels(static_cast<std::size_t>(a.width) * a.height);
                    for (std::size_t p = 0; p < pixels.size(); ++p) {
                        const float alpha = a.bytes[p * 4 + 3] / caseelephant::probe_constants::color_channel_max_float;
                        // Source channels are bytes: cache the exact reference decode for
                        // every possible value instead of evaluating pow per pixel.
                        static const auto transfer = [] {
                            std::array<float, 256> table{};
                            for (unsigned i = 0; i < 256; ++i) {
                                table[i] = decode(i / caseelephant::probe_constants::color_channel_max_float);
                            }
                            return table;
                        }();
                        pixels[p] = {transfer[a.bytes[p * 4]] * alpha, transfer[a.bytes[p * 4 + 1]] * alpha,
                                     transfer[a.bytes[p * 4 + 2]] * alpha, alpha};
                    }
                    n.upload.resize(pixels.size() * sizeof(Color));
                    std::memcpy(n.upload.data(), pixels.data(), n.upload.size());
                    n.pitch = a.width * sizeof(Color);
                }
                // Capacity failure must happen before allocating a tracked resource.
                // All incoming hits are already pinned; a full packet that exceeds
                // the cache cannot leave an unowned CPU lease behind on rejection.
                if (cache_assets) {
                    evict_assets(n.upload.size());
                }
                D3D11_TEXTURE2D_DESC d{};
                d.Width = a.width;
                d.Height = a.height;
                d.MipLevels = 1;
                d.ArraySize = 1;
                d.SampleDesc.Count = 1;
                d.Format = r8       ? DXGI_FORMAT_R8_UNORM
                           : opaque ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                    : DXGI_FORMAT_R32G32B32A32_FLOAT;
                d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                check_hresult(device->CreateTexture2D(&d, nullptr, &n.texture), "Create input texture");
                check_hresult(device->CreateShaderResourceView(n.texture.Get(), nullptr, &n.srv), "Create input SRV");
                lifetime(tracker.CreateResource({key.first, key.second, n.upload.size(), ResourceKind::Texture}, n.id),
                         "CreateResource");
                ++native_creations;
                if (cache_assets) {
                    const auto bytes = n.upload.size();
                    const auto resource = n.id;
                    try {
                        asset_cache.emplace(key, CachedAsset{std::move(n), bytes, ++cache_clock});
                    } catch (...) {
                        lifetime(tracker.ReleaseResource(resource), "failed cache insertion rollback");
                        ++native_releases;
                        throw;
                    }
                    auto& cached = asset_cache.at(key);
                    cached_bytes += bytes;
                    peak_cached_bytes = std::max(peak_cached_bytes, cached_bytes);
                    n.id = cached.native.id;
                    n.texture = cached.native.texture;
                    n.srv = cached.native.srv;
                    n.upload = cached.native.upload;
                    n.pitch = cached.native.pitch;
                    n.cache_copy = true;
                    lifetime(tracker.RetainResource(n.id), "new cached frame lease");
                }
                frame.dependencies.push_back(n.id);
                assets.push_back(std::move(n));
            }
            lifetime(tracker.PrepareFrame(frame.dependencies, frame.id), "PrepareFrame");
            return frame;
        }

        void collect() {
            for (auto& a : assets) {
                ResourceInfo state;
                if (a.texture && tracker.InspectResource(a.id, state) == LifetimeResult::StaleHandle) {
                    a.srv.Reset();
                    a.texture.Reset();
                    a.upload.clear();
                    if (!a.cache_copy) {
                        ++native_releases;
                    }
                }
            }
            for (auto& [key, cached] : asset_cache) {
                ResourceInfo resource_state;
                if (tracker.InspectResource(cached.native.id, resource_state) == LifetimeResult::Ok &&
                    resource_state.phase == ResourcePhase::Ready && !cached.native.upload.empty()) {
                    std::vector<std::uint8_t>{}.swap(cached.native.upload);
                }
            }
        }

        void bind_target(Surface& target) {
            ID3D11ShaderResourceView* empty{};
            deferred->PSSetShaderResources(0, 1, &empty);
            auto* rtv = target.rtv.Get();
            deferred->OMSetRenderTargets(1, &rtv, nullptr);
        }

        void draw(const Draw& d, ID3D11ShaderResourceView* texture, UINT material, float spread = 0) {
            Color color = d.srgb;
            for (int c = 0; c < 3; ++c) {
                color[c] = decode(color[c]) * color[3];
            }
            Color outline = d.outline;
            for (int c = 0; c < 3; ++c) {
                outline[c] = decode(outline[c]) * outline[3];
            }
            require(d.stroke >= 0 && (material != caseelephant::probe_constants::shader_material::signed_distance ||
                                      d.stroke < spread - 1),
                    "SDF stroke exceeds distance padding");
            Constants data{{d.rect.x, d.rect.y, d.rect.w, d.rect.h},
                           {static_cast<float>(width), static_cast<float>(height)},
                           material,
                           d.opacity,
                           color,
                           d.uv,
                           {spread, d.stroke, 0, 0},
                           outline};
            deferred->UpdateSubresource(constants.Get(), 0, nullptr, &data, 0, 0);
            deferred->PSSetShaderResources(0, 1, &texture);
            D3D11_RECT clip{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
            if (d.clipped) {
                clip = {static_cast<LONG>(d.rect.x + caseelephant::probe_constants::scissor_test_inset),
                        static_cast<LONG>(d.rect.y + caseelephant::probe_constants::scissor_test_inset),
                        static_cast<LONG>(d.rect.x + d.rect.w - caseelephant::probe_constants::scissor_test_inset),
                        static_cast<LONG>(d.rect.y + d.rect.h - caseelephant::probe_constants::scissor_test_inset)};
            }
            deferred->RSSetScissorRects(1, &clip);
            deferred->Draw(caseelephant::probe_constants::quad_vertex_count, 0);
        }

        Encoded encode(const PreparedFrame& frame, bool rtt, bool fail_after_upload = false,
                       ID3D11DeviceContext* native_context = nullptr, ID3D11RenderTargetView* native_target = nullptr,
                       bool clear_native_target = true, const float* native_clear = nullptr) {
            struct RestoreContext {
                Experiment& renderer;
                ComPtr<ID3D11DeviceContext> previous;

                ~RestoreContext() {
                    renderer.deferred = std::move(previous);
                }
            } restore{*this, deferred};

            if (native_context) {
                deferred = native_context;
            }
            Encoded encoded;
            lifetime(tracker.EncodeFrame(frame.id, encoded.token), "EncodeFrame before recording");
            try {
                deferred->ClearState();
                for (auto& a : assets) {
                    ResourceInfo resource_state;
                    lifetime(tracker.InspectResource(a.id, resource_state), "inspect upload readiness");
                    // A submitted upload is usable by later work on this same
                    // queue, while an unsubmitted/cancelled upload is not.
                    if (resource_state.phase != ResourcePhase::Ready &&
                        resource_state.phase != ResourcePhase::Submitted) {
                        require(!a.upload.empty(), "created resource retains retry upload");
                        deferred->UpdateSubresource(a.texture.Get(), 0, nullptr, a.upload.data(), a.pitch, 0);
                        ++uploads;
                    }
                }
                if (fail_after_upload) {
                    throw std::runtime_error("injected recording failure");
                }
                const float transparent[4]{};
                auto& destination = rtt ? intermediate : output;
                if (native_target) {
                    ID3D11ShaderResourceView* empty{};
                    deferred->PSSetShaderResources(0, 1, &empty);
                    deferred->OMSetRenderTargets(1, &native_target, nullptr);
                    if (clear_native_target) {
                        deferred->ClearRenderTargetView(native_target, native_clear ? native_clear : transparent);
                    }
                } else {
                    bind_target(destination);
                    deferred->ClearRenderTargetView(destination.rtv.Get(), transparent);
                }
                D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
                deferred->RSSetViewports(1, &viewport);
                deferred->RSSetState(raster.Get());
                deferred->OMSetDepthStencilState(depth.Get(), 0);
                deferred->OMSetBlendState(blend.Get(), nullptr, D3D11_DEFAULT_SAMPLE_MASK);
                deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                deferred->VSSetShader(vs.Get(), nullptr, 0);
                deferred->PSSetShader(ps.Get(), nullptr, 0);
                auto* cb = constants.Get();
                deferred->VSSetConstantBuffers(0, 1, &cb);
                deferred->PSSetConstantBuffers(0, 1, &cb);
                auto* ss = sampler.Get();
                deferred->PSSetSamplers(0, 1, &ss);
                for (const auto& d : frame.snapshot.draws) {
                    if (d.group) {
                        bind_target(group);
                        deferred->ClearRenderTargetView(group.rtv.Get(), transparent);
                        Draw red = d;
                        red.group = false;
                        red.opacity = 1;
                        red.srgb = {1, 0, 0, 1};
                        draw(red, nullptr, caseelephant::probe_constants::shader_material::solid);
                        Draw blue = red;
                        blue.srgb = {0, 0, 1, 1};
                        draw(blue, nullptr, caseelephant::probe_constants::shader_material::solid);
                        bind_target(destination);
                        Draw composite = d;
                        composite.uv = {d.rect.x / width, d.rect.y / height, d.rect.w / width, d.rect.h / height};
                        draw(composite, group.srv.Get(), caseelephant::probe_constants::shader_material::texture);
                    } else {
                        if (d.asset >= 0 && size_t(d.asset) >= frame.snapshot.assets.size()) {
                            const auto& external = frame.external.at(size_t(d.asset) - frame.snapshot.assets.size());
                            draw(d, external.srv.Get(),
                                 external.bottom_left
                                     ? caseelephant::probe_constants::shader_material::texture_bottom_up
                                     : caseelephant::probe_constants::shader_material::texture);
                            continue;
                        }
                        auto* texture = d.asset < 0 ? nullptr : assets[static_cast<std::size_t>(d.asset)].srv.Get();
                        const auto* asset =
                            d.asset < 0 ? nullptr : frame.snapshot.assets[static_cast<std::size_t>(d.asset)].get();
                        const UINT material = !asset ? caseelephant::probe_constants::shader_material::solid
                                              : asset->encoding == Encoding::Coverage
                                                  ? caseelephant::probe_constants::shader_material::coverage
                                              : asset->encoding == Encoding::SignedDistance
                                                  ? caseelephant::probe_constants::shader_material::signed_distance
                                                  : caseelephant::probe_constants::shader_material::texture;
                        draw(d, texture, material, asset ? asset->spread : 0);
                    }
                }
                if (rtt) {
                    bind_target(output);
                    deferred->ClearRenderTargetView(output.rtv.Get(), transparent);
                    draw({{0, 0, static_cast<float>(width), static_cast<float>(height)}}, intermediate.srv.Get(),
                         caseelephant::probe_constants::shader_material::texture);
                }
                // A transparent linear destination checks premultiplied RGB/alpha
                // independently of the opaque screenshot's alpha=1 background.
                if (!native_target) {
                    bind_target(group);
                    deferred->ClearRenderTargetView(group.rtv.Get(), transparent);
                    draw({{0, 0, static_cast<float>(width), static_cast<float>(height)}, {0, 1, 0, 0.5f}}, nullptr,
                         caseelephant::probe_constants::shader_material::solid);
                }
                deferred->ClearState();
                check_hresult(deferred->FinishCommandList(FALSE, &encoded.commands), "FinishCommandList");
                return encoded;
            } catch (...) {
                // Finish and destroy the partially recorded list BEFORE unpinning.
                deferred->ClearState();
                ComPtr<ID3D11CommandList> discarded;
                check_hresult(deferred->FinishCommandList(FALSE, &discarded), "Discard partial command list");
                discarded.Reset();
                lifetime(tracker.DiscardEncoded(encoded.token), "Discard failed recording");
                throw;
            }
        }

        void complete() {
            const auto deadline =
                std::chrono::steady_clock::now() +
                std::chrono::seconds(static_cast<int>(caseelephant::probe_constants::completion_timeout_seconds));
            immediate->Flush();
            BOOL done = FALSE;
            for (;;) {
                const auto hr =
                    immediate->GetData(host_done.Get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH);
                check_hresult(hr, "GetData host completion");
                if (hr == S_OK && done) {
                    break;
                }
                require(std::chrono::steady_clock::now() < deadline, "GPU completion deadline");
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(static_cast<int>(caseelephant::probe_constants::completion_poll_ms)));
            }
            BOOL ui = FALSE;
            check_hresult(immediate->GetData(ui_done.Get(), &ui, sizeof(ui), D3D11_ASYNC_GETDATA_DONOTFLUSH),
                          "UI completion");
            require(ui != FALSE, "host completion did not include UI");
            if (backend) {
                complete_backend();
            } else {
                lifetime(tracker.NotifyCompletedSerial(caseelephant::probe_constants::device_epoch,
                                                       caseelephant::probe_constants::immediate_queue,
                                                       pending_completion),
                         "NotifyCompletedSerial actual shared host query");
            }
            acknowledged = true;
            collect();
        }

        std::vector<Pixel> execute(bool rtt, FramePacket packet = scene()) {
            auto frame = prepare(std::move(packet));
            // Cancellation and failure must not make unexecuted uploads Ready.
            auto cancelled = encode(frame, rtt);
            cancelled.commands.Reset();
            lifetime(tracker.DiscardEncoded(cancelled.token), "Cancel before submit");
            bool failure{};
            try {
                (void)encode(frame, rtt, true);
            } catch (const std::runtime_error& e) {
                failure = std::string(e.what()) == "injected recording failure";
            }
            test(failure, "partial recording failure cleanup");
            for (const auto& a : assets) {
                ResourceInfo state;
                lifetime(tracker.InspectResource(a.id, state), "Inspect after cancellation");
                test(state.phase == ResourcePhase::Created && state.encoded_pins == 0, "cancelled upload is not Ready");
            }
            auto encoded = encode(frame, rtt);
            UseTokenInfo token;
            lifetime(tracker.InspectUseToken(encoded.token, token), "Inspect upload plan");
            test(token.upload_count == assets.size(), "independent retry includes every upload");
            lifetime(tracker.ReleasePreparedFrame(frame.id), "ReleasePreparedFrame");
            frame.snapshot = {};
            for (const auto& a : assets) {
                lifetime(tracker.ReleaseResource(a.id), "Release CPU asset lease");
            }
            collect();
            test(native_releases == 0, "encoded pins survive CPU/prepared release");
            const D3D11_VIEWPORT host_viewport{3, 4, 17, 19, 0, 1};
            const D3D11_RECT host_scissor{5, 6, 15, 16};
            immediate->RSSetViewports(1, &host_viewport);
            immediate->RSSetScissorRects(1, &host_scissor);
            immediate->ExecuteCommandList(encoded.commands.Get(), TRUE);
            immediate->End(ui_done.Get());
            // Issue the diagnostic host completion marker before any throwing
            // post-submit assertion; failure cleanup can then drain actual work.
            immediate->CopyResource(staging.Get(), output.texture.Get());
            immediate->CopyResource(alpha_staging.Get(), group.texture.Get());
            immediate->End(host_done.Get());
            submitted = true;
            lifetime(tracker.NotifySubmitted(encoded.token, caseelephant::probe_constants::immediate_queue,
                                             caseelephant::probe_constants::fixture_ui_serial),
                     "NotifySubmitted after ExecuteCommandList");
            encoded.commands.Reset();
            D3D11_VIEWPORT restored_viewport{};
            UINT viewports = 1;
            immediate->RSGetViewports(&viewports, &restored_viewport);
            D3D11_RECT restored_scissor{};
            UINT scissors = 1;
            immediate->RSGetScissorRects(&scissors, &restored_scissor);
            test(viewports == 1 && std::memcmp(&restored_viewport, &host_viewport, sizeof(host_viewport)) == 0,
                 "host viewport restored");
            test(scissors == 1 && std::memcmp(&restored_scissor, &host_scissor, sizeof(host_scissor)) == 0,
                 "host scissor restored");
            // The host-owned copies have serial 43 and no UI use token.
            test(tracker.Shutdown() == LifetimeResult::Busy, "shutdown must drain actual submission");
            collect();
            test(native_releases == 0, "submission retains native textures until acknowledgement");
            for (const auto& a : assets) {
                ResourceInfo state;
                lifetime(tracker.InspectResource(a.id, state), "Inspect submitted texture");
                test(state.cpu_leases == 0 && state.prepared_pins == 0 && state.submitted_pins == 1,
                     "only submitted pin remains");
            }
            complete();
            test(native_releases == assets.size(), "native registry releases after actual completion");
            test(tracker.Stats().completed_serial == caseelephant::probe_constants::fixture_host_serial &&
                     tracker.Stats().last_submitted_serial == caseelephant::probe_constants::fixture_ui_serial,
                 "shared queue progresses beyond UI serial");
            test(tracker.Shutdown() == LifetimeResult::Ok, "shutdown drained");
            std::vector<Pixel> pixels(static_cast<std::size_t>(width) * height);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check_hresult(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map completed readback");
            for (UINT y = 0; y < height; ++y) {
                std::memcpy(pixels.data() + static_cast<std::size_t>(y) * width,
                            static_cast<const std::uint8_t*>(mapped.pData) +
                                static_cast<std::size_t>(y) * mapped.RowPitch,
                            width * sizeof(Pixel));
            }
            immediate->Unmap(staging.Get(), 0);
            check_hresult(immediate->Map(alpha_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                          "Map transparent readback");
            std::memcpy(transparent_sample.data(), mapped.pData, sizeof(Color));
            immediate->Unmap(alpha_staging.Get(), 0);
            const Color expected_alpha{0, 0.5f, 0, 0.5f};
            for (std::size_t c = 0; c < 4; ++c) {
                test(std::abs(transparent_sample[c] - expected_alpha[c]) <
                         caseelephant::probe_constants::linear_alpha_tolerance,
                     "transparent linear premultiplied RGBA");
            }
            immediate->ClearState();
            return pixels;
        }

        void test(bool condition, const char* message) {
            require(condition, message);
            ++assertions;
        }
        template <class Present>
        void render_window(FramePacket packet, Present&& present);
        template <class Present>
        void compose_window(Present&& present);

        void enable_timing() {
            D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            check_hresult(device->CreateQuery(&desc, &timing_disjoint), "timing disjoint");
            desc.Query = D3D11_QUERY_TIMESTAMP;
            check_hresult(device->CreateQuery(&desc, &timing_start), "timing start");
            check_hresult(device->CreateQuery(&desc, &timing_end), "timing end");
        }

        void check_debug_messages() {
            if (!info) {
                return;
            }
            const auto count = info->GetNumStoredMessagesAllowedByRetrievalFilter();
            for (UINT64 i = 0; i < count; ++i) {
                SIZE_T size{};
                check_hresult(info->GetMessage(i, nullptr, &size), "InfoQueue message size");
                std::vector<std::uint8_t> bytes(size);
                auto* m = reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
                check_hresult(info->GetMessage(i, m, &size), "InfoQueue message");
                if (m->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
                    throw std::runtime_error(m->pDescription);
                }
            }
        }
    };

#include "probe_render_backend.hpp"

    struct Check {
        const char* id;
        UINT x;
        UINT y;
        Pixel expected;
    };

    const std::array<Check, 10> checks{{{"white-half", 32, 32, {188, 188, 188, 255}},
                                        {"ordered-red-blue", 96, 32, {137, 0, 188, 255}},
                                        {"srgb-gray-input", 160, 32, {128, 128, 128, 255}},
                                        {"coverage-128", 224, 32, {188, 188, 188, 255}},
                                        {"parent-opacity-group", 32, 96, {0, 0, 188, 255}},
                                        {"premultiplied-image-edge", 96, 96, {188, 0, 0, 255}},
                                        {"actual-freetype-glyph", 160, 96, {255, 255, 255, 255}},
                                        {"glyph-exterior", 136, 72, {0, 0, 0, 255}},
                                        {"scissor-interior", 224, 96, {255, 0, 0, 255}},
                                        {"scissor-exterior", 200, 72, {0, 0, 0, 255}}}};

    std::span<const std::uint8_t> pixel_bytes(const std::vector<Pixel>& pixels) {
        return {reinterpret_cast<const std::uint8_t*>(pixels.data()), pixels.size() * sizeof(Pixel)};
    }

    void save_png(const std::filesystem::path& path, const std::vector<Pixel>& pixels,
                  UINT w = caseelephant::probe_constants::color_fixture_width,
                  UINT h = caseelephant::probe_constants::color_fixture_height) {
        caseelephant::testing::write_png(path, w, h, pixel_bytes(pixels));
    }

    std::string rgba(Pixel p) {
        std::ostringstream s;
        s << '[' << static_cast<int>(p[0]) << ',' << static_cast<int>(p[1]) << ',' << static_cast<int>(p[2]) << ','
          << static_cast<int>(p[3]) << ']';
        return s.str();
    }

    std::string linear_rgba(Color c) {
        std::ostringstream s;
        s << std::setprecision(caseelephant::probe_constants::report_significant_digits) << '[' << c[0] << ',' << c[1]
          << ',' << c[2] << ',' << c[3] << ']';
        return s.str();
    }

    std::vector<Pixel> read_png(const std::filesystem::path& path) {
        const auto image = caseelephant::testing::read_png(path);
        require(image.width == width && image.height == height, "Browser capture must be 256x128 at DPR 1");
        std::vector<Pixel> pixels(static_cast<std::size_t>(image.width) * image.height);
        std::memcpy(pixels.data(), image.rgba.data(), image.rgba.size());
        return pixels;
    }

    int capture_browser(const std::filesystem::path& png, const std::filesystem::path& output) {
        const auto pixels = read_png(png);
        std::ostringstream report;
        report
            << "{\"fixture\":\"caseelephant-d3d11-color-v1\",\"width\":256,\"height\":128,\"source\":\"actual Chrome PNG capture\",\"samples\":[";
        bool comma{};
        for (const auto& check : checks) {
            if (comma) {
                report << ',';
            }
            comma = true;
            report << "{\"id\":" << json_string(check.id)
                   << ",\"rgba\":" << rgba(pixels[static_cast<std::size_t>(check.y) * width + check.x]) << '}';
        }
        report << "]}\n";
        std::filesystem::create_directories(output.parent_path());
        std::ofstream file(output, std::ios::binary);
        require(bool(file << report.str()), "write browser color samples");
        std::cout << "Captured " << checks.size() << " actual browser PNG samples\n";
        return 0;
    }

    int run(bool warp, const std::filesystem::path& folder) {
        std::filesystem::create_directories(folder);
        Experiment direct(warp);
        Experiment rtt(warp);
        auto direct_pixels = direct.execute(false);
        auto rtt_pixels = rtt.execute(true);
        std::ostringstream report;
        report
            << "{\"fixture\":\"caseelephant-d3d11-color-v1\",\"driver\":" << json_string(warp ? "warp" : "hardware")
            << ",\"adapter\":" << json_string(direct.adapter)
            << ",\"feature_level\":\"11_0\",\"debug_layer\":" << (direct.debug_layer ? "true" : "false")
            << ",\"width\":256,\"height\":128,\"color_contract\":\"linear-srgb-premultiplied\",\"tolerance_rgba8\":2,\"checks\":[";
        bool comma{};
        for (const auto& check : checks) {
            const auto p = direct_pixels[static_cast<std::size_t>(check.y) * width + check.x];
            const auto q = rtt_pixels[static_cast<std::size_t>(check.y) * width + check.x];
            for (std::size_t c = 0; c < 4; ++c) {
                direct.test(std::abs(int(p[c]) - int(check.expected[c])) <=
                                (c == 3 ? 0 : caseelephant::probe_constants::rgba8_parity_tolerance),
                            check.id);
                rtt.test(std::abs(int(q[c]) - int(check.expected[c])) <=
                             (c == 3 ? 0 : caseelephant::probe_constants::rgba8_parity_tolerance),
                         check.id);
            }
            if (comma) {
                report << ',';
            }
            comma = true;
            report << "{\"id\":" << json_string(check.id) << ",\"expected\":" << rgba(check.expected)
                   << ",\"direct\":" << rgba(p) << ",\"rtt\":" << rgba(q) << '}';
        }
        int max_difference{};
        for (std::size_t i = 0; i < direct_pixels.size(); ++i) {
            for (std::size_t c = 0; c < 4; ++c) {
                max_difference = std::max(max_difference, std::abs(int(direct_pixels[i][c]) - int(rtt_pixels[i][c])));
            }
        }
        direct.test(max_difference <= caseelephant::probe_constants::rgba8_parity_tolerance,
                    "full-image direct/RTT parity");
        direct.check_debug_messages();
        rtt.check_debug_messages();
        report
            << "],\"direct_rtt_max_difference\":" << max_difference
            << ",\"assertions\":" << direct.assertions + rtt.assertions
            << ",\"native_textures_retired\":" << direct.native_releases + rtt.native_releases
            << ",\"transparent_linear_direct\":" << linear_rgba(direct.transparent_sample)
            << ",\"transparent_linear_rtt\":" << linear_rgba(rtt.transparent_sample)
            << ",\"debug_warning_error_count\":" << (direct.debug_layer ? "0" : "null")
            << ",\"actual_completion_query\":true,\"ui_serial\":42,\"host_completed_serial\":43,\"dom_pipeline_verified\":false,\"mobile_verified\":false,\"passed\":true}\n";
        save_png(folder / "direct.png", direct_pixels);
        save_png(folder / "rtt.png", rtt_pixels);
        std::ofstream file(folder / "report.json", std::ios::binary);
        require(bool(file << report.str()), "write GPU report");
        std::cout << "PASS D3D11 " << (warp ? "WARP" : "hardware") << ": " << direct.adapter << ", "
                  << direct.assertions + rtt.assertions
                  << " assertions; real query completion; direct/RTT max difference " << max_difference
                  << "; debug layer " << (direct.debug_layer ? "on" : "unavailable") << '\n';
        return 0;
    }

    float overlap(float pixel, float start, float length) {
        return std::clamp(std::min(pixel + 1, start + length) - std::max(pixel, start), 0.0f, 1.0f);
    }

    int run_sdf(bool warp, const std::filesystem::path& folder) {
        std::filesystem::create_directories(folder);
        const auto scene = sdf_scene();
        Experiment direct(warp);
        Experiment rtt(warp);
        const auto p = direct.execute(false, scene.packet);
        const auto q = rtt.execute(true, scene.packet);
        float max_edge_error{};
        std::size_t edge_samples{};
        std::size_t partial_samples{};
        std::size_t latin_pixels{};
        int max_difference{};
        for (std::size_t i = 0; i < p.size(); ++i) {
            direct.test(p[i][3] == caseelephant::constants::color_channel_max &&
                            q[i][3] == caseelephant::constants::color_channel_max,
                        "SDF opaque background alpha");
            for (std::size_t c = 0; c < 4; ++c) {
                max_difference = std::max(max_difference, std::abs(int(p[i][c]) - int(q[i][c])));
            }
        }
        // Independent area-coverage oracle for axis-aligned straight square edges.
        // Exclude two-edge corner neighborhoods: a single-channel field rounds corners.
        for (const auto& r : scene.squares) {
            float case_error{};
            std::size_t partial{};
            for (int y = static_cast<int>(r.y) - caseelephant::probe_constants::sdf_edge_scan_margin;
                 y < r.y + r.h + caseelephant::probe_constants::sdf_edge_scan_margin; ++y) {
                for (int x = static_cast<int>(r.x) - caseelephant::probe_constants::sdf_edge_scan_margin;
                     x < r.x + r.w + caseelephant::probe_constants::sdf_edge_scan_margin; ++x) {
                    const bool near_x =
                        std::abs(x + 0.5f - r.x) < caseelephant::probe_constants::sdf_corner_exclusion ||
                        std::abs(x + 0.5f - r.x - r.w) < caseelephant::probe_constants::sdf_corner_exclusion;
                    const bool near_y =
                        std::abs(y + 0.5f - r.y) < caseelephant::probe_constants::sdf_corner_exclusion ||
                        std::abs(y + 0.5f - r.y - r.h) < caseelephant::probe_constants::sdf_corner_exclusion;
                    if (near_x && near_y) {
                        continue;
                    }
                    const float expected =
                        overlap(static_cast<float>(x), r.x, r.w) * overlap(static_cast<float>(y), r.y, r.h);
                    const auto& pixel = p[static_cast<std::size_t>(y) * width + x];
                    direct.test(pixel[0] == pixel[1] && pixel[1] == pixel[2], "SDF square neutral fill");
                    const float error =
                        std::abs(decode(pixel[0] / caseelephant::probe_constants::color_channel_max_float) - expected);
                    case_error = std::max(case_error, error);
                    ++edge_samples;
                    if (expected > 0 && expected < 1) {
                        ++partial;
                        if (pixel[0] > 0 && pixel[0] < caseelephant::constants::color_channel_max) {
                            ++partial_samples;
                        }
                    }
                }
            }
            max_edge_error = std::max(max_edge_error, case_error);
            direct.test(partial > 40, "subpixel square edges sampled");
            if (case_error > caseelephant::probe_constants::sdf_edge_tolerance) {
                std::cerr << "scale=" << r.w / caseelephant::constants::text::sdf_default_base_ppem
                          << " edge error=" << case_error << '\n';
            }
            direct.test(case_error <= caseelephant::probe_constants::sdf_edge_tolerance,
                        "SDF straight-edge area coverage (0.12 linear tolerance)");
        }
        direct.test(partial_samples > 200, "derivative AA produced partial coverage at all scales");
        float stroke_error{};
        for (int x = 208; x < 237; ++x) {
            const auto& pixel = p[24 * width + x];
            const float fill = overlap(static_cast<float>(x), scene.stroked.x, 20);
            const float outer = overlap(static_cast<float>(x), scene.stroked.x - 1, 22);
            stroke_error =
                std::max({stroke_error,
                          std::abs(decode(pixel[0] / caseelephant::probe_constants::color_channel_max_float) - fill),
                          std::abs(decode(pixel[2] / caseelephant::probe_constants::color_channel_max_float) - outer)});
            direct.test(pixel[0] == pixel[1], "blue stroke leaves equal red/green fill");
        }
        direct.test(stroke_error <= caseelephant::probe_constants::sdf_edge_tolerance,
                    "SDF stroke source-distance coverage");
        for (UINT y = 78; y < caseelephant::probe_constants::color_fixture_height; ++y) {
            for (UINT x = 0; x < 210; ++x) {
                if (p[y * width + x][0] > 200) {
                    ++latin_pixels;
                }
            }
        }
        direct.test(latin_pixels > 600, "actual Roboto SDF visible at two sizes");
        for (UINT y = 0; y < height; ++y) {
            for (UINT x = 0; x < width; ++x) {
                bool covered{};
                for (const auto& d : scene.packet.draws) {
                    if (d.asset >= 0 && x + 0.5f >= d.rect.x && y + 0.5f >= d.rect.y &&
                        x + 0.5f < d.rect.x + d.rect.w && y + 0.5f < d.rect.y + d.rect.h) {
                        covered = true;
                    }
                }
                if (!covered) {
                    direct.test(p[y * width + x] == Pixel{0, 0, 0, 255}, "no glyph pixels outside quads");
                }
            }
        }
        direct.test(max_difference <= caseelephant::probe_constants::rgba8_parity_tolerance,
                    "SDF full-image direct/RTT parity");
        direct.check_debug_messages();
        rtt.check_debug_messages();
        save_png(folder / "direct.png", p);
        save_png(folder / "rtt.png", q);
        // Distance diagnostic: encoded byte image, not a coverage/glyph screenshot.
        std::vector<Pixel> distance_image;
        distance_image.reserve(width * height);
        for (const auto value : scene.packet.assets[0]->bytes) {
            distance_image.push_back({value, value, value, 255});
        }
        save_png(folder / "distance-atlas.png", distance_image);
        std::ostringstream report;
        report
            << std::setprecision(caseelephant::probe_constants::report_significant_digits)
            << "{\"fixture\":\"caseelephant-sdf-v1\",\"driver\":" << json_string(warp ? "warp" : "hardware")
            << ",\"adapter\":" << json_string(direct.adapter)
            << ",\"debug_layer\":" << (direct.debug_layer ? "true" : "false")
            << ",\"width\":256,\"height\":128,\"generator\":\"FreeType-2.13.3-bsdf\",\"base_ppem\":40,\"spread_source_texels\":8"
            << ",\"zero_byte\":128,\"encoding\":\"R8_UNORM-linear-distance\",\"atlas_bytes\":32768,\"atlas_mip_levels\":1,\"guard_texels\":1"
            << ",\"generated_glyphs\":" << scene.generated_glyphs
            << ",\"glyph_distance_bytes\":" << scene.distance_bytes
            << ",\"square_scales\":[0.5,1,1.5],\"latin_ppem\":[24,48],\"stroke_source_texels\":2,\"edge_samples\":"
            << edge_samples << ",\"partial_coverage_samples\":" << partial_samples
            << ",\"straight_edge_max_linear_error\":" << max_edge_error
            << ",\"straight_edge_tolerance\":0.12,\"corner_oracle_verified\":false,\"stroke_max_linear_error\":"
            << stroke_error << ",\"latin_visible_pixels\":" << latin_pixels
            << ",\"direct_rtt_max_difference\":" << max_difference
            << ",\"assertions\":" << direct.assertions + rtt.assertions
            << ",\"native_textures_retired\":" << direct.native_releases + rtt.native_releases
            << ",\"actual_completion_query\":true,\"ui_serial\":42,\"host_completed_serial\":43,\"debug_warning_error_count\":"
            << (direct.debug_layer ? "0" : "null")
            << ",\"dom_pipeline_verified\":false,\"mobile_verified\":false,\"msdf_verified\":false,\"passed\":true}\n";
        std::ofstream file(folder / "report.json", std::ios::binary);
        require(bool(file << report.str()), "write SDF report");
        std::cout << "PASS SDF D3D11 " << (warp ? "WARP" : "hardware") << ": " << direct.adapter << "; "
                  << direct.assertions + rtt.assertions << " assertions; straight edge max error " << max_edge_error
                  << "; stroke " << stroke_error << "; direct/RTT " << max_difference << '\n';
        return 0;
    }

    // Evidence text files end with a newline.
    void write_text(const std::filesystem::path& path, const std::string& text) {
        caseelephant::testing::write_file(path, text + '\n');
    }

    FramePacket prepare_page_glyphs(const caseelephant::ProbePage& page, caseelephant::TextProbeFont& font) {
        FramePacket packet;

        struct Cached {
            int asset{-1};
            caseelephant::ProbeGlyphBitmap bitmap;
        };

        std::map<std::pair<std::uint32_t, float>, Cached> cache;
        std::size_t bytes{};
        for (const auto& paint : page.paints) {
            if (paint.kind == caseelephant::ProbePaintKind::Rectangle) {
                packet.draws.push_back({{paint.x, paint.y, paint.width, paint.height}, paint.srgb});
                continue;
            }
            float pen_x = paint.x;
            float pen_y = paint.y;
            for (const auto& glyph : paint.shaped.glyphs) {
                const auto key = std::pair{glyph.glyph_id, paint.font_size};
                if (!cache.contains(key)) {
                    auto bitmap = font.rasterize(glyph.glyph_id, paint.font_size);
                    Cached cached;
                    if (bitmap.width && bitmap.height) {
                        bytes += bitmap.coverage.size();
                        require(bytes <= caseelephant::probe_constants::page_glyph_max_bytes &&
                                    cache.size() < caseelephant::probe_constants::max_frame_assets,
                                "page glyph budget");
                        cached.asset = static_cast<int>(packet.assets.size());
                        packet.assets.push_back(std::make_shared<const Asset>(
                            Asset{bitmap.width, bitmap.height, Encoding::Coverage, bitmap.coverage}));
                    }
                    cached.bitmap = std::move(bitmap);
                    cache.emplace(key, std::move(cached));
                }
                const auto& cached = cache.at(key);
                const auto& b = cached.bitmap;
                if (cached.asset >= 0) {
                    packet.draws.push_back({{pen_x + glyph.offset_x + b.left, pen_y - glyph.offset_y - b.top,
                                             static_cast<float>(b.width), static_cast<float>(b.height)},
                                            paint.srgb,
                                            cached.asset});
                }
                pen_x += glyph.advance_x;
                pen_y -= glyph.advance_y;
            }
        }
        return packet;
    }

    std::string page_json(const caseelephant::ProbePage& page) {
        std::ostringstream s;
        s << std::setprecision(caseelephant::probe_constants::report_significant_digits) << "{\"boxes\":[";
        bool comma{};
        for (const auto& box : page.boxes) {
            if (comma) {
                s << ',';
            }
            comma = true;
            s << "{\"id\":" << json_string(box.id) << ",\"x\":" << box.x << ",\"y\":" << box.y
              << ",\"width\":" << box.width << ",\"height\":" << box.height << '}';
        }
        s << "],\"text_runs\":[";
        comma = false;
        for (const auto& p : page.paints) {
            if (p.kind == caseelephant::ProbePaintKind::Text) {
                if (comma) {
                    s << ',';
                }
                comma = true;
                s << "{\"id\":" << json_string(p.id) << ",\"text\":" << json_string(p.text) << ",\"x\":" << p.x
                  << ",\"baseline\":" << p.y << ",\"glyphs\":" << p.shaped.glyphs.size() << '}';
            }
        }
        return s.str() + "]}";
    }

    void pump_react(caseelephant::ScriptDomProbe& script, unsigned time) {
        for (unsigned batch = 0; batch < caseelephant::constants::script::max_task_pump_turns; ++batch) {
            (void)script.eval("__ceHost.pump(" + std::to_string(time) + ")", "<host-timer-checkpoint>");
            // This separate eval happens after the previous microtask checkpoint.
            if (script.eval("__ceHost.pending()") == "0") {
                return;
            }
        }
        throw std::runtime_error("CE_TASK_LIMIT: React probe did not become idle");
    }

    int run_react(bool warp, const std::filesystem::path& folder) {
        std::filesystem::create_directories(folder);
        caseelephant::ScriptProbeConfig config{caseelephant::constants::script::probe_vm_stack_bytes};
#ifdef CE_REACT_GC_STRESS
        config.execution_budget_ms = caseelephant::constants::script::max_execution_budget_ms;
#endif
        caseelephant::ScriptDomProbe script(config);
        caseelephant::install_script_platform(script, caseelephant::ScriptPlatformModule::WebHost);
        script.eval(read_file(std::filesystem::path(CE_SOURCE_DIR) / "examples/react-vite/native/app.js"),
                    "react-vite-app.js");
        pump_react(script, 0);
        require(script.eval("caseElephantReact.version") == "19.3.0", "fixed real React version");
        require(script.eval("caseElephantReact.errors.length") == "0", "React initial error");
        const auto item = script.node_identity("#item-ember");
        const auto button = script.node_identity("#increment");
        caseelephant::TextProbeFont font(CE_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf");
        std::array<caseelephant::ProbePage, caseelephant::probe_constants::interaction_frames> pages;
        std::array<std::string, caseelephant::probe_constants::interaction_frames> states;
        pages[0] = caseelephant::build_page_probe(script.snapshot_subtree("#page"), font);
        states[0] = script.eval("JSON.stringify(caseElephantReact.snapshot())");
        script.eval("document.getElementById('increment').dispatchEvent(new Event('click',{bubbles:true}));");
        pump_react(script, caseelephant::probe_constants::fixture_frame_ms);
        require(script.eval("document.getElementById('count').textContent") == "Count: 1",
                "React useState click update");
        require(script.node_identity("#increment") == button, "React state update replaced button identity");
        pages[1] = caseelephant::build_page_probe(script.snapshot_subtree("#page"), font);
        states[1] = script.eval("JSON.stringify(caseElephantReact.snapshot())");
        script.eval("document.getElementById('reverse').dispatchEvent(new Event('click',{bubbles:true}));");
        pump_react(script, 2 * caseelephant::probe_constants::fixture_frame_ms);
        require(script.eval("caseElephantReact.snapshot().order.join(',')") == "moss,tide,ember",
                "React keyed reorder");
        require(script.node_identity("#item-ember") == item, "keyed update did not retain native node identity");
        require(script.eval("caseElephantReact.errors.length") == "0" && script.stats().listener_exceptions == 0,
                "React interaction error");
        pages[2] = caseelephant::build_page_probe(script.snapshot_subtree("#page"), font);
        states[2] = script.eval("JSON.stringify(caseElephantReact.snapshot())");
        write_text(folder / "native-dom.json", script.dump_document());
        script.eval("caseElephantReact.unmount();");
        pump_react(script, 3 * caseelephant::probe_constants::fixture_frame_ms);
        script.collect_garbage();
        require(script.eval("document.getElementById('root').childNodes.length") == "0", "React root unmount");
        script.reset_document(); // GPU and page value snapshots cannot depend on the VM.
        require(!script.is_live(item), "page epoch reset left old identity alive");
        std::ostringstream report;
        report << "{\"fixture\":\"caseelephant-react-vite-v1\",\"react\":\"19.3.0\",\"vite\":\"8.3.3\",\"driver\":"
               << json_string(warp ? "warp" : "hardware") << ",\"script_budget_ms\":" << config.execution_budget_ms
               << ",\"frames\":[";
        std::vector<Pixel> initial_pixels;
        std::vector<Pixel> count_pixels;
        std::size_t retired{};
        std::size_t assertions{};
        bool debug = true;
        for (std::size_t i = 0; i < pages.size(); ++i) {
            auto packet = prepare_page_glyphs(pages[i], font);
            const auto textures = packet.assets.size();
            const auto draws = packet.draws.size();
            Experiment direct(warp, caseelephant::constants::default_view_width,
                              caseelephant::constants::default_view_height);
            Experiment rtt(warp, caseelephant::constants::default_view_width,
                           caseelephant::constants::default_view_height);
            auto p = direct.execute(false, packet);
            auto q = rtt.execute(true, std::move(packet));
            int difference{};
            for (std::size_t pixel = 0; pixel < p.size(); ++pixel) {
                for (std::size_t c = 0; c < 4; ++c) {
                    difference = std::max(difference, std::abs(int(p[pixel][c]) - int(q[pixel][c])));
                }
            }
            direct.test(difference <= caseelephant::probe_constants::rgba8_parity_tolerance,
                        "React full-image direct/RTT parity");
            direct.test(p[8 * caseelephant::constants::default_view_width + 8] == Pixel{16, 21, 30, 255},
                        "React inline background native pixels");
            const Pixel first_color = i == 2 ? Pixel{146, 200, 121, 255} : Pixel{244, 162, 97, 255};
            direct.test(p[166 * caseelephant::constants::default_view_width + 330] == first_color,
                        "React keyed bars reach GPU paint order");
            std::size_t text_pixels{};
            for (UINT y = 48; y < 84; ++y) {
                for (UINT x = 24; x < 220; ++x) {
                    if (p[y * caseelephant::constants::default_view_width + x][0] > 120) {
                        ++text_pixels;
                    }
                }
            }
            direct.test(text_pixels > 400, "actual React title glyph pixels");
            if (i == 0) {
                initial_pixels = p;
            }
            if (i == 1) {
                std::size_t changed{};
                bool outside_unchanged = true;
                for (UINT y = 0; y < caseelephant::constants::default_view_height; ++y) {
                    for (UINT x = 0; x < caseelephant::constants::default_view_width; ++x) {
                        if (p[y * caseelephant::constants::default_view_width + x] !=
                            initial_pixels[y * caseelephant::constants::default_view_width + x]) {
                            if (x >= 40 && x < 208 && y >= 158 && y < 198) {
                                ++changed;
                            } else {
                                outside_unchanged = false;
                            }
                        }
                    }
                }
                direct.test(changed > 30 && outside_unchanged, "useState changes only counter text region");
                count_pixels = p;
            }
            if (i == 2) {
                std::size_t changed{};
                for (UINT y = 158; y < 258; ++y) {
                    for (UINT x = 256; x < 600; ++x) {
                        if (p[y * caseelephant::constants::default_view_width + x] !=
                            count_pixels[y * caseelephant::constants::default_view_width + x]) {
                            ++changed;
                        }
                    }
                }
                direct.test(changed > 500, "keyed reorder changes actual list pixels");
            }
            direct.check_debug_messages();
            rtt.check_debug_messages();
            debug = debug && direct.debug_layer && rtt.debug_layer;
            retired += direct.native_releases + rtt.native_releases;
            assertions += direct.assertions + rtt.assertions;
            save_png(folder / ("frame-" + std::to_string(i) + ".png"), p, caseelephant::constants::default_view_width,
                     caseelephant::constants::default_view_height);
            save_png(folder / ("frame-" + std::to_string(i) + "-rtt.png"), q,
                     caseelephant::constants::default_view_width, caseelephant::constants::default_view_height);
            if (i) {
                report << ',';
            }
            report << "{\"state\":" << states[i] << ",\"layout\":" << page_json(pages[i])
                   << ",\"textures\":" << textures << ",\"draws\":" << draws
                   << ",\"direct_rtt_max_difference\":" << difference << '}';
        }
        report
            << "],\"assertions\":" << assertions << ",\"native_textures_retired\":" << retired
            << ",\"debug_layer\":" << (debug ? "true" : "false")
            << ",\"debug_warning_error_count\":" << (debug ? "0" : "null")
            << ",\"actual_completion_query\":true,\"keyed_identity_preserved\":true,\"native_event_dispatch_verified\":true,\"snapshots_survive_vm_reset\":true,\"root_unmount_verified\":true"
            << ",\"html_parser_verified\":false,\"stylesheet_cascade_verified\":false,\"native_window_input_verified\":false,\"mobile_verified\":false,\"passed\":true}";
        write_text(folder / "report.json", report.str());
        std::cout << "PASS React/Vite native DOM -> inline CSS -> Yoga -> HarfBuzz/FreeType -> D3D11 "
                  << (warp ? "WARP" : "hardware") << "; 3 real pixel frames; " << retired << " textures retired; "
                  << assertions << " assertions\n";
        return 0;
    }

#include "react_window_host.hpp"
#include "render_backend_probe.hpp"
#include "cheetah_probe.hpp"
#include "cheetah_alert_probe.hpp"
#include "flow_probe.hpp"
#include "two_frame_pipeline.hpp"
#include "cheetah_flow_window_probe.hpp"
#include "render_cache_probe.hpp"
#include "two_frame_probe.hpp"
#include "gallery_canvas_probe.hpp"
#include "gallery_scene_session.hpp"
#include "scene_native_gpu_probe.hpp"
#include "gallery_layer_manifest.hpp"
#include "scene_gpu_surface_probe.hpp"
#include "scene_surface_retirement_probe.hpp"
#include "gallery_cluster_gpu_probe.hpp"
#include "gallery_interaction_profile.hpp"
#include "gallery_owner_probe.hpp"
#include "gallery_preview_probe.hpp"
#include "external_texture_probe.hpp"
#include "complex_css_probe.hpp"
#include "render_stages_probe.hpp"
} // namespace

int main(int argc, char** argv) {
    try {
        const bool owner_async = argc == 3 && (std::string(argv[1]) == "--owner-async-test-hardware" ||
                                               std::string(argv[1]) == "--owner-async-test-warp");
        const bool render_stages = argc == 3 && (std::string(argv[1]) == "--render-stages-test-hardware" ||
                                                 std::string(argv[1]) == "--render-stages-test-warp");
        const bool complex_css_test = argc == 3 && (std::string(argv[1]) == "--complex-css-test-hardware" ||
                                                    std::string(argv[1]) == "--complex-css-test-warp");
        const bool scene_native_test = argc == 3 && (std::string(argv[1]) == "--scene-native-test-hardware" ||
                                                     std::string(argv[1]) == "--scene-native-test-warp");
        const bool scene_gpu_test = argc == 3 && (std::string(argv[1]) == "--scene-gpu-test-hardware" ||
                                                  std::string(argv[1]) == "--scene-gpu-test-warp");
        const bool gallery_profile = argc == 4 && std::string(argv[1]) == "--gallery-owner-profile";
        const bool gallery_input_benchmark = argc == 4 && std::string(argv[1]) == "--gallery-input-benchmark-hardware";
        const bool gallery_benchmark = argc == 4 && std::string(argv[1]) == "--gallery-benchmark-hardware";
        const bool surface_retirement = argc == 3 && (std::string(argv[1]) == "--scene-surface-retirement-hardware" ||
                                                      std::string(argv[1]) == "--scene-surface-retirement-warp");
        const bool gallery_scroll = argc == 3 && (std::string(argv[1]) == "--gallery-scroll-test-hardware" ||
                                                  std::string(argv[1]) == "--gallery-scroll-test-warp");
        const bool gallery_cluster = argc == 3 && (std::string(argv[1]) == "--gallery-cluster-gpu-hardware" ||
                                                   std::string(argv[1]) == "--gallery-cluster-gpu-warp");
        const bool gallery_trace = argc == 3 && (std::string(argv[1]) == "--gallery-window-profile-hardware" ||
                                                 std::string(argv[1]) == "--gallery-window-profile-warp");
        const bool gallery_preview = argc == 3 && (std::string(argv[1]) == "--gallery-window-hardware" ||
                                                   std::string(argv[1]) == "--gallery-window-warp" ||
                                                   std::string(argv[1]) == "--gallery-window-test-hardware" ||
                                                   std::string(argv[1]) == "--gallery-window-test-warp");
        const bool external_texture = argc == 3 && (std::string(argv[1]) == "--external-texture-warp" ||
                                                    std::string(argv[1]) == "--external-texture-hardware");
        const bool gallery_canvas_window =
            argc == 3 && (std::string(argv[1]) == "--gallery-canvas-window-hardware" ||
                          std::string(argv[1]) == "--gallery-canvas-window-warp" ||
                          std::string(argv[1]) == "--gallery-canvas-window-test-warp" ||
                          std::string(argv[1]) == "--gallery-canvas-window-test-hardware");
        const bool gallery_canvas = argc == 3 && (std::string(argv[1]) == "--gallery-canvas-warp" ||
                                                  std::string(argv[1]) == "--gallery-canvas-hardware");
        const bool pipeline_probe =
            argc == 3 && (std::string(argv[1]) == "--pipeline-warp" || std::string(argv[1]) == "--pipeline-hardware");
        const bool cache_probe =
            argc == 3 && (std::string(argv[1]) == "--cache-warp" || std::string(argv[1]) == "--cache-hardware");
        const bool browser = argc == 4 && std::string(argv[1]) == "--browser-png";
        const bool sdf =
            argc == 3 && (std::string(argv[1]) == "--sdf-warp" || std::string(argv[1]) == "--sdf-hardware");
        const bool react =
            argc == 3 && (std::string(argv[1]) == "--react-warp" || std::string(argv[1]) == "--react-hardware");
        const bool backend =
            argc == 3 && (std::string(argv[1]) == "--backend-warp" || std::string(argv[1]) == "--backend-hardware");
        const bool cheetah =
            argc == 3 && (std::string(argv[1]) == "--cheetah-warp" || std::string(argv[1]) == "--cheetah-hardware" ||
                          std::string(argv[1]) == "--cheetah-window-hardware");
        const bool automatic = argc == 3 && (std::string(argv[1]) == "--cheetah-auto-warp" ||
                                             std::string(argv[1]) == "--cheetah-auto-hardware");
        const bool cheetah_flow = argc == 3 && (std::string(argv[1]) == "--cheetah-flow-warp" ||
                                                std::string(argv[1]) == "--cheetah-flow-hardware");
        const bool flow_window =
            argc == 3 && (std::string(argv[1]) == "--cheetah-flow-window-test-4k-motion-warp" ||
                          std::string(argv[1]) == "--cheetah-flow-window-test-4k-motion-hardware" ||
                          std::string(argv[1]) == "--cheetah-flow-window-test-warp" ||
                          std::string(argv[1]) == "--cheetah-flow-window-test-hardware" ||
                          std::string(argv[1]) == "--cheetah-flow-window-warp" ||
                          std::string(argv[1]) == "--cheetah-flow-window-hardware" ||
                          std::string(argv[1]) == "--cheetah-flow-window-test-4k-warp" ||
                          std::string(argv[1]) == "--cheetah-flow-window-test-4k-hardware" ||
                          std::string(argv[1]) == "--cheetah-flow-window-perf-hardware" ||
                          std::string(argv[1]) == "--cheetah-flow-window-perf-sync-hardware");
        const bool flow =
            argc == 3 && (std::string(argv[1]) == "--flow-warp" || std::string(argv[1]) == "--flow-hardware");
        const bool theme = argc == 3 && (std::string(argv[1]) == "--cheetah-style-warp" ||
                                         std::string(argv[1]) == "--cheetah-style-hardware");
        const bool alert = argc == 3 && (std::string(argv[1]) == "--cheetah-alert-warp" ||
                                         std::string(argv[1]) == "--cheetah-alert-hardware");
        const bool window = argc == 3 && (std::string(argv[1]) == "--react-window-hardware" ||
                                          std::string(argv[1]) == "--react-window-warp" ||
                                          std::string(argv[1]) == "--react-window-test-hardware" ||
                                          std::string(argv[1]) == "--react-window-test-warp");
        require(
            gallery_input_benchmark || gallery_trace || gallery_profile || surface_retirement || gallery_scroll ||
                gallery_cluster || owner_async || scene_native_test || render_stages || complex_css_test ||
                scene_gpu_test || gallery_benchmark || gallery_preview || external_texture || gallery_canvas_window ||
                gallery_canvas || pipeline_probe || cache_probe || flow_window || cheetah_flow || flow || automatic ||
                browser || sdf || react || window || backend || cheetah || alert || theme ||
                (argc == 3 && (std::string(argv[1]) == "--warp" || std::string(argv[1]) == "--hardware")),
            "Usage: ce_d3d11_probe --gallery-window-[test-]hardware|warp <output-directory> (or an existing probe mode)");
        check_hresult(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");
        try {
            const auto result =
                gallery_input_benchmark ? run_gallery_preview(false, argv[2], false, std::string("input-") + argv[3])
                : gallery_trace
                    ? run_gallery_preview(std::string(argv[1]).ends_with("warp"), argv[2], false, {}, true, false, true)
                : gallery_profile    ? run_gallery_interaction_profile(argv[2], argv[3])
                : surface_retirement ? run_scene_surface_retirement(std::string(argv[1]).ends_with("warp"), argv[2])
                : gallery_scroll
                    ? run_gallery_preview(std::string(argv[1]).ends_with("warp"), argv[2], true, {}, true, true)
                : gallery_cluster   ? run_gallery_cluster_gpu(std::string(argv[1]).ends_with("warp"), argv[2])
                : owner_async       ? run_owner_async(std::string(argv[1]).ends_with("warp"), argv[2])
                : scene_native_test ? run_scene_gpu_test(std::string(argv[1]).ends_with("warp"), argv[2], true)
                : render_stages     ? run_render_stages(std::string(argv[1]).ends_with("warp"), argv[2])
                : complex_css_test  ? run_complex_css_test(std::string(argv[1]).ends_with("warp"), argv[2])
                : scene_gpu_test    ? run_scene_gpu_test(std::string(argv[1]).ends_with("warp"), argv[2])
                : gallery_benchmark ? run_gallery_preview(false, argv[2], false, argv[3])
                : gallery_preview   ? run_gallery_preview(std::string(argv[1]).ends_with("warp"), argv[2],
                                                          std::string(argv[1]).find("-test-") != std::string::npos)
                : external_texture  ? run_external_texture(std::string(argv[1]).ends_with("warp"), argv[2])
                : gallery_canvas_window
                    ? run_gallery_canvas_window(std::string(argv[1]).ends_with("warp"), argv[2],
                                                std::string(argv[1]).find("-test-") != std::string::npos)
                : gallery_canvas ? run_gallery_canvas(std::string(argv[1]).ends_with("warp"), argv[2])
                : pipeline_probe ? run_two_frame_probe(std::string(argv[1]).ends_with("warp"), argv[2])
                : cache_probe    ? run_render_cache(std::string(argv[1]).ends_with("warp"), argv[2])
                : flow_window    ? run_cheetah_flow_window(std::string(argv[1]).ends_with("warp"),
                                                           std::string(argv[1]).find("-test-") != std::string::npos ||
                                                               std::string(argv[1]).find("-perf-") != std::string::npos,
                                                           argv[2],
                                                           std::string(argv[1]).find("-test-") == std::string::npos ||
                                                               std::string(argv[1]).find("-4k-") != std::string::npos,
                                                           std::string(argv[1]).find("-perf-") != std::string::npos,
                                                           std::string(argv[1]).find("-sync-") != std::string::npos,
                                                           std::string(argv[1]).find("-motion-") != std::string::npos)
                : cheetah_flow
                    ? run_cheetah_alert(std::string(argv[1]) == "--cheetah-flow-warp", argv[2], true, true, true)
                : flow      ? run_flow(std::string(argv[1]) == "--flow-warp", argv[2])
                : automatic ? run_cheetah_alert(std::string(argv[1]) == "--cheetah-auto-warp", argv[2], true, true)
                : theme     ? run_cheetah_alert(std::string(argv[1]) == "--cheetah-style-warp", argv[2], true)
                : alert     ? run_cheetah_alert(std::string(argv[1]) == "--cheetah-alert-warp", argv[2])
                : cheetah   ? run_cheetah(std::string(argv[1]) == "--cheetah-warp",
                                          std::string(argv[1]) == "--cheetah-window-hardware", argv[2])
                : backend   ? run_backend(std::string(argv[1]) == "--backend-warp", argv[2])
                : window    ? run_react_window(std::string(argv[1]).ends_with("warp"),
                                               std::string(argv[1]).find("-test-") != std::string::npos, argv[2])
                : browser   ? capture_browser(argv[2], argv[3])
                : react     ? run_react(std::string(argv[1]) == "--react-warp", argv[2])
                : sdf       ? run_sdf(std::string(argv[1]) == "--sdf-warp", argv[2])
                            : run(std::string(argv[1]) == "--warp", argv[2]);
            CoUninitialize();
            return result;
        } catch (...) {
            CoUninitialize();
            throw;
        }
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
