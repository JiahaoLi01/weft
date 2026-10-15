#include "render/backends/d3d11/backend.hpp"

#include "render/frame_input.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace weft::render::d3d11 {
    namespace {
        namespace limits = weft::constants::render::d3d11;
    } // namespace

    void render_result(weft_render_result result, const char* operation) {
        if (result != WEFT_RENDER_OK) {
            throw std::runtime_error(std::string(operation) + " result=" + std::to_string(result));
        }
    }

    Backend::Backend(Renderer& device) : gpu(device) {
        const auto table =
            make_c_backend(*this, {WEFT_RENDER_API_D3D11,
                                   limits::device_epoch,
                                   limits::immediate_queue,
                                   {sizeof(weft_render_capabilities), limits::features, limits::target_formats,
                                    limits::max_surface_dimension, limits::max_surface_dimension, 0}});
        // A real game workload contains three 1024x1536 RGBA images. The
        // reference adapter reserves a bounded 32 MiB CPU frame input budget.
        const weft_renderer_desc desc{sizeof(weft_renderer_desc),
                                      WEFT_RENDER_ABI_VERSION,
                                      limits::max_prepared_frames,
                                      limits::frames_in_flight,
                                      limits::frames_in_flight,
                                      weft::constants::render::default_max_draws,
                                      weft::constants::render::default_max_assets,
                                      0,
                                      limits::max_frame_asset_bytes};
        render_result(weft_renderer_create(&table, &desc, &renderer), "create D3D11 backend interface");
    }

    Backend::~Backend() {
        if (weft_renderer_destroy(renderer) != WEFT_RENDER_OK) {
            std::terminate();
        }
    }

    void Backend::enable_external(unsigned capacity, std::uint64_t bytes) {
        const auto table = make_c_external_backend(*this, {(1u << WEFT_RENDER_TARGET_LINEAR_RGBA8) |
                                                               (1u << WEFT_RENDER_TARGET_LINEAR_RGBA16F) |
                                                               (1u << WEFT_RENDER_TARGET_LINEAR_RGBA32F),
                                                           capacity, bytes});
        render_result(weft_renderer_install_external_backend(renderer, &table), "install external texture extension");
    }

    weft_render_result check_external_texture(const weft_render_external_texture& description, ID3D11Device* device,
                                              ComPtr<ID3D11ShaderResourceView>& view,
                                              ComPtr<ID3D11Resource>& resource) {
        const auto* d = &description;
        if (d->native_texture_size < sizeof(weft_render_d3d11_texture)) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        const auto& p = *static_cast<const weft_render_d3d11_texture*>(d->native_texture);
        if (p.struct_size < sizeof(p) || p.abi_version != WEFT_RENDER_EXTERNAL_ABI_VERSION || p.reserved ||
            !p.shader_resource_view) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        auto* srv = static_cast<ID3D11ShaderResourceView*>(p.shader_resource_view);
        ComPtr<ID3D11Device> owner;
        srv->GetDevice(&owner);
        if (owner.Get() != device) {
            return WEFT_RENDER_WRONG_BACKEND;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        srv->GetDesc(&desc);
        const auto format = d->format == WEFT_RENDER_TARGET_LINEAR_RGBA8     ? DXGI_FORMAT_R8G8B8A8_UNORM
                            : d->format == WEFT_RENDER_TARGET_SRGB_RGBA8     ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                            : d->format == WEFT_RENDER_TARGET_LINEAR_BGRA8   ? DXGI_FORMAT_B8G8R8A8_UNORM
                            : d->format == WEFT_RENDER_TARGET_SRGB_BGRA8     ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
                            : d->format == WEFT_RENDER_TARGET_LINEAR_RGBA16F ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                                                             : DXGI_FORMAT_R32G32B32A32_FLOAT;
        srv->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(resource.As(&texture))) {
            return WEFT_RENDER_UNSUPPORTED_FEATURE;
        }
        D3D11_TEXTURE2D_DESC actual{};
        texture->GetDesc(&actual);
        // The view's levels: from the most detailed one, as many as the description says (all of the texture's when
        // the view takes the rest).
        const UINT levels = std::max(1u, d->mip_levels);
        const UINT viewed = desc.Texture2D.MipLevels == UINT(-1) ? actual.MipLevels - desc.Texture2D.MostDetailedMip
                                                                 : desc.Texture2D.MipLevels;
        if (desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || desc.Format != format ||
            desc.Texture2D.MostDetailedMip || viewed != levels) {
            return WEFT_RENDER_UNSUPPORTED_FEATURE;
        }
        if (actual.Width != d->width || actual.Height != d->height) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        // A texture the host may make its levels of (GENERATE_MIPS), or one another device or API writes, shared and
        // opened on this device (the host orders those writes with the UI's reads); no arrays, multisampling or CPU
        // access.
        constexpr UINT taken = D3D11_RESOURCE_MISC_GENERATE_MIPS | D3D11_RESOURCE_MISC_SHARED |
                               D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        if (actual.ArraySize != 1 || actual.MipLevels < levels || actual.SampleDesc.Count != 1 ||
            (actual.MiscFlags & ~taken) || actual.CPUAccessFlags ||
            (actual.Usage != D3D11_USAGE_DEFAULT && actual.Usage != D3D11_USAGE_IMMUTABLE)) {
            return WEFT_RENDER_UNSUPPORTED_FEATURE;
        }
        view = srv;
        return WEFT_RENDER_OK;
    }

    weft_render_result Backend::import_texture(const weft_render_external_texture& description, std::uint64_t& out) {
        out = 0;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11Resource> resource;
        if (const auto v = check_external_texture(description, gpu.device.Get(), srv, resource); v != WEFT_RENDER_OK) {
            return v;
        }
        for (const auto& t : imports) {
            if (t.resource.Get() == resource.Get()) {
                return WEFT_RENDER_INVALID_STATE;
            }
        }
        for (size_t i = 0; i < imports.size(); ++i) {
            if (!imports[i].srv) {
                imports[i] = {srv, resource};
                out = i + 1;
                ++imported_count;
                return WEFT_RENDER_OK;
            }
        }
        return WEFT_RENDER_BUSY;
    }

    void Backend::release_texture(std::uint64_t native) {
        if (!native || native > imports.size() || !imports[native - 1].srv) {
            std::terminate();
        }
        imports[native - 1] = {};
        ++released_imports;
    }

    weft_render_result Backend::external_views(const weft_render_external_asset* textures, std::uint32_t count,
                                               std::vector<ExternalGpuAsset>& out) {
        out.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const auto id = textures[i].native_texture;
            if (!id || id > imports.size() || !imports[id - 1].srv) {
                return WEFT_RENDER_STALE_HANDLE;
            }
            out.push_back({imports[id - 1].srv, textures[i].description.origin == WEFT_RENDER_EXTERNAL_BOTTOM_LEFT});
        }
        return WEFT_RENDER_OK;
    }

    weft_render_result Backend::prepare_external(const weft_render_frame& f, const weft_render_external_asset* textures,
                                                 std::uint32_t count, std::uint64_t& out) {
        out = 0;
        std::vector<ExternalGpuAsset> external;
        try {
            if (const auto result = external_views(textures, count, external); result != WEFT_RENDER_OK) {
                return result;
            }
        } catch (...) {
            return WEFT_RENDER_OUT_OF_MEMORY;
        }
        return prepare_quads(f, std::move(external), out);
    }

    weft_render_result Backend::prepare_list(const weft_paint_list& list, const weft_render_external_asset* textures,
                                             std::uint32_t count, std::uint64_t& out) {
        out = 0;
        std::vector<ExternalGpuAsset> external;
        try {
            if (const auto result = external_views(textures, count, external); result != WEFT_RENDER_OK) {
                return result;
            }
        } catch (...) {
            return WEFT_RENDER_OUT_OF_MEMORY;
        }
        return prepare_with(std::move(external), out, [&] {
            // A list shares its immutable assets; their identities are the cache keys.
            std::vector<std::pair<std::uint64_t, std::uint64_t>> keys;
            if (gpu.cache_assets) {
                keys.reserve(list.list->assets.size());
                for (const auto& asset : list.list->assets) {
                    keys.emplace_back(gpu.asset_identity(asset), 1);
                }
            }
            return gpu.prepare(list.list, keys);
        });
    }

    weft_render_result Backend::prepare(const weft_render_frame& frame, std::uint64_t& out) {
        return prepare_quads(frame, {}, out);
    }

    weft_render_result Backend::prepare_quads(const weft_render_frame& frame, std::vector<ExternalGpuAsset> external,
                                              std::uint64_t& out) {
        const auto external_count = std::uint32_t(external.size());
        return prepare_with(std::move(external), out, [&] {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> keys;
            keys.reserve(frame.asset_count);
            for (uint32_t i = 0; i < frame.asset_count; ++i) {
                keys.emplace_back(frame.assets[i].asset_id, frame.assets[i].asset_version);
            }
            auto packet = frame_packet(frame, [&](std::uint32_t i) {
                return !gpu.cache_assets || !gpu.cache.entries.contains(keys[i]);
            });
            return gpu.prepare(std::move(packet), keys, external_count);
        });
    }

    template <class Make>
    weft_render_result Backend::prepare_with(std::vector<ExternalGpuAsset> external, std::uint64_t& out, Make&& make) {
        out = 0;
        const auto state = gpu.tracker.Stats();
        if (prepared || state.encoded_tokens || (state.submitted_tokens && !gpu.pipeline_enabled)) {
            return WEFT_RENDER_BUSY;
        }
        gpu.assets.clear();
        try {
            prepared = make();
            prepared->external = std::move(external);
            out = limits::prepared_frame_handle;
            return WEFT_RENDER_OK;
        } catch (...) {
            try {
                throw;
            } catch (const std::exception& error) {
                try {
                    std::cerr << "D3D11 prepare: " << error.what() << '\n';
                } catch (...) {
                }
            } catch (...) {
            }
            // Preparation did not record work. Undo all created CPU leases.
            for (const auto& a : gpu.assets) {
                if (gpu.tracker.ReleaseResource(a.id) != LifetimeResult::Ok) {
                    std::terminate();
                }
            }
            gpu.collect();
            gpu.assets.clear();
            return WEFT_RENDER_BACKEND_ERROR;
        }
    }

    void Backend::release_prepared(std::uint64_t native) {
        if (native != limits::prepared_frame_handle || !prepared ||
            gpu.tracker.ReleasePreparedFrame(prepared->id) != LifetimeResult::Ok) {
            std::terminate();
        }
        prepared.reset();
        for (const auto& a : gpu.assets) {
            if (gpu.tracker.ReleaseResource(a.id) != LifetimeResult::Ok) {
                std::terminate();
            }
        }
        gpu.collect();
    }

    weft_render_result Backend::encode(std::uint64_t native, const weft_render_pass& render_pass, std::uint64_t& out) {
        const auto* pass = &render_pass;
        out = 0;
        if (native != limits::prepared_frame_handle || !prepared ||
            pass->native_pass_size < sizeof(weft_render_d3d11_pass)) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        const auto& p = *static_cast<const weft_render_d3d11_pass*>(pass->native_pass);
        if (p.struct_size < sizeof(p) || p.abi_version != WEFT_RENDER_ABI_VERSION || !p.out_command_list) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        *p.out_command_list = nullptr;
        if (p.clear_target > 1 || p.reserved) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        if (p.clear_target) {
            for (float c : p.clear_linear_premultiplied) {
                if (!std::isfinite(c) || c < 0 || c > 1) {
                    return WEFT_RENDER_INVALID_ARGUMENT;
                }
            }
            for (size_t i = 0; i < 3; ++i) {
                if (p.clear_linear_premultiplied[i] > p.clear_linear_premultiplied[3]) {
                    return WEFT_RENDER_INVALID_ARGUMENT;
                }
            }
        }
        if (!p.deferred_context || !p.render_target_view || pass->width != gpu.width || pass->height != gpu.height) {
            return WEFT_RENDER_UNSUPPORTED_FEATURE;
        }
        try {
            auto* context = static_cast<ID3D11DeviceContext*>(p.deferred_context);
            auto* target = static_cast<ID3D11RenderTargetView*>(p.render_target_view);
            if (context->GetType() != D3D11_DEVICE_CONTEXT_DEFERRED) {
                return WEFT_RENDER_INVALID_ARGUMENT;
            }
            ComPtr<ID3D11Device> context_device;
            ComPtr<ID3D11Device> target_device;
            context->GetDevice(&context_device);
            target->GetDevice(&target_device);
            if (context_device.Get() != gpu.device.Get() || target_device.Get() != gpu.device.Get()) {
                return WEFT_RENDER_WRONG_BACKEND;
            }
            D3D11_RENDER_TARGET_VIEW_DESC view{};
            target->GetDesc(&view);
            const auto expected = pass->target_format == WEFT_RENDER_TARGET_SRGB_RGBA8 ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                  : pass->target_format == WEFT_RENDER_TARGET_LINEAR_RGBA8
                                      ? DXGI_FORMAT_R8G8B8A8_UNORM
                                      : DXGI_FORMAT_R32G32B32A32_FLOAT;
            if (view.Format != expected || view.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D ||
                view.Texture2D.MipSlice) {
                return WEFT_RENDER_UNSUPPORTED_FEATURE;
            }
            ComPtr<ID3D11Resource> resource;
            target->GetResource(&resource);
            ComPtr<ID3D11Texture2D> texture;
            throw_if_failed(resource.As(&texture), "backend target texture");
            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);
            if (description.Width != pass->width || description.Height != pass->height ||
                description.SampleDesc.Count != 1) {
                return WEFT_RENDER_INVALID_ARGUMENT;
            }
            for (const auto& external : prepared->external) {
                ComPtr<ID3D11Resource> source;
                external.srv->GetResource(&source);
                if (source.Get() == resource.Get()) {
                    return WEFT_RENDER_INVALID_STATE;
                }
            }
            for (size_t i = 0; i < uses.size(); ++i) {
                if (!uses[i]) {
                    auto encoded = gpu.encode(*prepared, false, inject_recording_failure, context, target,
                                              p.clear_target != 0, p.clear_linear_premultiplied);
                    uses[i] = encoded.token;
                    out = i + 1;
                    *p.out_command_list = encoded.commands.Detach();
                    return WEFT_RENDER_OK;
                }
            }
            return WEFT_RENDER_BUSY;
        } catch (...) {
            // Renderer::encode closes/discards its partial list before
            // releasing pins; no executable partial commands escape this ABI.
            return WEFT_RENDER_BACKEND_ERROR;
        }
    }

    void Backend::discard_encoded(std::uint64_t native) {
        auto& use = uses[native - 1];
        if (!use || gpu.tracker.DiscardEncoded(*use) != LifetimeResult::Ok) {
            std::terminate();
        }
        use.reset();
        gpu.collect();
    }

    void Backend::submitted(std::uint64_t native, std::uint64_t serial) {
        auto& use = uses[native - 1];
        if (!use || gpu.tracker.NotifySubmitted(*use, limits::immediate_queue, serial) != LifetimeResult::Ok) {
            std::terminate();
        }
    }

    void Backend::completed(std::uint64_t serial) {
        if (gpu.tracker.NotifyCompletedSerial(limits::device_epoch, limits::immediate_queue, serial) !=
            LifetimeResult::Ok) {
            std::terminate();
        }
        for (auto& use : uses) {
            if (use) {
                UseTokenInfo info;
                if (gpu.tracker.InspectUseToken(*use, info) == LifetimeResult::StaleHandle) {
                    use.reset();
                }
            }
        }
        gpu.collect();
    }

    weft_render_result Backend::shutdown() {
        try {
            gpu.evict_assets(0, true);
        } catch (...) {
            return WEFT_RENDER_BACKEND_ERROR;
        }
        const auto result = gpu.tracker.Shutdown();
        return result == LifetimeResult::Ok     ? WEFT_RENDER_OK
               : result == LifetimeResult::Busy ? WEFT_RENDER_BUSY
                                                : WEFT_RENDER_BACKEND_ERROR;
    }

    void Backend::abandon() {
        if (gpu.tracker.AbandonDeviceEpoch(
                limits::device_epoch, DeviceAbandonConfirmation::HostConfirmedNoFurtherAccess) != LifetimeResult::Ok) {
            std::terminate();
        }
        // Host proof replaces an unattainable completion query on this device.
        gpu.acknowledged = true;
        for (auto& use : uses) {
            use.reset();
        }
        gpu.collect();
    }

    weft_render_pass Backend::pass(ID3D11DeviceContext* context, ID3D11RenderTargetView* target, void** commands,
                                   weft_render_d3d11_pass& payload) {
        weft_render_info info{};
        info.struct_size = sizeof(info);
        render_result(weft_renderer_get_info(renderer, &info), "backend info");
        payload = {sizeof(payload), WEFT_RENDER_ABI_VERSION, context, target, commands, 1, 0, {0, 0, 0, 0}};
        return {sizeof(weft_render_pass),
                WEFT_RENDER_ABI_VERSION,
                info.identity,
                gpu.width,
                gpu.height,
                WEFT_RENDER_TARGET_SRGB_RGBA8,
                1,
                &payload,
                sizeof(payload)};
    }

    PacketView::PacketView(const FramePacket& packet, uint64_t version, Renderer* cache) {
        if (packet.list) {
            ensure(packet.draws.empty() && packet.assets.empty(), "a list packet holds only its list");
            list.list = packet.list;
            return;
        }
        for (size_t i = 0; i < packet.assets.size(); ++i) {
            const auto& a = *packet.assets[i];
            assets.push_back({cache ? cache->asset_identity(packet.assets[i]) : i + 1, cache ? 1 : version, a.width,
                              a.height, static_cast<uint32_t>(a.encoding), 0, a.bytes.data(), a.bytes.size(), a.spread,
                              0});
        }
        for (const auto& d : packet.draws) {
            ensure(!d.group && !d.clipped, "backend v1 supports ordered quads, no diagnostic group/clip flags");
            weft_render_draw draw{
                d.rect.x,  d.rect.y, d.rect.w, d.rect.h, {}, d.asset < 0 ? WEFT_RENDER_SOLID_ASSET : uint32_t(d.asset),
                d.opacity, {},       d.stroke, {}};
            std::copy(d.srgb.begin(), d.srgb.end(), draw.srgb);
            std::copy(d.uv.begin(), d.uv.end(), draw.uv);
            std::copy(d.outline.begin(), d.outline.end(), draw.outline_srgb);
            draws.push_back(draw);
        }
        frame = {sizeof(frame),          WEFT_RENDER_ABI_VERSION, draws.data(),
                 uint32_t(draws.size()), uint32_t(assets.size()), assets.data()};
    }

    weft_render_result PacketView::prepare(weft_renderer* renderer, weft_render_handle& out,
                                           const std::vector<weft_render_handle>& external) const {
        const auto count = uint32_t(external.size());
        if (list.list) {
            const weft_render_list_frame input{
                sizeof(input), WEFT_RENDER_ABI_VERSION, &list, count, 0, count ? external.data() : nullptr};
            return weft_renderer_prepare_list(renderer, &input, &out);
        }
        if (!count) {
            return weft_renderer_prepare(renderer, &frame, &out);
        }
        const weft_render_external_frame input{sizeof(input),  WEFT_RENDER_EXTERNAL_ABI_VERSION, frame, count, 0,
                                               external.data()};
        return weft_renderer_prepare_external(renderer, &input, &out);
    }
} // namespace weft::render::d3d11
