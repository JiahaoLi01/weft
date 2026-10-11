#include "render/backends/d3d11/renderer.hpp"

#include "constants/common.hpp"
#include "render/backends/d3d11/backend.hpp"
#include "render/srgb.hpp"

#include <d3d11sdklayers.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <tuple>

namespace weft::render::d3d11 {
    namespace {
        // Compiled at build time from quad.hlsl (cmake/WeftShaders.cmake) with the flags the renderer used for
        // runtime compilation: /O3 with NDEBUG, /Zi /Od without.
        constexpr unsigned char quad_vs[] = {
#ifdef NDEBUG
#include "shaders/quad.vs.inc"
#else
#include "shaders/quad.vs.debug.inc"
#endif
        };
        constexpr unsigned char quad_ps[] = {
#ifdef NDEBUG
#include "shaders/quad.ps.inc"
#else
#include "shaders/quad.ps.debug.inc"
#endif
        };
        constexpr unsigned char convert_vs_bytes[] = {
#ifdef NDEBUG
#include "shaders/srgb_convert.vs.inc"
#else
#include "shaders/srgb_convert.vs.debug.inc"
#endif
        };
        constexpr unsigned char convert_ps_bytes[] = {
#ifdef NDEBUG
#include "shaders/srgb_convert.ps.inc"
#else
#include "shaders/srgb_convert.ps.debug.inc"
#endif
        };
        // The shared scene shader as a linear pass, always optimized (cmake/WeftShaders.cmake).
        constexpr unsigned char display_vs_bytes[] = {
#include "shaders/display_linear.vs.inc"
        };
        constexpr unsigned char display_ps_bytes[] = {
#include "shaders/display_linear.ps.inc"
        };
        // Entries of each srgb_convert.hlsl table: one per byte value.
        constexpr unsigned convert_table_entries = 256;

        // An immutable structured buffer of values and its view, alive while recorded commands use them.
        template <class T>
        std::pair<ComPtr<ID3D11Buffer>, ComPtr<ID3D11ShaderResourceView>>
        structured(ID3D11Device* device, const std::vector<T>& values, const char* what) {
            D3D11_BUFFER_DESC d{};
            d.ByteWidth = UINT(values.size() * sizeof(T));
            d.Usage = D3D11_USAGE_IMMUTABLE;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            d.StructureByteStride = sizeof(T);
            const D3D11_SUBRESOURCE_DATA data{values.data(), 0, 0};
            std::pair<ComPtr<ID3D11Buffer>, ComPtr<ID3D11ShaderResourceView>> out;
            throw_if_failed(device->CreateBuffer(&d, &data, &out.first), what);
            D3D11_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = DXGI_FORMAT_UNKNOWN;
            v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            v.Buffer.NumElements = UINT(values.size());
            throw_if_failed(device->CreateShaderResourceView(out.first.Get(), &v, &out.second), what);
            return out;
        }

        namespace materials = weft::constants::render::d3d11::shader_material;

        void lifetime(LifetimeResult r, const char* operation) {
            ensure(r == LifetimeResult::Ok, operation);
        }

        std::string utf8(const wchar_t* text) {
            const auto size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
            ensure(size > 0, "adapter name conversion");
            std::string result(static_cast<std::size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
            result.pop_back();
            return result;
        }

        using Color = std::array<float, 4>;
    } // namespace

    void ensure(bool condition, std::string_view message) {
        if (!condition) {
            throw std::runtime_error(std::string(message));
        }
    }

    void throw_if_failed(HRESULT result, const char* operation) {
        if (FAILED(result)) {
            std::ostringstream message;
            message << operation << " HRESULT=0x" << std::hex << static_cast<unsigned long>(result);
            throw std::runtime_error(message.str());
        }
    }

    Renderer::Renderer(bool warp, UINT w, UINT h) : width(w), height(h) {
        ensure(w && h && w <= weft::constants::render::d3d11::max_surface_dimension &&
                   h <= weft::constants::render::d3d11::max_surface_dimension,
               "bounded probe surface");
        ComPtr<IDXGIFactory1> factory;
        throw_if_failed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
        ComPtr<IDXGIAdapter1> selected;
        if (!warp) {
            SIZE_T largest{};
            for (UINT i = 0;; ++i) {
                ComPtr<IDXGIAdapter1> candidate;
                const auto hr = factory->EnumAdapters1(i, &candidate);
                if (hr == DXGI_ERROR_NOT_FOUND) {
                    break;
                }
                throw_if_failed(hr, "EnumAdapters1");
                DXGI_ADAPTER_DESC1 desc{};
                throw_if_failed(candidate->GetDesc1(&desc), "GetDesc1");
                if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && (!selected || desc.DedicatedVideoMemory > largest)) {
                    selected = candidate;
                    largest = desc.DedicatedVideoMemory;
                }
            }
            ensure(selected != nullptr, "No hardware adapter; use explicit --warp instead");
        }
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
        const auto create = [&](UINT flags) {
            return D3D11CreateDevice(selected.Get(), warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                     flags | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1, D3D11_SDK_VERSION, &device,
                                     &level, &immediate);
        };
        auto hr = create(D3D11_CREATE_DEVICE_DEBUG);
        if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING) {
            hr = create(0);
        } else if (SUCCEEDED(hr)) {
            debug_layer = true;
        }
        throw_if_failed(hr, "D3D11CreateDevice");
        ComPtr<IDXGIDevice> dxgi;
        throw_if_failed(device.As(&dxgi), "IDXGIDevice");
        ComPtr<IDXGIAdapter> actual;
        throw_if_failed(dxgi->GetAdapter(&actual), "GetAdapter");
        DXGI_ADAPTER_DESC desc{};
        throw_if_failed(actual->GetDesc(&desc), "GetDesc");
        adapter = utf8(desc.Description);
        if (debug_layer) {
            throw_if_failed(device.As(&info), "ID3D11InfoQueue");
        }
        throw_if_failed(device->CreateDeferredContext(0, &deferred), "CreateDeferredContext");
        throw_if_failed(device->CreateVertexShader(quad_vs, sizeof(quad_vs), nullptr, &vs), "CreateVertexShader");
        throw_if_failed(device->CreatePixelShader(quad_ps, sizeof(quad_ps), nullptr, &ps), "CreatePixelShader");
        throw_if_failed(device->CreateVertexShader(display_vs_bytes, sizeof(display_vs_bytes), nullptr, &display_vs),
                        "display list vertex shader");
        throw_if_failed(device->CreatePixelShader(display_ps_bytes, sizeof(display_ps_bytes), nullptr, &display_ps),
                        "display list pixel shader");
        {
            D3D11_BUFFER_DESC d{};
            d.ByteWidth = weft::constants::render::scene::view_constants_bytes;
            d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            throw_if_failed(device->CreateBuffer(&d, nullptr, &display_constants), "display list view constants");
        }
        throw_if_failed(device->CreateVertexShader(convert_vs_bytes, sizeof(convert_vs_bytes), nullptr, &convert_vs),
                        "Create conversion vertex shader");
        throw_if_failed(device->CreatePixelShader(convert_ps_bytes, sizeof(convert_ps_bytes), nullptr, &convert_ps),
                        "Create conversion pixel shader");
        {
            // The CPU reference values: sRGB decode, then byte / 255, both in single precision.
            std::array<float, 2 * convert_table_entries> tables{};
            for (unsigned i = 0; i < convert_table_entries; ++i) {
                tables[i] = srgb_to_linear(i / weft::constants::render::color_channel_max_float);
                tables[convert_table_entries + i] = i / weft::constants::render::color_channel_max_float;
            }
            D3D11_BUFFER_DESC td{};
            td.ByteWidth = UINT(sizeof(tables));
            td.Usage = D3D11_USAGE_IMMUTABLE;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            const D3D11_SUBRESOURCE_DATA initial{tables.data(), 0, 0};
            ComPtr<ID3D11Buffer> buffer;
            throw_if_failed(device->CreateBuffer(&td, &initial, &buffer), "Create conversion tables");
            D3D11_SHADER_RESOURCE_VIEW_DESC tv{};
            tv.Format = DXGI_FORMAT_R32_FLOAT;
            tv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            tv.Buffer.NumElements = UINT(tables.size());
            throw_if_failed(device->CreateShaderResourceView(buffer.Get(), &tv, &convert_tables),
                            "Create conversion table view");
        }
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(QuadConstants);
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        throw_if_failed(device->CreateBuffer(&cb, nullptr, &constants), "CreateBuffer");
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
        throw_if_failed(device->CreateBlendState(&b, &blend), "CreateBlendState");
        D3D11_RASTERIZER_DESC r{};
        r.FillMode = D3D11_FILL_SOLID;
        r.CullMode = D3D11_CULL_NONE;
        r.DepthClipEnable = TRUE;
        r.ScissorEnable = TRUE;
        throw_if_failed(device->CreateRasterizerState(&r, &raster), "CreateRasterizerState");
        D3D11_DEPTH_STENCIL_DESC d{};
        throw_if_failed(device->CreateDepthStencilState(&d, &depth), "CreateDepthStencilState");
        D3D11_SAMPLER_DESC s{};
        s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        s.MaxLOD = D3D11_FLOAT32_MAX;
        throw_if_failed(device->CreateSamplerState(&s, &sampler), "CreateSamplerState");
        output = surface(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        intermediate = surface(DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT);
        group = surface(DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT);
        D3D11_TEXTURE2D_DESC sd{};
        output.texture->GetDesc(&sd);
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        throw_if_failed(device->CreateTexture2D(&sd, nullptr, &staging), "Create staging");
        sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        throw_if_failed(device->CreateTexture2D(&sd, nullptr, &alpha_staging), "Create alpha staging");
        const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
        throw_if_failed(device->CreateQuery(&query, &ui_done), "UI completion query");
        throw_if_failed(device->CreateQuery(&query, &host_done), "Host completion query");
    }

    Renderer::~Renderer() {
        if (submitted && !acknowledged) {
            try {
                complete();
            } catch (...) {
                std::terminate();
            }
        }
        delete backend;
    }

    Surface Renderer::surface(DXGI_FORMAT storage, DXGI_FORMAT view) {
        Surface result;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = width;
        d.Height = height;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.SampleDesc.Count = 1;
        d.Format = storage;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        throw_if_failed(device->CreateTexture2D(&d, nullptr, &result.texture), "Create surface");
        D3D11_RENDER_TARGET_VIEW_DESC r{};
        r.Format = view;
        r.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        throw_if_failed(device->CreateRenderTargetView(result.texture.Get(), &r, &result.rtv), "Create RTV");
        D3D11_SHADER_RESOURCE_VIEW_DESC s{};
        s.Format = view;
        s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        s.Texture2D.MipLevels = 1;
        throw_if_failed(device->CreateShaderResourceView(result.texture.Get(), &s, &result.srv), "Create SRV");
        return result;
    }

    std::uint64_t Renderer::asset_identity(const std::shared_ptr<const Asset>& source) {
        if (const auto i = asset_identities.find(source.get()); i != asset_identities.end()) {
            if (auto previous = i->second.first.lock(); previous && previous == source) {
                return i->second.second;
            }
            asset_identities.erase(i); // Address reuse cannot alias an old version.
        }
        // Weak metadata retains no CPU bitmap and keeps all live identities stable.
        if (asset_identities.size() >= weft::constants::render::d3d11::asset_identity_prune_threshold) {
            std::erase_if(asset_identities, [](const auto& entry) {
                return entry.second.first.expired();
            });
        }
        ensure(asset_identities.size() < weft::constants::render::d3d11::max_asset_identities,
               "live asset identity metadata bound");
        ensure(next_asset_identity != UINT64_MAX, "asset identity generation bound");
        const auto id = next_asset_identity++;
        asset_identities.emplace(source.get(), std::pair{std::weak_ptr<const Asset>(source), id});
        return id;
    }

    void Renderer::evict_assets(std::size_t required, bool all) {
        ensure(!submitted || acknowledged, "GPU cache retirement requires actual completion");
        ensure(!all || !async_in_flight, "GPU cache drain requires all asynchronous host completions");
        ensure(required <= cache_byte_limit, "GPU cache resource byte bound");
        while (!asset_cache.empty() &&
               (all || asset_cache.size() >= cache_resource_limit || required > cache_byte_limit - cached_bytes)) {
            auto victim = asset_cache.end();
            for (auto i = asset_cache.begin(); i != asset_cache.end(); ++i) {
                ResourceInfo resource_state;
                lifetime(tracker.InspectResource(i->second.native.id, resource_state), "inspect cache eviction");
                if (resource_state.cpu_leases == 1 && !resource_state.prepared_pins && !resource_state.encoded_pins &&
                    !resource_state.submitted_pins &&
                    (victim == asset_cache.end() || i->second.touched < victim->second.touched)) {
                    victim = i;
                }
            }
            ensure(victim != asset_cache.end(), "bounded cache cannot evict resources in current full packet");
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

    PreparedFrame Renderer::prepare(FramePacket packet,
                                    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& keys) {
        PreparedFrame frame;
        frame.snapshot = std::move(packet);
        frame.dependencies.reserve(frame.snapshot.assets.size());
        assets.reserve(frame.snapshot.assets.size());

        struct ExistingLeases {
            Renderer& renderer;
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
                    lifetime(tracker.RetainResource(found->second.native.id), "pin all incoming cached dependencies");
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
                        n.source = cached.native.source;
                        n.source_srv = cached.native.source_srv;
                        n.rtv = cached.native.rtv;
                    }
                    ensure(existing.values[i] && *existing.values[i] == n.id, "complete cached dependency lease");
                    cached.touched = ++cache_clock;
                    ++cache_hits;
                    frame.dependencies.push_back(n.id);
                    assets.push_back(std::move(n));
                    existing.values[i].reset();
                    continue;
                }
            }
            ensure(a.width && a.height &&
                       a.bytes.size() == static_cast<std::size_t>(a.width) * a.height *
                                             (r8 ? 1 : weft::constants::rgba8_bytes_per_pixel),
                   "asset dimensions/encoding");
            if (a.encoding == Encoding::SignedDistance) {
                ensure(a.spread >= weft::constants::render::min_sdf_spread &&
                           a.spread <= weft::constants::render::max_sdf_spread,
                       "distance spread metadata");
            }
            // sRGB textures decode before filtering; exact for opaque inputs.
            // Translucent inputs keep linear premultiplied floats to avoid halos.
            bool opaque = !r8;
            for (std::size_t p = 3; opaque && p < a.bytes.size(); p += 4) {
                opaque = a.bytes[p] == weft::constants::color_channel_max;
            }
            // Translucent texels are converted on the GPU from the uploaded bytes (srgb_convert.hlsl); the
            // resource is accounted as its float texture.
            const bool convert = !r8 && !opaque;
            n.upload = a.bytes;
            n.pitch = r8 ? a.width : a.width * weft::constants::rgba8_bytes_per_pixel;
            const auto resource_bytes =
                convert ? static_cast<std::size_t>(a.width) * a.height * sizeof(Color) : n.upload.size();
            // Capacity failure must happen before allocating a tracked resource.
            // All incoming hits are already pinned; a full packet that exceeds
            // the cache cannot leave an unowned CPU lease behind on rejection.
            if (cache_assets) {
                evict_assets(resource_bytes);
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
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (convert ? D3D11_BIND_RENDER_TARGET : 0u);
            throw_if_failed(device->CreateTexture2D(&d, nullptr, &n.texture), "Create input texture");
            throw_if_failed(device->CreateShaderResourceView(n.texture.Get(), nullptr, &n.srv), "Create input SRV");
            if (convert) {
                throw_if_failed(device->CreateRenderTargetView(n.texture.Get(), nullptr, &n.rtv),
                                "Create conversion target");
                auto source = d;
                source.Format = DXGI_FORMAT_R8G8B8A8_UINT;
                source.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                throw_if_failed(device->CreateTexture2D(&source, nullptr, &n.source), "Create conversion source");
                throw_if_failed(device->CreateShaderResourceView(n.source.Get(), nullptr, &n.source_srv),
                                "Create conversion source SRV");
            }
            lifetime(tracker.CreateResource({key.first, key.second, resource_bytes, ResourceKind::Texture}, n.id),
                     "CreateResource");
            ++native_creations;
            if (cache_assets) {
                const auto bytes = resource_bytes;
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
                n.source = cached.native.source;
                n.source_srv = cached.native.source_srv;
                n.rtv = cached.native.rtv;
                n.cache_copy = true;
                lifetime(tracker.RetainResource(n.id), "new cached frame lease");
            }
            frame.dependencies.push_back(n.id);
            assets.push_back(std::move(n));
        }
        lifetime(tracker.PrepareFrame(frame.dependencies, frame.id), "PrepareFrame");
        return frame;
    }

    PreparedFrame Renderer::prepare(paint::DisplayList list,
                                    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& keys) {
        for (const auto& item : list.items) {
            ensure(item.kind == paint::Item::Kind::Box || item.kind == paint::Item::Kind::Image,
                   "display list pass draws boxes and images");
            if (item.kind == paint::Item::Kind::Image) {
                const auto& image = list.images.at(item.payload);
                const auto& asset = *list.assets.at(image.asset);
                ensure(image.stroke >= 0 &&
                           (asset.encoding != Encoding::SignedDistance || image.stroke < asset.spread - 1),
                       "SDF stroke exceeds distance padding");
            }
        }
        ensure(bool(paint::check(list, 0)), "well-formed display list");
        FramePacket packet;
        packet.assets = list.assets;
        auto frame = prepare(std::move(packet), keys);
        frame.list = std::move(list);
        return frame;
    }

    void Renderer::collect() {
        for (auto& a : assets) {
            ResourceInfo state;
            if (a.texture && tracker.InspectResource(a.id, state) == LifetimeResult::StaleHandle) {
                a.srv.Reset();
                a.texture.Reset();
                a.source_srv.Reset();
                a.source.Reset();
                a.rtv.Reset();
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
                cached.native.source_srv.Reset();
                cached.native.source.Reset();
                cached.native.rtv.Reset();
            }
        }
    }

    void Renderer::bind_target(Surface& target) {
        ID3D11ShaderResourceView* empty{};
        deferred->PSSetShaderResources(0, 1, &empty);
        auto* rtv = target.rtv.Get();
        deferred->OMSetRenderTargets(1, &rtv, nullptr);
    }

    void Renderer::draw(const Draw& d, ID3D11ShaderResourceView* texture, UINT material, float spread) {
        Color color = d.srgb;
        for (int c = 0; c < 3; ++c) {
            color[c] = srgb_to_linear(color[c]) * color[3];
        }
        Color outline = d.outline;
        for (int c = 0; c < 3; ++c) {
            outline[c] = srgb_to_linear(outline[c]) * outline[3];
        }
        ensure(d.stroke >= 0 && (material != materials::signed_distance || d.stroke < spread - 1),
               "SDF stroke exceeds distance padding");
        QuadConstants data{{d.rect.x, d.rect.y, d.rect.w, d.rect.h},
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
            clip = {static_cast<LONG>(d.rect.x + weft::constants::render::d3d11::diagnostic_clip_inset),
                    static_cast<LONG>(d.rect.y + weft::constants::render::d3d11::diagnostic_clip_inset),
                    static_cast<LONG>(d.rect.x + d.rect.w - weft::constants::render::d3d11::diagnostic_clip_inset),
                    static_cast<LONG>(d.rect.y + d.rect.h - weft::constants::render::d3d11::diagnostic_clip_inset)};
        }
        deferred->RSSetScissorRects(1, &clip);
        deferred->Draw(weft::constants::render::d3d11::quad_vertex_count, 0);
    }

    Encoded Renderer::encode(const PreparedFrame& frame, bool rtt, bool fail_after_upload,
                             ID3D11DeviceContext* native_context, ID3D11RenderTargetView* native_target,
                             bool clear_native_target, const float* native_clear) {
        struct RestoreContext {
            Renderer& renderer;
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
            std::vector<const NativeAsset*> conversions;
            for (auto& a : assets) {
                ResourceInfo resource_state;
                lifetime(tracker.InspectResource(a.id, resource_state), "inspect upload readiness");
                // A submitted upload is usable by later work on this same
                // queue, while an unsubmitted/cancelled upload is not.
                if (resource_state.phase != ResourcePhase::Ready && resource_state.phase != ResourcePhase::Submitted) {
                    ensure(!a.upload.empty() && (!a.rtv || a.source), "created resource retains retry upload");
                    deferred->UpdateSubresource(a.source ? a.source.Get() : a.texture.Get(), 0, nullptr,
                                                a.upload.data(), a.pitch, 0);
                    ++uploads;
                    if (a.source) {
                        conversions.push_back(&a);
                    }
                }
            }
            if (!conversions.empty()) {
                deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                deferred->VSSetShader(convert_vs.Get(), nullptr, 0);
                deferred->PSSetShader(convert_ps.Get(), nullptr, 0);
                auto* tables = convert_tables.Get();
                deferred->PSSetShaderResources(1, 1, &tables);
                for (const auto* a : conversions) {
                    D3D11_TEXTURE2D_DESC d{};
                    a->texture->GetDesc(&d);
                    auto* rtv = a->rtv.Get();
                    deferred->OMSetRenderTargets(1, &rtv, nullptr);
                    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(d.Width), static_cast<float>(d.Height),
                                                  0, 1};
                    deferred->RSSetViewports(1, &viewport);
                    auto* source = a->source_srv.Get();
                    deferred->PSSetShaderResources(0, 1, &source);
                    deferred->Draw(3, 0);
                    ++gpu_conversions;
                }
                // Unbind the converted targets before the frame samples them.
                deferred->ClearState();
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
            if (frame.list) {
                encode_list(*frame.list, native_target ? native_target : destination.rtv.Get());
            }
            for (const auto& d : frame.snapshot.draws) {
                if (d.group) {
                    bind_target(group);
                    deferred->ClearRenderTargetView(group.rtv.Get(), transparent);
                    Draw red = d;
                    red.group = false;
                    red.opacity = 1;
                    red.srgb = {1, 0, 0, 1};
                    draw(red, nullptr, materials::solid);
                    Draw blue = red;
                    blue.srgb = {0, 0, 1, 1};
                    draw(blue, nullptr, materials::solid);
                    bind_target(destination);
                    Draw composite = d;
                    composite.uv = {d.rect.x / width, d.rect.y / height, d.rect.w / width, d.rect.h / height};
                    draw(composite, group.srv.Get(), materials::texture);
                } else {
                    if (d.asset >= 0 && size_t(d.asset) >= frame.snapshot.assets.size()) {
                        const auto& external = frame.external.at(size_t(d.asset) - frame.snapshot.assets.size());
                        draw(d, external.srv.Get(),
                             external.bottom_left ? materials::texture_bottom_up : materials::texture);
                        continue;
                    }
                    auto* texture = d.asset < 0 ? nullptr : assets[static_cast<std::size_t>(d.asset)].srv.Get();
                    const auto* asset =
                        d.asset < 0 ? nullptr : frame.snapshot.assets[static_cast<std::size_t>(d.asset)].get();
                    const UINT shader_material = !asset                                  ? materials::solid
                                                 : asset->encoding == Encoding::Coverage ? materials::coverage
                                                 : asset->encoding == Encoding::SignedDistance
                                                     ? materials::signed_distance
                                                     : materials::texture;
                    draw(d, texture, shader_material, asset ? asset->spread : 0);
                }
            }
            if (rtt) {
                bind_target(output);
                deferred->ClearRenderTargetView(output.rtv.Get(), transparent);
                draw({{0, 0, static_cast<float>(width), static_cast<float>(height)}}, intermediate.srv.Get(),
                     materials::texture);
            }
            // A transparent linear destination checks premultiplied RGB/alpha
            // independently of the opaque screenshot's alpha=1 background.
            if (!native_target) {
                bind_target(group);
                deferred->ClearRenderTargetView(group.rtv.Get(), transparent);
                draw({{0, 0, static_cast<float>(width), static_cast<float>(height)}, {0, 1, 0, 0.5f}}, nullptr,
                     materials::solid);
            }
            deferred->ClearState();
            throw_if_failed(deferred->FinishCommandList(FALSE, &encoded.commands), "FinishCommandList");
            return encoded;
        } catch (...) {
            // Finish and destroy the partially recorded list BEFORE unpinning.
            deferred->ClearState();
            ComPtr<ID3D11CommandList> discarded;
            throw_if_failed(deferred->FinishCommandList(FALSE, &discarded), "Discard partial command list");
            discarded.Reset();
            lifetime(tracker.DiscardEncoded(encoded.token), "Discard failed recording");
            throw;
        }
    }

    void Renderer::encode_list(const paint::DisplayList& list, ID3D11RenderTargetView* target) {
        const auto pass = plan_scene_pass(list, 1, width, height, std::nullopt, Space::Linear).value();
        if (pass.clear != paint::Color{}) {
            deferred->ClearRenderTargetView(target, pass.clear.data());
        }
        if (pass.batches.empty()) {
            return;
        }
        const auto [records, record_view] = structured(device.Get(), pass.records, "display list records");
        ComPtr<ID3D11Buffer> clip_buffer;
        ComPtr<ID3D11ShaderResourceView> clip_view;
        if (!pass.clips.empty()) {
            std::tie(clip_buffer, clip_view) = structured(device.Get(), pass.clips, "display list clips");
        }
        D3D11_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        deferred->RSSetScissorRects(1, &scissor);
        deferred->VSSetShader(display_vs.Get(), nullptr, 0);
        deferred->PSSetShader(display_ps.Get(), nullptr, 0);
        auto* cb = display_constants.Get();
        deferred->VSSetConstantBuffers(0, 1, &cb);
        deferred->PSSetConstantBuffers(0, 1, &cb);
        ID3D11ShaderResourceView* views[3]{record_view.Get(), nullptr, clip_view.Get()};
        deferred->VSSetShaderResources(0, 1, views);
        deferred->PSSetShaderResources(0, 3, views);
        std::array<float, 4> view{static_cast<float>(width), static_cast<float>(height), 1, 0};
        for (const auto& batch : pass.batches) {
            ID3D11ShaderResourceView* image{};
            if (batch.step == ScenePass::Step::Image) {
                const auto& item = list.items[batch.first];
                image = assets.at(list.images[item.payload].asset).srv.Get();
            } else {
                ensure(batch.step == ScenePass::Step::Boxes, "display list pass draws boxes and images");
            }
            view[3] = static_cast<float>(batch.first);
            deferred->UpdateSubresource(display_constants.Get(), 0, nullptr, view.data(), 0, 0);
            deferred->PSSetShaderResources(1, 1, &image);
            deferred->DrawInstanced(weft::constants::render::d3d11::quad_vertex_count, batch.count, 0, 0);
        }
        // Back to the quad pipeline for the rest of the recording.
        ID3D11ShaderResourceView* empty[3]{};
        deferred->VSSetShaderResources(0, 1, empty);
        deferred->PSSetShaderResources(0, 3, empty);
        deferred->VSSetShader(vs.Get(), nullptr, 0);
        deferred->PSSetShader(ps.Get(), nullptr, 0);
        cb = constants.Get();
        deferred->VSSetConstantBuffers(0, 1, &cb);
        deferred->PSSetConstantBuffers(0, 1, &cb);
    }

    void Renderer::complete() {
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(static_cast<int>(weft::constants::render::d3d11::completion_timeout_seconds));
        immediate->Flush();
        BOOL done = FALSE;
        for (;;) {
            const auto hr = immediate->GetData(host_done.Get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            throw_if_failed(hr, "GetData host completion");
            if (hr == S_OK && done) {
                break;
            }
            ensure(std::chrono::steady_clock::now() < deadline, "GPU completion deadline");
            std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<int>(weft::constants::render::d3d11::completion_poll_ms)));
        }
        BOOL ui = FALSE;
        throw_if_failed(immediate->GetData(ui_done.Get(), &ui, sizeof(ui), D3D11_ASYNC_GETDATA_DONOTFLUSH),
                        "UI completion");
        ensure(ui != FALSE, "host completion did not include UI");
        if (backend) {
            complete_backend();
        } else {
            lifetime(tracker.NotifyCompletedSerial(weft::constants::render::d3d11::device_epoch,
                                                   weft::constants::render::d3d11::immediate_queue, pending_completion),
                     "NotifyCompletedSerial actual shared host query");
        }
        acknowledged = true;
        collect();
    }

    void Renderer::enable_timing() {
        D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        throw_if_failed(device->CreateQuery(&desc, &timing_disjoint), "timing disjoint");
        desc.Query = D3D11_QUERY_TIMESTAMP;
        throw_if_failed(device->CreateQuery(&desc, &timing_start), "timing start");
        throw_if_failed(device->CreateQuery(&desc, &timing_end), "timing end");
    }

    void Renderer::check_debug_messages() {
        if (!info) {
            return;
        }
        const auto count = info->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T size{};
            throw_if_failed(info->GetMessage(i, nullptr, &size), "InfoQueue message size");
            std::vector<std::uint8_t> bytes(size);
            auto* m = reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
            throw_if_failed(info->GetMessage(i, m, &size), "InfoQueue message");
            if (m->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
                throw std::runtime_error(m->pDescription);
            }
        }
    }

    Backend& Renderer::ensure_backend() {
        if (!backend) {
            backend = new Backend(*this);
        }
        return *backend;
    }

    void Renderer::complete_backend() {
        render_result(weft_renderer_notify_completed(backend->renderer, weft::constants::render::d3d11::device_epoch,
                                                     weft::constants::render::d3d11::immediate_queue,
                                                     pending_completion),
                      "backend actual completion");
    }
} // namespace weft::render::d3d11
