#include "render/backends/d3d11/renderer.hpp"

#include "constants/common.hpp"
#include "render/backends/d3d11/backend.hpp"
#include "render/quad_list.hpp"
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
        // Compiled at build time from srgb_convert.hlsl (cmake/WeftShaders.cmake): /O3 with NDEBUG, /Zi /Od without.
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
        constexpr unsigned char encoded_convert_ps_bytes[] = {
#ifdef NDEBUG
#include "shaders/encoded_convert.ps.inc"
#else
#include "shaders/encoded_convert.ps.debug.inc"
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
        // The register of an effect's first texture (scene.hlsl, effectTexture0); its others follow.
        constexpr UINT effect_texture_slot = 3;

        // Writes box of resource from data in command order. Where the runtime emulates command lists (WARP), the
        // original UpdateSubresource misplaces the source of a box with a nonzero origin on a deferred context;
        // UpdateSubresource1 does not.
        void update(ID3D11DeviceContext* context, ID3D11Resource* resource, const D3D11_BOX& box, const void* data,
                    UINT pitch) {
            ComPtr<ID3D11DeviceContext1> context1;
            throw_if_failed(context->QueryInterface(IID_PPV_ARGS(&context1)), "ID3D11DeviceContext1");
            context1->UpdateSubresource1(resource, 0, &box, data, pitch, 0, 0);
        }

        // Writes values into buffer through context, first growing it to hold them. A driver that records command lists
        // itself takes the update in command order. Where the runtime emulates command lists (WARP), the draws of a large
        // list read zeros after either update call, so the buffer is dynamic and mapped with discard there.
        template <class T>
        ID3D11ShaderResourceView* upload(ID3D11Device* device, ID3D11DeviceContext* context, PassBuffer& buffer,
                                         const std::vector<T>& values, bool driver_command_lists, const char* what) {
            if (buffer.capacity < values.size()) {
                const auto capacity = std::max({values.size(), 2 * buffer.capacity,
                                                std::size_t(weft::constants::render::scene::record_initial_capacity)});
                D3D11_BUFFER_DESC d{};
                d.ByteWidth = UINT(capacity * sizeof(T));
                d.Usage = driver_command_lists ? D3D11_USAGE_DEFAULT : D3D11_USAGE_DYNAMIC;
                d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                d.CPUAccessFlags = driver_command_lists ? 0u : UINT(D3D11_CPU_ACCESS_WRITE);
                d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
                d.StructureByteStride = sizeof(T);
                PassBuffer grown;
                throw_if_failed(device->CreateBuffer(&d, nullptr, &grown.buffer), what);
                D3D11_SHADER_RESOURCE_VIEW_DESC v{};
                v.Format = DXGI_FORMAT_UNKNOWN;
                v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
                v.Buffer.NumElements = UINT(capacity);
                throw_if_failed(device->CreateShaderResourceView(grown.buffer.Get(), &v, &grown.view), what);
                grown.capacity = capacity;
                buffer = std::move(grown);
            }
            if (driver_command_lists) {
                update(context, buffer.buffer.Get(), {0, 0, 0, UINT(values.size() * sizeof(T)), 1, 1}, values.data(),
                       0);
            } else {
                D3D11_MAPPED_SUBRESOURCE mapped{};
                throw_if_failed(context->Map(buffer.buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), what);
                std::memcpy(mapped.pData, values.data(), values.size() * sizeof(T));
                context->Unmap(buffer.buffer.Get(), 0);
            }
            return buffer.view.Get();
        }

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

        // One item over the whole w x h px target.
        paint::Item whole(paint::Item::Kind kind, UINT w, UINT h) {
            paint::Item item;
            item.kind = kind;
            item.clip = {0, 0, float(w), float(h)};
            item.width = float(w);
            item.height = float(h);
            return item;
        }

        // The image of the list's one external texture over the whole target.
        paint::DisplayList composite(UINT w, UINT h) {
            paint::DisplayList list;
            list.external_textures = 1;
            list.images.emplace_back();
            list.items.push_back(whole(paint::Item::Kind::Image, w, h));
            return list;
        }

        // A box of one straight sRGB color over the whole target.
        paint::DisplayList fill(UINT w, UINT h, const paint::Color& color) {
            paint::DisplayList list;
            list.boxes.emplace_back().background = color;
            list.items.push_back(whole(paint::Item::Kind::Box, w, h));
            return list;
        }
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

    Renderer::Renderer(bool warp, UINT w, UINT h, bool with_debug_layer) : width(w), height(h) {
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
        auto hr = with_debug_layer ? create(D3D11_CREATE_DEVICE_DEBUG) : create(0);
        if (with_debug_layer && hr == DXGI_ERROR_SDK_COMPONENT_MISSING) {
            hr = create(0);
        } else if (with_debug_layer && SUCCEEDED(hr)) {
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
        {
            D3D11_FEATURE_DATA_THREADING threading{};
            throw_if_failed(device->CheckFeatureSupport(D3D11_FEATURE_THREADING, &threading, sizeof(threading)),
                            "threading support");
            driver_command_lists = threading.DriverCommandLists != FALSE;
        }
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
        throw_if_failed(device->CreatePixelShader(encoded_convert_ps_bytes, sizeof(encoded_convert_ps_bytes), nullptr,
                                                  &encoded_convert_ps),
                        "Create encoded conversion pixel shader");
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
        s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        throw_if_failed(device->CreateSamplerState(&s, &effect_sampler), "CreateSamplerState effects");
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
        return asset_identities.identify(source);
    }

    void Renderer::evict_assets(std::size_t required, bool all) {
        ensure(!submitted || acknowledged, "GPU cache retirement requires actual completion");
        ensure(!all || !async_in_flight, "GPU cache drain requires all asynchronous host completions");
        cache.evict(tracker, required, all, [&](const TextureCache<NativeAsset>::Entry& evicted) {
            if (evicted.native.atlas) {
                atlas.release(*evicted.native.atlas);
            }
            ++native_releases;
        });
        if (all) {
            collect();
            assets.clear();
            asset_identities.clear();
        }
    }

    PreparedFrame Renderer::prepare(FramePacket packet,
                                    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& keys,
                                    std::uint32_t external_textures) {
        if (packet.list) {
            ensure(packet.draws.empty() && packet.assets.empty() && !external_textures,
                   "a list packet holds only its list");
            return prepare(std::move(packet.list), keys);
        }
        return prepare(std::make_shared<const paint::DisplayList>(quad_list(packet, external_textures, width, height)),
                       keys);
    }

    PreparedFrame Renderer::prepare(paint::DisplayList list,
                                    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& keys) {
        return prepare(std::make_shared<const paint::DisplayList>(std::move(list)), keys);
    }

    PreparedFrame Renderer::prepare(std::shared_ptr<const paint::DisplayList> list,
                                    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& keys) {
        ensure(list && bool(paint::check(*list, weft::constants::render::scene::max_group_depth)),
               "well-formed display list");
        for (const auto& item : list->items) {
            ensure(item.kind != paint::Item::Kind::Text && item.kind != paint::Item::Kind::Layer &&
                       item.kind != paint::Item::Kind::Shadow && item.kind != paint::Item::Kind::Path &&
                       item.kind != paint::Item::Kind::Plane && item.kind != paint::Item::Kind::Particles &&
                       item.kind != paint::Item::Kind::Backdrop,
                   "the display list pass draws no scene rasters, shadows, paths, planes, particles or backdrops");
            if (item.kind == paint::Item::Kind::Image) {
                const auto& image = list->images[item.payload];
                ensure(image.stroke >= 0 && (image.asset >= list->assets.size() ||
                                             list->assets[image.asset]->encoding != Encoding::SignedDistance ||
                                             image.stroke < list->assets[image.asset]->spread - 1),
                       "SDF stroke exceeds distance padding");
            }
        }
        // An asset an effect samples has a texture of its own, never an atlas region, under a key of its own: an image
        // of the same asset on the atlas stays apart.
        std::vector<bool> standalone(list->assets.size());
        for (const auto& effect : list->effects) {
            for (const auto t : effect.textures) {
                standalone[t] = true;
            }
        }
        auto keyed = keys;
        for (std::size_t i = 0; cache_assets && i < keyed.size() && i < standalone.size(); ++i) {
            if (standalone[i]) {
                keyed[i].second |= weft::constants::render::texture_cache::standalone_key_bit;
            }
        }
        PreparedFrame frame;
        frame.list = std::move(list);
        const auto& sources = frame.list->assets;
        frame.dependencies.reserve(sources.size());
        assets.reserve(sources.size());

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
        } existing{*this, std::vector<std::optional<ResourceId>>(cache_assets ? keyed.size() : 0)};

        // Protect the ENTIRE incoming dependency set before creating anything.
        // LRU cannot evict a later draw's hit while processing an earlier miss.
        if (cache_assets) {
            for (std::size_t i = 0; i < keyed.size(); ++i) {
                if (const auto found = cache.entries.find(keyed[i]); found != cache.entries.end()) {
                    lifetime(tracker.RetainResource(found->second.native.id), "pin all incoming cached dependencies");
                    existing.values[i] = found->second.native.id;
                }
            }
        }
        for (std::size_t i = 0; i < sources.size(); ++i) {
            const auto& a = *sources[i];
            NativeAsset n;
            const bool r8 = a.encoding != Encoding::SrgbStraight;
            const auto key =
                cache_assets ? keyed.at(i) : std::pair<std::uint64_t, std::uint64_t>{i + 1, window_serial + 1};
            if (cache_assets) {
                if (auto* found = cache.use(key)) {
                    auto& cached = *found;
                    ResourceInfo resource_state;
                    lifetime(tracker.InspectResource(cached.native.id, resource_state),
                             "inspect cached immutable asset");
                    n.id = cached.native.id;
                    n.texture = cached.native.texture;
                    n.srv = cached.native.srv;
                    n.pitch = cached.native.pitch;
                    n.atlas = cached.native.atlas;
                    n.cache_copy = true;
                    if (resource_state.phase != ResourcePhase::Ready &&
                        resource_state.phase != ResourcePhase::Submitted) {
                        n.upload = cached.native.upload;
                        n.source = cached.native.source;
                        n.source_srv = cached.native.source_srv;
                        n.rtv = cached.native.rtv;
                    }
                    ensure(existing.values[i] && *existing.values[i] == n.id, "complete cached dependency lease");
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
            auto resource_bytes =
                convert ? static_cast<std::size_t>(a.width) * a.height * sizeof(Color) : n.upload.size();
            // Capacity failure must happen before allocating a tracked resource.
            // All incoming hits are already pinned; a full packet that exceeds
            // the cache cannot leave an unowned CPU lease behind on rejection.
            if (cache_assets) {
                evict_assets(resource_bytes);
            }
            if (cache_assets && atlas_candidate(a) && !standalone[i]) {
                n.atlas = place_on_atlas(a, n.upload);
            }
            if (n.atlas) {
                const auto& page = atlas_pages.at(n.atlas->page);
                n.texture = page.texture;
                n.srv = page.srv;
                n.pitch = n.atlas->width;
                resource_bytes = n.upload.size();
            } else {
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
            }
            lifetime(tracker.CreateResource({key.first, key.second, resource_bytes, ResourceKind::Texture}, n.id),
                     "CreateResource");
            ++native_creations;
            if (cache_assets) {
                const auto bytes = resource_bytes;
                const auto resource = n.id;
                const auto region = n.atlas;
                try {
                    cache.add(key, std::move(n), bytes);
                } catch (...) {
                    lifetime(tracker.ReleaseResource(resource), "failed cache insertion rollback");
                    if (region) {
                        atlas.release(*region);
                    }
                    ++native_releases;
                    throw;
                }
                auto& cached = cache.entries.at(key);
                n.id = cached.native.id;
                n.texture = cached.native.texture;
                n.srv = cached.native.srv;
                n.upload = cached.native.upload;
                n.pitch = cached.native.pitch;
                n.source = cached.native.source;
                n.source_srv = cached.native.source_srv;
                n.rtv = cached.native.rtv;
                n.atlas = cached.native.atlas;
                n.cache_copy = true;
                lifetime(tracker.RetainResource(n.id), "new cached frame lease");
            }
            frame.dependencies.push_back(n.id);
            assets.push_back(std::move(n));
        }
        lifetime(tracker.PrepareFrame(frame.dependencies, frame.id), "PrepareFrame");
        return frame;
    }

    std::optional<AtlasPacker::Region> Renderer::place_on_atlas(const Asset& a, std::vector<std::uint8_t>& upload) {
        constexpr auto border = std::uint32_t(weft::constants::render::texture_cache::atlas_border);
        const auto pages = atlas.pages();
        auto region = atlas.allocate(a.width + 2 * border, a.height + 2 * border);
        if (!region) {
            return std::nullopt;
        }
        if (atlas.pages() > pages) {
            AtlasPage page;
            D3D11_TEXTURE2D_DESC d{};
            d.Width = d.Height = atlas.page_size();
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.SampleDesc.Count = 1;
            d.Format = DXGI_FORMAT_R8_UNORM;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            try {
                throw_if_failed(device->CreateTexture2D(&d, nullptr, &page.texture), "Create atlas page");
                throw_if_failed(device->CreateShaderResourceView(page.texture.Get(), nullptr, &page.srv),
                                "Create atlas page SRV");
                atlas_pages.push_back(std::move(page));
            } catch (...) {
                atlas.release(*region);
                throw;
            }
        }
        upload = bordered(a);
        ++atlas_placements;
        return region;
    }

    std::array<float, 4> Renderer::atlas_region(const NativeAsset& a) const {
        return a.atlas ? region_uv(*a.atlas) : std::array<float, 4>{0, 0, 1, 1};
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
        for (auto& [key, cached] : cache.entries) {
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
                    if (a.atlas) {
                        const auto& r = *a.atlas;
                        update(deferred.Get(), a.texture.Get(), {r.x, r.y, 0, r.x + r.width, r.y + r.height, 1},
                               a.upload.data(), a.pitch);
                    } else {
                        deferred->UpdateSubresource(a.source ? a.source.Get() : a.texture.Get(), 0, nullptr,
                                                    a.upload.data(), a.pitch, 0);
                    }
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
            auto* target = native_target ? native_target : destination.rtv.Get();
            if (!native_target || clear_native_target) {
                deferred->ClearRenderTargetView(target, native_target && native_clear ? native_clear : transparent);
            }
            std::vector<ImageView> images;
            images.reserve(assets.size() + frame.external.size());
            for (const auto& a : assets) {
                images.push_back({a.srv.Get(), false, atlas_region(a)});
            }
            for (const auto& e : frame.external) {
                images.push_back({e.srv.Get(), e.bottom_left});
            }
            record(deferred.Get(), *frame.list, target, width, height, images);
            if (rtt) {
                deferred->ClearRenderTargetView(output.rtv.Get(), transparent);
                record(deferred.Get(), composite(width, height), output.rtv.Get(), width, height,
                       {{intermediate.srv.Get()}});
            }
            // A transparent linear destination checks premultiplied RGB/alpha
            // independently of the opaque screenshot's alpha=1 background.
            if (!native_target) {
                constexpr paint::Color half_green{0, 1, 0, .5f};
                deferred->ClearRenderTargetView(group.rtv.Get(), transparent);
                record(deferred.Get(), fill(width, height, half_green), group.rtv.Get(), width, height, {});
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

    void Renderer::record(ID3D11DeviceContext* context, const paint::DisplayList& list, ID3D11RenderTargetView* target,
                          UINT target_width, UINT target_height, const std::vector<ImageView>& images) {
        // Images on one texture (an atlas page) draw together with the boxes between them.
        std::vector<std::uint64_t> textures;
        textures.reserve(images.size());
        for (const auto& image : images) {
            textures.push_back(std::uint64_t(reinterpret_cast<std::uintptr_t>(image.view)));
        }
        auto planned = plan_scene_pass(list, 1, target_width, target_height, std::nullopt, Space::Linear, &textures);
        ensure(bool(planned), "display list pass plan");
        auto& pass = *planned;
        for (std::size_t i = 0; i < list.items.size(); ++i) {
            if (list.items[i].kind == paint::Item::Kind::Image) {
                const auto& image = images.at(list.images[list.items[i].payload].asset);
                place_in(pass.records[i], image.region);
                if (image.bottom_up) {
                    flip_rows(pass.records[i]);
                }
            }
        }
        if (pass.clear != paint::Color{}) {
            context->ClearRenderTargetView(target, pass.clear.data());
        }
        if (pass.batches.empty()) {
            return;
        }
        auto* record_view =
            upload(device.Get(), context, display_records, pass.records, driver_command_lists, "display list records");
        auto* clip_view = pass.clips.empty() ? nullptr
                                             : upload(device.Get(), context, display_clips, pass.clips,
                                                      driver_command_lists, "display list clips");
        const D3D11_VIEWPORT viewport{0, 0, float(target_width), float(target_height), 0, 1};
        context->RSSetViewports(1, &viewport);
        const D3D11_RECT scissor{0, 0, LONG(target_width), LONG(target_height)};
        context->RSSetScissorRects(1, &scissor);
        context->RSSetState(raster.Get());
        context->OMSetDepthStencilState(depth.Get(), 0);
        context->OMSetBlendState(blend.Get(), nullptr, D3D11_DEFAULT_SAMPLE_MASK);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(display_vs.Get(), nullptr, 0);
        context->PSSetShader(display_ps.Get(), nullptr, 0);
        auto* cb = display_constants.Get();
        context->VSSetConstantBuffers(0, 1, &cb);
        context->PSSetConstantBuffers(0, 1, &cb);
        auto* ss = sampler.Get();
        context->PSSetSamplers(0, 1, &ss);
        ID3D11ShaderResourceView* views[3]{record_view, nullptr, clip_view};
        context->VSSetShaderResources(0, 1, views);
        context->PSSetShaderResources(0, 3, views);
        context->OMSetRenderTargets(1, &target, nullptr);
        std::array<float, 4> view{float(target_width), float(target_height), 1, 0};
        auto draw = [&](std::uint32_t first, std::uint32_t count, ID3D11ShaderResourceView* image) {
            view[3] = float(first);
            context->UpdateSubresource(display_constants.Get(), 0, nullptr, view.data(), 0, 0);
            context->PSSetShaderResources(1, 1, &image);
            context->DrawInstanced(weft::constants::render::d3d11::quad_vertex_count, count, 0, 0);
            ++display_draws;
            display_instances += count;
        };
        // The target, then one layer per open group.
        std::vector<ID3D11RenderTargetView*> targets{target};
        for (const auto& batch : pass.batches) {
            switch (batch.step) {
                case ScenePass::Step::Boxes:
                    draw(batch.first, batch.count, nullptr);
                    break;
                case ScenePass::Step::Image:
                    draw(batch.first, batch.count, images.at(batch.source).view);
                    break;
                case ScenePass::Step::BeginGroup: {
                    ensure(target_width == width && target_height == height, "groups draw at the renderer's size");
                    const auto nesting = targets.size() - 1;
                    while (group_layers.size() <= nesting) {
                        group_layers.push_back(surface(DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT));
                    }
                    auto* layer = group_layers[nesting].rtv.Get();
                    // The layer may still be the image of the composite that ended its previous group.
                    ID3D11ShaderResourceView* none{};
                    context->PSSetShaderResources(1, 1, &none);
                    const float transparent[4]{};
                    context->ClearRenderTargetView(layer, transparent);
                    context->OMSetRenderTargets(1, &layer, nullptr);
                    targets.push_back(layer);
                    break;
                }
                case ScenePass::Step::EndGroup:
                    targets.pop_back();
                    context->OMSetRenderTargets(1, &targets.back(), nullptr);
                    draw(batch.first, 1, group_layers[targets.size() - 1].srv.Get());
                    break;
                case ScenePass::Step::Effect: {
                    // The effect's pixel stage over its records, its textures at t3 on, repeated by s1.
                    const auto& effect = list.effects[batch.source];
                    std::array<ID3D11ShaderResourceView*, weft::constants::max_effect_textures> effect_views{};
                    for (std::size_t t = 0; t < effect.textures.size(); ++t) {
                        effect_views[t] = images.at(effect.textures[t]).view;
                    }
                    context->PSSetShader(effect_shader(list.programs[effect.program]), nullptr, 0);
                    context->PSSetShaderResources(effect_texture_slot, UINT(effect_views.size()), effect_views.data());
                    auto* repeat = effect_sampler.Get();
                    context->PSSetSamplers(1, 1, &repeat);
                    draw(batch.first, batch.count, nullptr);
                    context->PSSetShader(display_ps.Get(), nullptr, 0);
                    break;
                }
                case ScenePass::Step::Atlas:
                case ScenePass::Step::Particles:
                case ScenePass::Step::Backdrop:
                    // prepare refuses particles and backdrops: they draw in the scene pass alone, as atlas pages do.
                    break;
            }
        }
        std::array<ID3D11ShaderResourceView*, effect_texture_slot + weft::constants::max_effect_textures> empty{};
        context->VSSetShaderResources(0, 1, empty.data());
        context->PSSetShaderResources(0, UINT(empty.size()), empty.data());
    }

    ID3D11PixelShader* Renderer::effect_shader(const std::shared_ptr<const paint::EffectProgram>& program) {
        if (const auto found = effect_shaders_.find(program.get()); found != effect_shaders_.end()) {
            return found->second.shader.Get();
        }
        ensure(!program->d3d11_linear.empty(), "an effect built for D3D11");
        EffectShader made{program, {}};
        throw_if_failed(device->CreatePixelShader(program->d3d11_linear.data(), program->d3d11_linear.size(), nullptr,
                                                  &made.shader),
                        "CreatePixelShader effect");
        return effect_shaders_.emplace(program.get(), std::move(made)).first->second.shader.Get();
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
