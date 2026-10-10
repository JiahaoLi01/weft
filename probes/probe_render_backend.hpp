// Private D3D11 adapter for the actual probe device. Core C ABI has no COM or
// D3D11 dependency. The host creates device/context/RTV and owns submissions.
#include <optional>
#include "probe_constants.hpp"

void render_result(ce_render_result result, const char* operation) {
    if (result != CE_RENDER_OK) {
        throw std::runtime_error(std::string(operation) + " result=" + std::to_string(result));
    }
}

class ProbeBackend {
public:
    Experiment& gpu;
    ce_renderer* renderer{};
    std::optional<PreparedFrame> prepared;
    std::array<std::optional<UseTokenId>, caseelephant::probe_constants::frames_in_flight> uses;
    bool inject_recording_failure{};

    struct ImportedTexture {
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11Resource> resource;
    };

    std::array<ImportedTexture, caseelephant::probe_constants::max_imported_textures> imports;
    unsigned imported_count{};
    unsigned released_imports{};

    void enable_external(unsigned capacity = caseelephant::probe_constants::external_texture_capacity,
                         std::uint64_t bytes = caseelephant::probe_constants::external_texture_max_bytes) {
        const ce_render_external_backend table{sizeof(table),
                                               CE_RENDER_EXTERNAL_ABI_VERSION,
                                               (1u << CE_RENDER_TARGET_LINEAR_RGBA8) |
                                                   (1u << CE_RENDER_TARGET_LINEAR_RGBA16F) |
                                                   (1u << CE_RENDER_TARGET_LINEAR_RGBA32F),
                                               0,
                                               capacity,
                                               0,
                                               bytes,
                                               this,
                                               import_texture,
                                               release_texture,
                                               prepare_external};
        render_result(ce_renderer_install_external_backend(renderer, &table), "install external texture extension");
    }

    static ce_render_result import_texture(void* ptr, const ce_render_external_texture* d, uint64_t* out) noexcept {
        auto& self = *static_cast<ProbeBackend*>(ptr);
        *out = 0;
        if (d->native_texture_size < sizeof(ce_render_d3d11_texture)) {
            return CE_RENDER_INVALID_ARGUMENT;
        }
        const auto& p = *static_cast<const ce_render_d3d11_texture*>(d->native_texture);
        if (p.struct_size < sizeof(p) || p.abi_version != CE_RENDER_EXTERNAL_ABI_VERSION || p.reserved ||
            !p.shader_resource_view) {
            return CE_RENDER_INVALID_ARGUMENT;
        }
        auto* srv = static_cast<ID3D11ShaderResourceView*>(p.shader_resource_view);
        ComPtr<ID3D11Device> device;
        srv->GetDevice(&device);
        if (device.Get() != self.gpu.device.Get()) {
            return CE_RENDER_WRONG_BACKEND;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        srv->GetDesc(&view);
        const auto format = d->format == CE_RENDER_TARGET_LINEAR_RGBA8     ? DXGI_FORMAT_R8G8B8A8_UNORM
                            : d->format == CE_RENDER_TARGET_LINEAR_RGBA16F ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                                                           : DXGI_FORMAT_R32G32B32A32_FLOAT;
        if (view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || view.Format != format ||
            view.Texture2D.MostDetailedMip || view.Texture2D.MipLevels != 1) {
            return CE_RENDER_UNSUPPORTED_FEATURE;
        }
        ComPtr<ID3D11Resource> resource;
        srv->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(resource.As(&texture))) {
            return CE_RENDER_UNSUPPORTED_FEATURE;
        }
        D3D11_TEXTURE2D_DESC actual{};
        texture->GetDesc(&actual);
        if (actual.Width != d->width || actual.Height != d->height) {
            return CE_RENDER_INVALID_ARGUMENT;
        }
        if (actual.ArraySize != 1 || actual.MipLevels != 1 || actual.SampleDesc.Count != 1 || actual.MiscFlags ||
            actual.CPUAccessFlags || (actual.Usage != D3D11_USAGE_DEFAULT && actual.Usage != D3D11_USAGE_IMMUTABLE)) {
            return CE_RENDER_UNSUPPORTED_FEATURE;
        }
        for (const auto& t : self.imports) {
            if (t.resource.Get() == resource.Get()) {
                return CE_RENDER_INVALID_STATE;
            }
        }
        for (size_t i = 0; i < self.imports.size(); ++i) {
            if (!self.imports[i].srv) {
                self.imports[i] = {srv, resource};
                *out = i + 1;
                ++self.imported_count;
                return CE_RENDER_OK;
            }
        }
        return CE_RENDER_BUSY;
    }

    static void release_texture(void* ptr, uint64_t native) noexcept {
        auto& self = *static_cast<ProbeBackend*>(ptr);
        if (!native || native > self.imports.size() || !self.imports[native - 1].srv) {
            std::terminate();
        }
        self.imports[native - 1] = {};
        ++self.released_imports;
    }

    static ce_render_result prepare_external(void* ptr, const ce_render_frame* f,
                                             const ce_render_external_asset* textures, uint32_t count,
                                             uint64_t* out) noexcept {
        auto& self = *static_cast<ProbeBackend*>(ptr);
        *out = 0;
        try {
            std::vector<ExternalGpuAsset> external;
            external.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                const auto id = textures[i].native_texture;
                if (!id || id > self.imports.size() || !self.imports[id - 1].srv) {
                    return CE_RENDER_STALE_HANDLE;
                }
                external.push_back(
                    {self.imports[id - 1].srv, textures[i].description.origin == CE_RENDER_EXTERNAL_BOTTOM_LEFT});
            }
            const auto result = prepare(ptr, f, out);
            if (result == CE_RENDER_OK) {
                self.prepared->external = std::move(external);
            }
            return result;
        } catch (...) {
            return CE_RENDER_OUT_OF_MEMORY;
        }
    }

    explicit ProbeBackend(Experiment& device) : gpu(device) {
        const ce_render_backend table{sizeof(ce_render_backend),
                                      CE_RENDER_ABI_VERSION,
                                      CE_RENDER_API_D3D11,
                                      0,
                                      caseelephant::probe_constants::device_epoch,
                                      caseelephant::probe_constants::immediate_queue,
                                      {sizeof(ce_render_capabilities), caseelephant::probe_constants::backend_features,
                                       caseelephant::probe_constants::backend_target_formats,
                                       caseelephant::probe_constants::max_surface_dimension,
                                       caseelephant::probe_constants::max_surface_dimension, 0},
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
        const ce_renderer_desc limits{sizeof(ce_renderer_desc),
                                      CE_RENDER_ABI_VERSION,
                                      caseelephant::probe_constants::backend_max_prepared_frames,
                                      caseelephant::probe_constants::frames_in_flight,
                                      caseelephant::probe_constants::frames_in_flight,
                                      caseelephant::constants::render::default_max_draws,
                                      caseelephant::probe_constants::max_frame_assets,
                                      0,
                                      caseelephant::probe_constants::backend_max_frame_asset_bytes};
        render_result(ce_renderer_create(&table, &limits, &renderer), "create D3D11 backend interface");
    }

    ~ProbeBackend() {
        if (ce_renderer_destroy(renderer) != CE_RENDER_OK) {
            std::terminate();
        }
    }

    static ce_render_result prepare(void* ptr, const ce_render_frame* input, uint64_t* out) noexcept {
        auto& self = *static_cast<ProbeBackend*>(ptr);
        *out = 0;
        const auto state = self.gpu.tracker.Stats();
        if (self.prepared || state.encoded_tokens || (state.submitted_tokens && !self.gpu.pipeline_enabled)) {
            return CE_RENDER_BUSY;
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
                d.asset = a.asset_index == CE_RENDER_SOLID_ASSET ? -1 : int(a.asset_index);
                d.opacity = a.opacity;
                std::copy_n(a.uv, 4, d.uv.begin());
                d.stroke = a.sdf_stroke;
                std::copy_n(a.outline_srgb, 4, d.outline.begin());
                packet.draws.push_back(d);
            }
            self.prepared = self.gpu.prepare(std::move(packet), keys);
            *out = caseelephant::probe_constants::backend_prepared_frame_handle;
            return CE_RENDER_OK;
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
            return CE_RENDER_BACKEND_ERROR;
        }
    }

    static void release(void* ptr, uint64_t native) noexcept {
        auto& self = *static_cast<ProbeBackend*>(ptr);
        if (native != caseelephant::probe_constants::backend_prepared_frame_handle || !self.prepared ||
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

    static ce_render_result encode(void* ptr, uint64_t native, const ce_render_pass* pass, uint64_t* out) noexcept {
        auto& self = *static_cast<ProbeBackend*>(ptr);
        *out = 0;
        if (native != caseelephant::probe_constants::backend_prepared_frame_handle || !self.prepared ||
            pass->native_pass_size < sizeof(ce_render_d3d11_pass)) {
            return CE_RENDER_INVALID_ARGUMENT;
        }
        const auto& p = *static_cast<const ce_render_d3d11_pass*>(pass->native_pass);
        if (p.struct_size < sizeof(p) || p.abi_version != CE_RENDER_ABI_VERSION || !p.out_command_list) {
            return CE_RENDER_INVALID_ARGUMENT;
        }
        *p.out_command_list = nullptr;
        if (p.clear_target > 1 || p.reserved) {
            return CE_RENDER_INVALID_ARGUMENT;
        }
        if (p.clear_target) {
            for (float c : p.clear_linear_premultiplied) {
                if (!std::isfinite(c) || c < 0 || c > 1) {
                    return CE_RENDER_INVALID_ARGUMENT;
                }
            }
            for (size_t i = 0; i < 3; ++i) {
                if (p.clear_linear_premultiplied[i] > p.clear_linear_premultiplied[3]) {
                    return CE_RENDER_INVALID_ARGUMENT;
                }
            }
        }
        if (!p.deferred_context || !p.render_target_view || pass->width != self.gpu.width ||
            pass->height != self.gpu.height) {
            return CE_RENDER_UNSUPPORTED_FEATURE;
        }
        try {
            auto* context = static_cast<ID3D11DeviceContext*>(p.deferred_context);
            auto* target = static_cast<ID3D11RenderTargetView*>(p.render_target_view);
            if (context->GetType() != D3D11_DEVICE_CONTEXT_DEFERRED) {
                return CE_RENDER_INVALID_ARGUMENT;
            }
            ComPtr<ID3D11Device> context_device;
            ComPtr<ID3D11Device> target_device;
            context->GetDevice(&context_device);
            target->GetDevice(&target_device);
            if (context_device.Get() != self.gpu.device.Get() || target_device.Get() != self.gpu.device.Get()) {
                return CE_RENDER_WRONG_BACKEND;
            }
            D3D11_RENDER_TARGET_VIEW_DESC view{};
            target->GetDesc(&view);
            const auto expected = pass->target_format == CE_RENDER_TARGET_SRGB_RGBA8 ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                  : pass->target_format == CE_RENDER_TARGET_LINEAR_RGBA8
                                      ? DXGI_FORMAT_R8G8B8A8_UNORM
                                      : DXGI_FORMAT_R32G32B32A32_FLOAT;
            if (view.Format != expected || view.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D ||
                view.Texture2D.MipSlice) {
                return CE_RENDER_UNSUPPORTED_FEATURE;
            }
            ComPtr<ID3D11Resource> resource;
            target->GetResource(&resource);
            ComPtr<ID3D11Texture2D> texture;
            checked(resource.As(&texture), "backend target texture");
            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);
            if (description.Width != pass->width || description.Height != pass->height ||
                description.SampleDesc.Count != 1) {
                return CE_RENDER_INVALID_ARGUMENT;
            }
            for (const auto& external : self.prepared->external) {
                ComPtr<ID3D11Resource> source;
                external.srv->GetResource(&source);
                if (source.Get() == resource.Get()) {
                    return CE_RENDER_INVALID_STATE;
                }
            }
            for (size_t i = 0; i < self.uses.size(); ++i) {
                if (!self.uses[i]) {
                    auto encoded = self.gpu.encode(*self.prepared, false, self.inject_recording_failure, context,
                                                   target, p.clear_target != 0, p.clear_linear_premultiplied);
                    self.uses[i] = encoded.token;
                    *out = i + 1;
                    *p.out_command_list = encoded.commands.Detach();
                    return CE_RENDER_OK;
                }
            }
            return CE_RENDER_BUSY;
        } catch (...) {
            // Experiment::encode closes/discards its partial list before
            // releasing pins; no executable partial commands escape this ABI.
            return CE_RENDER_BACKEND_ERROR;
        }
    }

    static void discard(void* ptr, uint64_t native) noexcept {
        auto& self = *static_cast<ProbeBackend*>(ptr);
        auto& use = self.uses[native - 1];
        if (!use || self.gpu.tracker.DiscardEncoded(*use) != LifetimeResult::Ok) {
            std::terminate();
        }
        use.reset();
        self.gpu.collect();
    }

    static void submitted(void* ptr, uint64_t native, uint64_t serial) noexcept {
        auto& self = *static_cast<ProbeBackend*>(ptr);
        auto& use = self.uses[native - 1];
        if (!use || self.gpu.tracker.NotifySubmitted(*use, caseelephant::probe_constants::immediate_queue, serial) !=
                        LifetimeResult::Ok) {
            std::terminate();
        }
    }

    static void completed(void* ptr, uint64_t serial) noexcept {
        auto& self = *static_cast<ProbeBackend*>(ptr);
        if (self.gpu.tracker.NotifyCompletedSerial(caseelephant::probe_constants::device_epoch,
                                                   caseelephant::probe_constants::immediate_queue,
                                                   serial) != LifetimeResult::Ok) {
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

    static ce_render_result shutdown(void* ptr) noexcept {
        try {
            static_cast<ProbeBackend*>(ptr)->gpu.evict_assets(0, true);
        } catch (...) {
            return CE_RENDER_BACKEND_ERROR;
        }
        const auto result = static_cast<ProbeBackend*>(ptr)->gpu.tracker.Shutdown();
        return result == LifetimeResult::Ok     ? CE_RENDER_OK
               : result == LifetimeResult::Busy ? CE_RENDER_BUSY
                                                : CE_RENDER_BACKEND_ERROR;
    }

    static void abandon(void* ptr) noexcept {
        auto& self = *static_cast<ProbeBackend*>(ptr);
        if (self.gpu.tracker.AbandonDeviceEpoch(caseelephant::probe_constants::device_epoch,
                                                DeviceAbandonConfirmation::HostConfirmedNoFurtherAccess) !=
            LifetimeResult::Ok) {
            std::terminate();
        }
        // Host proof replaces an unattainable completion query on this device.
        self.gpu.acknowledged = true;
        for (auto& use : self.uses) {
            use.reset();
        }
        self.gpu.collect();
    }

    ce_render_pass pass(ID3D11DeviceContext* context, ID3D11RenderTargetView* target, void** commands,
                        ce_render_d3d11_pass& payload) {
        ce_render_info info{};
        info.struct_size = sizeof(info);
        render_result(ce_renderer_get_info(renderer, &info), "backend info");
        payload = {sizeof(payload), CE_RENDER_ABI_VERSION, context, target, commands, 1, 0, {0, 0, 0, 0}};
        return {sizeof(ce_render_pass),
                CE_RENDER_ABI_VERSION,
                info.identity,
                gpu.width,
                gpu.height,
                CE_RENDER_TARGET_SRGB_RGBA8,
                1,
                &payload,
                sizeof(payload)};
    }
};

struct PacketView {
    std::vector<ce_render_asset> assets;
    std::vector<ce_render_draw> draws;
    ce_render_frame frame{};

    PacketView(const FramePacket& packet, uint64_t version, Experiment* cache = nullptr) {
        for (size_t i = 0; i < packet.assets.size(); ++i) {
            const auto& a = *packet.assets[i];
            assets.push_back({cache ? cache->asset_identity(packet.assets[i]) : i + 1, cache ? 1 : version, a.width,
                              a.height, static_cast<uint32_t>(a.encoding), 0, a.bytes.data(), a.bytes.size(), a.spread,
                              0});
        }
        for (const auto& d : packet.draws) {
            require(!d.group && !d.clipped, "backend v1 supports ordered quads, no diagnostic group/clip flags");
            ce_render_draw draw{
                d.rect.x,  d.rect.y, d.rect.w, d.rect.h, {}, d.asset < 0 ? CE_RENDER_SOLID_ASSET : uint32_t(d.asset),
                d.opacity, {},       d.stroke, {}};
            std::copy(d.srgb.begin(), d.srgb.end(), draw.srgb);
            std::copy(d.uv.begin(), d.uv.end(), draw.uv);
            std::copy(d.outline.begin(), d.outline.end(), draw.outline_srgb);
            draws.push_back(draw);
        }
        frame = {sizeof(frame),          CE_RENDER_ABI_VERSION,   draws.data(),
                 uint32_t(draws.size()), uint32_t(assets.size()), assets.data()};
    }
};

ProbeBackend& Experiment::ensure_backend() {
    if (!backend) {
        backend = new ProbeBackend(*this);
    }
    return *backend;
}

void Experiment::complete_backend() {
    render_result(ce_renderer_notify_completed(backend->renderer, caseelephant::probe_constants::device_epoch,
                                               caseelephant::probe_constants::immediate_queue, pending_completion),
                  "backend actual completion");
}

Experiment::~Experiment() {
    if (submitted && !acknowledged) {
        try {
            complete();
        } catch (...) {
            std::terminate();
        }
    }
    delete backend;
}

template <class Present>
void Experiment::render_window(FramePacket packet, Present&& present) {
    require(!pipeline_enabled, "synchronous and asynchronous window hosts cannot be mixed");
    require(!submitted || acknowledged, "previous window frame must complete before reuse");
    auto& api = ensure_backend();
    const auto count = packet.assets.size();
    PacketView view(packet, window_serial + 1, cache_assets ? this : nullptr);
    ce_render_handle frame{};
    ce_render_handle use{};
    const auto started = std::chrono::steady_clock::now();
    auto milliseconds = [](auto a, auto b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    render_result(ce_renderer_prepare(api.renderer, &view.frame, &frame), "backend prepare window");
    const auto prepared_at = std::chrono::steady_clock::now();
    prepare_ms = milliseconds(started, prepared_at);
    void* native_commands{};
    ce_render_d3d11_pass native{};
    auto pass = api.pass(deferred.Get(), output.rtv.Get(), &native_commands, native);
    const auto result = ce_renderer_encode(api.renderer, frame, &pass, &use);
    render_result(ce_renderer_release_prepared(api.renderer, frame), "backend release CPU/prepared window");
    render_result(result, "backend encode window");
    ComPtr<ID3D11CommandList> commands;
    commands.Attach(static_cast<ID3D11CommandList*>(native_commands));
    const auto encoded_at = std::chrono::steady_clock::now();
    encode_ms = milliseconds(prepared_at, encoded_at);
    // Source packet can be freed now; backend-use pins protect actual GPU data.
    packet = {};
    pending_completion = window_serial + caseelephant::probe_constants::serials_per_ui_frame;
    acknowledged = false;
    if (timing_disjoint) {
        immediate->Begin(timing_disjoint.Get());
        immediate->End(timing_start.Get());
    }
    immediate->ExecuteCommandList(commands.Get(), TRUE);
    if (timing_disjoint) {
        immediate->End(timing_end.Get());
        immediate->End(timing_disjoint.Get());
    }
    immediate->End(ui_done.Get());
    submitted = true;
    {
        struct Marker {
            Experiment& r;

            ~Marker() {
                r.immediate->End(r.host_done.Get());
            }
        } marker{*this};

        render_result(ce_renderer_notify_submitted(api.renderer, use, caseelephant::probe_constants::immediate_queue,
                                                   window_serial + 1),
                      "backend actual submission");
        commands.Reset();
        present();
    }
    const auto presented_at = std::chrono::steady_clock::now();
    present_ms = milliseconds(encoded_at, presented_at);
    complete();
    wait_ms = milliseconds(presented_at, std::chrono::steady_clock::now());
    window_serial += caseelephant::probe_constants::serials_per_ui_frame;
    if (timing_disjoint) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT data{};
        UINT64 start{};
        UINT64 end{};
        require(immediate->GetData(timing_disjoint.Get(), &data, sizeof(data), D3D11_ASYNC_GETDATA_DONOTFLUSH) ==
                        S_OK &&
                    !data.Disjoint && data.Frequency,
                "stable GPU timing frequency");
        require(immediate->GetData(timing_start.Get(), &start, sizeof(start), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                    immediate->GetData(timing_end.Get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                    end >= start,
                "completed GPU timestamp interval");
        gpu_ui_ms = double(end - start) * caseelephant::constants::milliseconds_per_second / data.Frequency;
    }
    require(cache_assets || count <= native_releases, "window glyph textures retired after actual completion");
    immediate->ClearState();
}

// The completed offscreen result is persistent. Host-only composition advances
// the same actual completion serial without inventing a new DOM/UI packet.
template <class Present>
void Experiment::compose_window(Present&& present) {
    require(!pipeline_enabled, "synchronous and asynchronous composition cannot be mixed");
    require(backend && submitted && acknowledged, "composition requires completed UI output");
    pending_completion = window_serial + 1;
    acknowledged = false;
    immediate->End(ui_done.Get());
    {
        struct Marker {
            Experiment& r;

            ~Marker() {
                r.immediate->End(r.host_done.Get());
            }
        } marker{*this};

        present();
    }
    complete();
    ++window_serial;
    immediate->ClearState();
}
