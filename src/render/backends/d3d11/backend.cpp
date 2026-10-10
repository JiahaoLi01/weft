#include "render/backends/d3d11/backend.hpp"

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
        const weft_render_backend table{sizeof(weft_render_backend),
                                        WEFT_RENDER_ABI_VERSION,
                                        WEFT_RENDER_API_D3D11,
                                        0,
                                        limits::device_epoch,
                                        limits::immediate_queue,
                                        {sizeof(weft_render_capabilities), limits::features, limits::target_formats,
                                         limits::max_surface_dimension, limits::max_surface_dimension, 0},
                                        this,
                                        prepare,
                                        release,
                                        encode,
                                        discard,
                                        submitted,
                                        completed,
                                        abandon,
                                        shutdown};
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
        const weft_render_external_backend table{sizeof(table),
                                                 WEFT_RENDER_EXTERNAL_ABI_VERSION,
                                                 (1u << WEFT_RENDER_TARGET_LINEAR_RGBA8) |
                                                     (1u << WEFT_RENDER_TARGET_LINEAR_RGBA16F) |
                                                     (1u << WEFT_RENDER_TARGET_LINEAR_RGBA32F),
                                                 0,
                                                 capacity,
                                                 0,
                                                 bytes,
                                                 this,
                                                 import_texture,
                                                 release_texture,
                                                 prepare_external};
        render_result(weft_renderer_install_external_backend(renderer, &table), "install external texture extension");
    }

    weft_render_result Backend::import_texture(void* ptr, const weft_render_external_texture* d,
                                               uint64_t* out) noexcept {
        auto& self = *static_cast<Backend*>(ptr);
        *out = 0;
        if (d->native_texture_size < sizeof(weft_render_d3d11_texture)) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        const auto& p = *static_cast<const weft_render_d3d11_texture*>(d->native_texture);
        if (p.struct_size < sizeof(p) || p.abi_version != WEFT_RENDER_EXTERNAL_ABI_VERSION || p.reserved ||
            !p.shader_resource_view) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        auto* srv = static_cast<ID3D11ShaderResourceView*>(p.shader_resource_view);
        ComPtr<ID3D11Device> device;
        srv->GetDevice(&device);
        if (device.Get() != self.gpu.device.Get()) {
            return WEFT_RENDER_WRONG_BACKEND;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        srv->GetDesc(&view);
        const auto format = d->format == WEFT_RENDER_TARGET_LINEAR_RGBA8     ? DXGI_FORMAT_R8G8B8A8_UNORM
                            : d->format == WEFT_RENDER_TARGET_LINEAR_RGBA16F ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                                                             : DXGI_FORMAT_R32G32B32A32_FLOAT;
        if (view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || view.Format != format ||
            view.Texture2D.MostDetailedMip || view.Texture2D.MipLevels != 1) {
            return WEFT_RENDER_UNSUPPORTED_FEATURE;
        }
        ComPtr<ID3D11Resource> resource;
        srv->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(resource.As(&texture))) {
            return WEFT_RENDER_UNSUPPORTED_FEATURE;
        }
        D3D11_TEXTURE2D_DESC actual{};
        texture->GetDesc(&actual);
        if (actual.Width != d->width || actual.Height != d->height) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        if (actual.ArraySize != 1 || actual.MipLevels != 1 || actual.SampleDesc.Count != 1 || actual.MiscFlags ||
            actual.CPUAccessFlags || (actual.Usage != D3D11_USAGE_DEFAULT && actual.Usage != D3D11_USAGE_IMMUTABLE)) {
            return WEFT_RENDER_UNSUPPORTED_FEATURE;
        }
        for (const auto& t : self.imports) {
            if (t.resource.Get() == resource.Get()) {
                return WEFT_RENDER_INVALID_STATE;
            }
        }
        for (size_t i = 0; i < self.imports.size(); ++i) {
            if (!self.imports[i].srv) {
                self.imports[i] = {srv, resource};
                *out = i + 1;
                ++self.imported_count;
                return WEFT_RENDER_OK;
            }
        }
        return WEFT_RENDER_BUSY;
    }

    void Backend::release_texture(void* ptr, uint64_t native) noexcept {
        auto& self = *static_cast<Backend*>(ptr);
        if (!native || native > self.imports.size() || !self.imports[native - 1].srv) {
            std::terminate();
        }
        self.imports[native - 1] = {};
        ++self.released_imports;
    }

    weft_render_result Backend::prepare_external(void* ptr, const weft_render_frame* f,
                                                 const weft_render_external_asset* textures, uint32_t count,
                                                 uint64_t* out) noexcept {
        auto& self = *static_cast<Backend*>(ptr);
        *out = 0;
        try {
            std::vector<ExternalGpuAsset> external;
            external.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                const auto id = textures[i].native_texture;
                if (!id || id > self.imports.size() || !self.imports[id - 1].srv) {
                    return WEFT_RENDER_STALE_HANDLE;
                }
                external.push_back(
                    {self.imports[id - 1].srv, textures[i].description.origin == WEFT_RENDER_EXTERNAL_BOTTOM_LEFT});
            }
            const auto result = prepare(ptr, f, out);
            if (result == WEFT_RENDER_OK) {
                self.prepared->external = std::move(external);
            }
            return result;
        } catch (...) {
            return WEFT_RENDER_OUT_OF_MEMORY;
        }
    }

    weft_render_result Backend::prepare(void* ptr, const weft_render_frame* input, uint64_t* out) noexcept {
        auto& self = *static_cast<Backend*>(ptr);
        *out = 0;
        const auto state = self.gpu.tracker.Stats();
        if (self.prepared || state.encoded_tokens || (state.submitted_tokens && !self.gpu.pipeline_enabled)) {
            return WEFT_RENDER_BUSY;
        }
        self.gpu.assets.clear();
        try {
            FramePacket packet;
            packet.assets.reserve(input->asset_count);
            packet.draws.reserve(input->draw_count);
            std::vector<std::pair<std::uint64_t, std::uint64_t>> keys;
            keys.reserve(input->asset_count);
            for (uint32_t i = 0; i < input->asset_count; ++i) {
                const auto& a = input->assets[i];
                auto asset = std::make_shared<Asset>(
                    Asset{a.width, a.height, static_cast<Encoding>(a.encoding), {}, a.sdf_spread});
                const auto key = std::pair{a.asset_id, a.asset_version};
                keys.push_back(key);
                if (!self.gpu.cache_assets || !self.gpu.asset_cache.contains(key)) {
                    asset->bytes.assign(a.bytes, a.bytes + static_cast<size_t>(a.byte_count));
                }
                packet.assets.push_back(std::move(asset));
            }
            for (uint32_t i = 0; i < input->draw_count; ++i) {
                const auto& a = input->draws[i];
                Draw d;
                d.rect = {a.x, a.y, a.width, a.height};
                std::copy_n(a.srgb, 4, d.srgb.begin());
                d.asset = a.asset_index == WEFT_RENDER_SOLID_ASSET ? -1 : int(a.asset_index);
                d.opacity = a.opacity;
                std::copy_n(a.uv, 4, d.uv.begin());
                d.stroke = a.sdf_stroke;
                std::copy_n(a.outline_srgb, 4, d.outline.begin());
                packet.draws.push_back(d);
            }
            self.prepared = self.gpu.prepare(std::move(packet), keys);
            *out = limits::prepared_frame_handle;
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
            for (const auto& a : self.gpu.assets) {
                if (self.gpu.tracker.ReleaseResource(a.id) != LifetimeResult::Ok) {
                    std::terminate();
                }
            }
            self.gpu.collect();
            self.gpu.assets.clear();
            return WEFT_RENDER_BACKEND_ERROR;
        }
    }

    void Backend::release(void* ptr, uint64_t native) noexcept {
        auto& self = *static_cast<Backend*>(ptr);
        if (native != limits::prepared_frame_handle || !self.prepared ||
            self.gpu.tracker.ReleasePreparedFrame(self.prepared->id) != LifetimeResult::Ok) {
            std::terminate();
        }
        self.prepared.reset();
        for (const auto& a : self.gpu.assets) {
            if (self.gpu.tracker.ReleaseResource(a.id) != LifetimeResult::Ok) {
                std::terminate();
            }
        }
        self.gpu.collect();
    }

    weft_render_result Backend::encode(void* ptr, uint64_t native, const weft_render_pass* pass,
                                       uint64_t* out) noexcept {
        auto& self = *static_cast<Backend*>(ptr);
        *out = 0;
        if (native != limits::prepared_frame_handle || !self.prepared ||
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
        if (!p.deferred_context || !p.render_target_view || pass->width != self.gpu.width ||
            pass->height != self.gpu.height) {
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
            if (context_device.Get() != self.gpu.device.Get() || target_device.Get() != self.gpu.device.Get()) {
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
            for (const auto& external : self.prepared->external) {
                ComPtr<ID3D11Resource> source;
                external.srv->GetResource(&source);
                if (source.Get() == resource.Get()) {
                    return WEFT_RENDER_INVALID_STATE;
                }
            }
            for (size_t i = 0; i < self.uses.size(); ++i) {
                if (!self.uses[i]) {
                    auto encoded = self.gpu.encode(*self.prepared, false, self.inject_recording_failure, context,
                                                   target, p.clear_target != 0, p.clear_linear_premultiplied);
                    self.uses[i] = encoded.token;
                    *out = i + 1;
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

    void Backend::discard(void* ptr, uint64_t native) noexcept {
        auto& self = *static_cast<Backend*>(ptr);
        auto& use = self.uses[native - 1];
        if (!use || self.gpu.tracker.DiscardEncoded(*use) != LifetimeResult::Ok) {
            std::terminate();
        }
        use.reset();
        self.gpu.collect();
    }

    void Backend::submitted(void* ptr, uint64_t native, uint64_t serial) noexcept {
        auto& self = *static_cast<Backend*>(ptr);
        auto& use = self.uses[native - 1];
        if (!use || self.gpu.tracker.NotifySubmitted(*use, limits::immediate_queue, serial) != LifetimeResult::Ok) {
            std::terminate();
        }
    }

    void Backend::completed(void* ptr, uint64_t serial) noexcept {
        auto& self = *static_cast<Backend*>(ptr);
        if (self.gpu.tracker.NotifyCompletedSerial(limits::device_epoch, limits::immediate_queue, serial) !=
            LifetimeResult::Ok) {
            std::terminate();
        }
        for (auto& use : self.uses) {
            if (use) {
                UseTokenInfo info;
                if (self.gpu.tracker.InspectUseToken(*use, info) == LifetimeResult::StaleHandle) {
                    use.reset();
                }
            }
        }
        self.gpu.collect();
    }

    weft_render_result Backend::shutdown(void* ptr) noexcept {
        try {
            static_cast<Backend*>(ptr)->gpu.evict_assets(0, true);
        } catch (...) {
            return WEFT_RENDER_BACKEND_ERROR;
        }
        const auto result = static_cast<Backend*>(ptr)->gpu.tracker.Shutdown();
        return result == LifetimeResult::Ok     ? WEFT_RENDER_OK
               : result == LifetimeResult::Busy ? WEFT_RENDER_BUSY
                                                : WEFT_RENDER_BACKEND_ERROR;
    }

    void Backend::abandon(void* ptr) noexcept {
        auto& self = *static_cast<Backend*>(ptr);
        if (self.gpu.tracker.AbandonDeviceEpoch(
                limits::device_epoch, DeviceAbandonConfirmation::HostConfirmedNoFurtherAccess) != LifetimeResult::Ok) {
            std::terminate();
        }
        // Host proof replaces an unattainable completion query on this device.
        self.gpu.acknowledged = true;
        for (auto& use : self.uses) {
            use.reset();
        }
        self.gpu.collect();
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
} // namespace weft::render::d3d11
