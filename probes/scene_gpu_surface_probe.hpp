#pragma once
#include "gallery_scene_manifest.hpp"
#include "gallery_constants.hpp"

// Explicit host-owned card surfaces. Three bounded versions allow two UI
// submissions in flight. A version is never rewritten before its real query
// completion and external caller lease retirement. No DOM or VM crosses here.
class SceneGpuSurfaces {
public:
    struct Slot {
        ComPtr<ID3D11Texture2D> encoded;
        ComPtr<ID3D11RenderTargetView> encoded_rtv;
        ComPtr<ID3D11ShaderResourceView> encoded_srv;
        Surface linear;
        ComPtr<ID2D1DeviceContext> target;
        ComPtr<ID2D1Bitmap1> bitmap;
        caseelephant::SceneRasterProbe painter;
        ce_render_handle handle{};
        std::uint64_t last_serial{};
        std::uint64_t version{};
        unsigned width{};
        unsigned height{};
        std::optional<std::array<float, 4>> clip;
        std::shared_ptr<const caseelephant::ProbeSceneFrame> retained_frame;
        SceneNativeGpu::Slot native;
        bool last_native{};
    };

private:
    Experiment& gpu_;
    TwoFramePipeline& pipeline_;
    ProbeBackend& api_;
    SceneNativeGpu native_;
    ComPtr<ID2D1Device> d2d_;
    ComPtr<ID3D11Buffer> constants_;
    std::array<std::array<std::unique_ptr<Slot>, caseelephant::probe_constants::gallery::surface_versions_per_card>,
               gallery_scene_count>
        slots_;
    std::array<Slot*, gallery_scene_count> current_{};
    caseelephant::SceneRasterProbe::Work retired_raster_{};
    std::uint64_t version_{};
    bool producer_pending{};
    std::vector<std::pair<unsigned, Slot*>> pending_;

    static void add_work(caseelephant::SceneRasterProbe::Work& total,
                         const caseelephant::SceneRasterProbe::Work& value) {
        total.gpu_projective_draws += value.gpu_projective_draws;
        total.gpu_plane_uploads += value.gpu_plane_uploads;
        total.uniform_border_draws += value.uniform_border_draws;
        total.fallback_border_draws += value.fallback_border_draws;
        total.border_geometry_builds += value.border_geometry_builds;
        total.border_geometry_reuses += value.border_geometry_reuses;
        total.paint_program_builds += value.paint_program_builds;
        total.paint_program_reuses += value.paint_program_reuses;
        total.paint_program_evictions += value.paint_program_evictions;
        total.typed_draws += value.typed_draws;
        total.layer_builds += value.layer_builds;
        total.layer_reuses += value.layer_reuses;
        total.layer_fallbacks += value.layer_fallbacks;
        total.layer_evictions += value.layer_evictions;
        total.partial_paints += value.partial_paints;
    }

    void create(Slot& s, unsigned w, unsigned h) {
        s.width = w;
        s.height = h;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        check_hresult(gpu_.device->CreateTexture2D(&desc, nullptr, &s.encoded), "GPU scene encoded target");
        check_hresult(gpu_.device->CreateRenderTargetView(s.encoded.Get(), nullptr, &s.encoded_rtv),
                      "GPU scene encoded RTV");
        check_hresult(gpu_.device->CreateShaderResourceView(s.encoded.Get(), nullptr, &s.encoded_srv),
                      "GPU scene encoded view");
        check_hresult(d2d_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &s.target), "GPU scene D2D context");
        ComPtr<IDXGISurface> surface;
        check_hresult(s.encoded.As(&surface), "GPU scene DXGI surface");
        auto properties =
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        check_hresult(s.target->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &s.bitmap),
                      "GPU scene D2D bitmap");
        s.target->SetTarget(s.bitmap.Get());
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        check_hresult(gpu_.device->CreateTexture2D(&desc, nullptr, &s.linear.texture), "GPU scene linear target");
        check_hresult(gpu_.device->CreateRenderTargetView(s.linear.texture.Get(), nullptr, &s.linear.rtv),
                      "GPU scene linear RTV");
        check_hresult(gpu_.device->CreateShaderResourceView(s.linear.texture.Get(), nullptr, &s.linear.srv),
                      "GPU scene linear SRV");
    }

    void convert(Slot& s) {
        auto* ctx = gpu_.immediate.Get();
        ctx->ClearState();
        auto* rtv = s.linear.rtv.Get();
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT viewport{0, 0, float(s.width), float(s.height), 0, 1};
        ctx->RSSetViewports(1, &viewport);
        D3D11_RECT clip{0, 0, LONG(s.width), LONG(s.height)};
        ctx->RSSetScissorRects(1, &clip);
        ctx->RSSetState(gpu_.raster.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(gpu_.vs.Get(), nullptr, 0);
        ctx->PSSetShader(gpu_.ps.Get(), nullptr, 0);
        Constants data{{0, 0, float(s.width), float(s.height)},
                       {float(s.width), float(s.height)},
                       caseelephant::probe_constants::gallery::encoded_bgra_material,
                       1,
                       {1, 1, 1, 1},
                       {0, 0, 1, 1},
                       {},
                       {}};
        ctx->UpdateSubresource(constants_.Get(), 0, nullptr, &data, 0, 0);
        auto* buffer = constants_.Get();
        ctx->VSSetConstantBuffers(0, 1, &buffer);
        ctx->PSSetConstantBuffers(0, 1, &buffer);
        auto* sampler = gpu_.sampler.Get();
        ctx->PSSetSamplers(0, 1, &sampler);
        auto* view = s.encoded_srv.Get();
        ctx->PSSetShaderResources(0, 1, &view);
        ctx->Draw(caseelephant::probe_constants::gallery::quad_vertex_count, 0);
        ctx->ClearState();
    }

public:
    std::size_t paints{};
    std::size_t reuses{};
    std::size_t allocated{};
    std::size_t retired{};
    std::size_t partial_paints{};
    std::size_t full_paints{};
    std::size_t hidden_retirements{};
    std::size_t peak_live_slots{};
    double damaged_pixels{};
    double full_pixels{};
    std::uint64_t peak_imported_bytes{};
    bool retain_paint{};
    bool native_paint{};

    const SceneNativeGpu& native_work() const {
        return native_;
    }

    SceneGpuSurfaces(Experiment& gpu, TwoFramePipeline& pipeline)
        : gpu_(gpu), pipeline_(pipeline), api_(gpu.ensure_backend()), native_(gpu) {
        api_.enable_external(caseelephant::probe_constants::gallery::external_texture_limit,
                             caseelephant::probe_constants::gallery::external_import_budget_bytes);
        ComPtr<IDXGIDevice> device;
        check_hresult(gpu.device.As(&device), "GPU scene D2D device identity");
        check_hresult(D2D1CreateDevice(device.Get(), nullptr, &d2d_), "GPU scene shared D2D device");
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = sizeof(Constants);
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        check_hresult(gpu.device->CreateBuffer(&desc, nullptr, &constants_), "GPU scene conversion constants");
    }

    Slot* image(unsigned index) const {
        return current_.at(index);
    }

    std::size_t live_slots() const {
        std::size_t count{};
        for (const auto& group : slots_) {
            for (const auto& slot : group) {
                count += bool(slot);
            }
        }
        return count;
    }

    // The host visibility set describes the complete next packet. Hidden
    // surfaces remain owned until their real last submission has completed.
    // This collector neither waits nor guesses completion from visibility.
    template <std::size_t N>
    void retire_hidden(const std::array<bool, N>& visible) {
        static_assert(N <= gallery_scene_count);
        require(!producer_pending && pending_.empty(), "hidden retirement before next producer batch");
        for (unsigned i = 0; i < N; ++i) {
            if (!visible[i]) {
                for (auto& slot : slots_[i]) {
                    if (slot && slot->last_serial <= pipeline_.completed_serial) {
                        if (slot->handle.renderer_id) {
                            render_result(ce_renderer_release_texture(api_.renderer, slot->handle),
                                          "GPU hidden completed caller retirement");
                            slot->handle = {};
                            ++retired;
                        }
                        if (current_[i] == slot.get()) {
                            current_[i] = nullptr;
                        }
                        add_work(retired_raster_, slot->painter.work());
                        native_.retire(slot->native);
                        slot.reset();
                        ++hidden_retirements;
                    }
                }
            }
        }
    }

    caseelephant::SceneRasterProbe::Work raster_work() const {
        auto total = retired_raster_;
        for (const auto& group : slots_) {
            for (const auto& slot : group) {
                if (slot) {
                    add_work(total, slot->painter.work());
                }
            }
        }
        return total;
    }

    bool needs_clip(unsigned index, std::array<float, 4> clip) const {
        return !current_.at(index) || current_[index]->clip != clip;
    }

    Slot& paint(unsigned index, const caseelephant::ProbeSceneFrame& frame,
                float scale = caseelephant::probe_constants::gallery::scene_raster_scale,
                std::optional<std::array<float, 4>> clip = std::nullopt) {
        return paint_owned(index, std::make_shared<const caseelephant::ProbeSceneFrame>(frame), scale, clip);
    }

    Slot& paint_owned(unsigned index, std::shared_ptr<const caseelephant::ProbeSceneFrame> value,
                      float scale = caseelephant::probe_constants::gallery::scene_raster_scale,
                      std::optional<std::array<float, 4>> clip = std::nullopt,
                      std::vector<caseelephant::ProbeNodeIdentity> layers = {}) {
        require(bool(value), "owned scene snapshot missing");
        const auto& frame = *value;
        require(std::isfinite(scale) && scale > 0 && scale <= caseelephant::probe_constants::gallery::max_surface_scale,
                "scene surface scale");
        for (auto v : frame.root.box) {
            require(std::isfinite(v), "scene surface finite bounds");
        }
        require(frame.root.box[2] > 0 && frame.root.box[3] > 0, "scene surface dimensions");
        require(std::none_of(pending_.begin(), pending_.end(),
                             [&](const auto& p) {
                                 return p.first == index;
                             }),
                "one producer version per card per packet");
        Slot* selected{};
        for (auto& candidate : slots_.at(index)) {
            if (candidate && candidate.get() != current_[index] &&
                candidate->last_serial <= pipeline_.completed_serial) {
                selected = candidate.get();
                break;
            }
        }
        if (!selected) {
            for (auto& candidate : slots_[index]) {
                if (!candidate) {
                    candidate = std::make_unique<Slot>();
                    selected = candidate.get();
                    ++allocated;
                    break;
                }
            }
        }
        if (!selected) {
            pipeline_.drain();
            for (auto& candidate : slots_[index]) {
                if (candidate.get() != current_[index]) {
                    selected = candidate.get();
                    break;
                }
            }
        }
        require(selected && selected->last_serial <= pipeline_.completed_serial,
                "GPU scene cannot rewrite an incomplete version");
        auto& s = *selected;
        if (s.handle.renderer_id) {
            render_result(ce_renderer_release_texture(api_.renderer, s.handle),
                          "GPU scene completed caller retirement");
            s.handle = {};
            ++retired;
            ++reuses;
        }
        const auto w = unsigned(std::ceil(frame.root.box[2] * scale));
        const auto h = unsigned(std::ceil(frame.root.box[3] * scale));
        require(w && h && std::uint64_t(w) * h <= caseelephant::probe_constants::gallery::max_surface_pixels,
                "GPU scene surface bound");
        if (!s.encoded || s.width != w || s.height != h) {
            create(s, w, h);
            s.retained_frame.reset();
        }
        peak_live_slots = std::max(peak_live_slots, live_slots());
        producer_pending = true;
        s.painter.set_layers(layers);
        auto damage = caseelephant::ProbeSceneDamage{true, true, {0, 0, frame.root.box[2], frame.root.box[3]}};
        if (retain_paint && s.retained_frame && s.clip == clip) {
            damage = caseelephant::scene_damage_probe(*s.retained_frame, frame, scale);
        }
        full_pixels += double(w) * h;
        const bool native =
            native_paint && native_.paint(s.native, frame, s.encoded_rtv.Get(), scale, w, h, clip, index, layers);
        if (s.last_native && !native) {
            damage.full = true;
        }
        s.last_native = native;
        if (native) {
            ++full_paints;
            damaged_pixels += double(w) * h;
        } else if (damage.full) {
            s.painter.paint_d2d(frame, s.target.Get(), scale, clip);
            ++full_paints;
            damaged_pixels += double(w) * h;
        } else if (damage.changed) {
            s.painter.paint_d2d_damage(frame, s.target.Get(), scale, damage.rect, clip);
            ++partial_paints;
            damaged_pixels +=
                double(damage.rect[2] - damage.rect[0]) * (damage.rect[3] - damage.rect[1]) * scale * scale;
        }
        s.clip = clip;
        s.retained_frame = retain_paint ? std::move(value) : nullptr;
        ++paints;
        pending_.emplace_back(index, selected);
        current_[index] = selected;
        return s;
    }

    void publish() {
        for (auto [index, selected] : pending_) {
            auto& s = *selected;
            convert(s);
            auto w = s.width;
            auto h = s.height;
            ce_render_info info{};
            info.struct_size = sizeof(info);
            render_result(ce_renderer_get_info(api_.renderer, &info), "GPU scene renderer epoch");
            ce_render_d3d11_texture payload{sizeof(payload), 1, s.linear.srv.Get(), 0};
            require(version_ != UINT64_MAX, "GPU scene version generation bound");
            s.version = ++version_;
            ce_render_external_texture description{sizeof(description),
                                                   1,
                                                   info.identity,
                                                   caseelephant::probe_constants::gallery::external_texture_id_base +
                                                       index,
                                                   s.version,
                                                   w,
                                                   h,
                                                   CE_RENDER_TARGET_LINEAR_RGBA16F,
                                                   0,
                                                   0,
                                                   0,
                                                   0,
                                                   0,
                                                   &payload,
                                                   sizeof(payload),
                                                   nullptr,
                                                   nullptr};
            const auto result = ce_renderer_import_texture(api_.renderer, &description, &s.handle);
            ce_render_external_info external{sizeof(external)};
            render_result(ce_renderer_get_external_info(api_.renderer, &external), "GPU scene import accounting");
            peak_imported_bytes = std::max(peak_imported_bytes, external.texture_bytes);
            if (result != CE_RENDER_OK) {
                throw std::runtime_error("GPU scene frozen linear version result=" + std::to_string(result) +
                                         " card=" + std::to_string(index) +
                                         " imported_bytes=" + std::to_string(external.texture_bytes) +
                                         " live_textures=" + std::to_string(external.live_textures));
            }
        }
        pending_.clear();
    }

    // Translate the complete CPU shell packet to complete GPU dependencies.
    // Unused placeholders are removed; original text/image assets keep order.
    template <std::size_t N>
    std::vector<ce_render_handle> dependencies(FramePacket& packet, const std::array<int, N>& indices,
                                               const std::array<bool, N>& visible) {
        std::vector<int> mapping(packet.assets.size(), -1);
        std::vector<ce_render_handle> external;
        for (unsigned i = 0; i < N; ++i) {
            if (visible[i]) {
                require(current_[i] && indices[i] >= 0, "GPU scene visible dependency");
                mapping.at(unsigned(indices[i])) = -2 - int(external.size());
                external.push_back(current_[i]->handle);
            }
        }
        std::vector<std::shared_ptr<const Asset>> cpu;
        for (std::size_t i = 0; i < mapping.size(); ++i) {
            if (mapping[i] == -1) {
                mapping[i] = int(cpu.size());
                cpu.push_back(packet.assets[i]);
            }
        }
        for (auto& d : packet.draws) {
            if (d.asset >= 0) {
                auto index = mapping.at(unsigned(d.asset));
                d.asset = index >= 0 ? index : int(cpu.size()) - index - 2;
            }
        }
        packet.assets = std::move(cpu);
        return external;
    }

    template <std::size_t N>
    void submitted(std::uint64_t serial, const std::array<bool, N>& visible) {
        producer_pending = false;
        for (unsigned i = 0; i < N; ++i) {
            if (visible[i]) {
                current_[i]->last_serial = serial;
            }
        }
    }

    void retire_card(unsigned index) {
        require(!producer_pending, "retire requires published submission");
        pipeline_.drain();
        for (auto& slot : slots_.at(index)) {
            if (slot) {
                require(slot->last_serial <= pipeline_.completed_serial, "card retirement before completion");
                if (slot->handle.renderer_id) {
                    render_result(ce_renderer_release_texture(api_.renderer, slot->handle),
                                  "GPU card final caller retirement");
                    slot->handle = {};
                    ++retired;
                }
                add_work(retired_raster_, slot->painter.work());
                native_.retire(slot->native);
                slot.reset();
            }
        }
        current_.at(index) = nullptr;
    }

    caseelephant::ProbeSceneBitmap read(unsigned index) {
        pipeline_.drain();
        auto& s = *current_.at(index);
        require(s.last_serial <= pipeline_.completed_serial, "GPU scene read requires real completion");
        D3D11_TEXTURE2D_DESC desc{};
        s.encoded->GetDesc(&desc);
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        check_hresult(gpu_.device->CreateTexture2D(&desc, nullptr, &staging), "GPU scene test staging");
        gpu_.immediate->CopyResource(staging.Get(), s.encoded.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check_hresult(gpu_.immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                      "GPU scene test completed readback");
        caseelephant::ProbeSceneBitmap out{s.width, s.height,
                                           std::vector<std::uint8_t>(std::size_t(s.width) * s.height * 4)};
        for (unsigned y = 0; y < s.height; ++y) {
            for (unsigned x = 0; x < s.width; ++x) {
                auto p = static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(y) * mapped.RowPitch + x * 4;
                auto q = out.rgba.data() + (std::size_t(y) * s.width + x) * 4;
                unsigned alpha = p[3];
                q[3] = std::uint8_t(alpha);
                for (unsigned c = 0; c < 3; ++c) {
                    q[c] = alpha ? std::uint8_t(std::min(
                                       static_cast<unsigned>(caseelephant::constants::color_channel_max),
                                       (unsigned(p[2 - c]) * caseelephant::constants::color_channel_max + alpha / 2) /
                                           alpha))
                                 : 0;
                }
            }
        }
        gpu_.immediate->Unmap(staging.Get(), 0);
        return out;
    }

    void release() {
        pipeline_.cancel_frame_timing();
        if (producer_pending) {
            D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
            ComPtr<ID3D11Query> done;
            check_hresult(gpu_.device->CreateQuery(&desc, &done), "GPU producer-only retirement query");
            gpu_.immediate->End(done.Get());
            gpu_.immediate->Flush();
            BOOL complete{};
            const auto deadline =
                std::chrono::steady_clock::now() +
                std::chrono::seconds(caseelephant::probe_constants::gallery::producer_completion_timeout_s);
            while (gpu_.immediate->GetData(done.Get(), &complete, sizeof(complete), 0) != S_OK || !complete) {
                require(std::chrono::steady_clock::now() < deadline, "GPU producer-only completion deadline");
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(caseelephant::probe_constants::gallery::completion_poll_ms));
            }
            producer_pending = false;
        }
        pipeline_.drain();
        for (auto& group : slots_) {
            for (auto& slot : group) {
                if (slot && slot->handle.renderer_id) {
                    render_result(ce_renderer_release_texture(api_.renderer, slot->handle),
                                  "GPU scene final caller retirement");
                    slot->handle = {};
                    ++retired;
                }
            }
        }
        require(api_.imported_count == api_.released_imports, "GPU scene native imports leaked");
    }

    ~SceneGpuSurfaces() {
        try {
            release();
        } catch (...) {
            std::terminate();
        }
    }
};

#include "../tests/scene_visual_compare.hpp"

int run_scene_gpu_test(bool warp, const std::filesystem::path& folder, bool optimized = false) {
    std::filesystem::create_directories(folder);
    Experiment gpu(warp, caseelephant::probe_constants::gallery::scene_test_surface_size,
                   caseelephant::probe_constants::gallery::scene_test_surface_size);
    gpu.cache_assets = true;
    TwoFramePipeline pipeline(gpu);
    SceneGpuSurfaces surfaces(gpu, pipeline);
    surfaces.retain_paint = surfaces.native_paint = optimized;
    std::ostringstream report;
    report << "{\"format\":\"scene-gpu-original-v1\",\"cases\":[";
    unsigned count{};
    unsigned captures{};
    double worst_mean{};
    double minimum_iou = 1;
    for (unsigned group = 0; group < 2; ++group) {
        for (unsigned index = 0;
             index < (group ? static_cast<unsigned>(caseelephant::probe_constants::gallery::d3_scene_count)
                            : static_cast<unsigned>(caseelephant::probe_constants::gallery::css_scene_count));
             ++index) {
            const unsigned slot = group ? caseelephant::probe_constants::gallery::css_scene_count + index : index;
            const auto id = std::string(group ? "d3-" : "css-") + (index < 10 ? "0" : "") + std::to_string(index);
            std::cout << id << " GPU..." << std::flush;
            GallerySceneSession scene(group != 0, index,
                                      group
                                          ? static_cast<float>(caseelephant::probe_constants::gallery::d3_card_size)
                                          : static_cast<float>(caseelephant::probe_constants::gallery::css_card_width));
            std::array<bool, caseelephant::probe_constants::gallery::core_scene_count> visible{};
            visible[slot] = true;
            for (unsigned frame = 0; frame <= caseelephant::probe_constants::gallery::scene_reference_last_frame;
                 ++frame) {
                const double time = frame * caseelephant::probe_constants::gallery::fixed_clock_step_ms;
                scene.step(time);
                if (!group && index >= 4 && index <= 7 && (frame == 18 || frame == 72)) {
                    scene.input(index == 7 ? caseelephant::ProbeMouseType::Click : caseelephant::ProbeMouseType::Move,
                                frame == 72 && index != 7 ? -10.f : scene.width / 2,
                                frame == 72 && index != 7 ? -10.f : 60.f);
                    scene.step(time);
                }
                auto& image =
                    surfaces.paint_owned(slot, std::make_shared<const caseelephant::ProbeSceneFrame>(scene.current),
                                         caseelephant::probe_constants::gallery::scene_raster_scale, std::nullopt,
                                         optimized ? gallery_layer_manifest(slot, scene.current, true)
                                                   : std::vector<caseelephant::ProbeNodeIdentity>{});
                surfaces.publish();
                FramePacket packet;
                packet.draws.push_back({{0, 0, float(image.width), float(image.height)}, {1, 1, 1, 1}, 0});
                const auto serial = pipeline.submit(std::move(packet),
                                                    [] {
                                                    },
                                                    {}, {image.handle});
                surfaces.submitted(serial, visible);
                if (frame == 24 || frame == 63 || frame == 120) {
                    const auto actual = surfaces.read(slot);
                    const auto expected = scene_reference(std::filesystem::path(CE_SOURCE_DIR) /
                                                          "tests/fixtures/scene-gallery-reference" /
                                                          (id + "-" + std::to_string(frame) + ".png"));
                    const auto diff = scene_difference(actual, expected);
                    const auto mean = *std::max_element(diff.mean.begin(), diff.mean.end());
                    worst_mean = std::max(worst_mean, mean);
                    minimum_iou = std::min(minimum_iou, diff.soft_iou);
                    if (!diff.passes()) {
                        std::cerr << " frame=" << frame << " mean=" << mean << " IoU=" << diff.soft_iou << '\n';
                    }
                    std::vector<Pixel> pixels(std::size_t(actual.width) * actual.height);
                    std::memcpy(pixels.data(), actual.rgba.data(), actual.rgba.size());
                    save_png(folder / (id + "-" + std::to_string(frame) + ".png"), pixels, actual.width, actual.height);
                    require(diff.passes(), "GPU original scene outside unchanged Chromium tolerances");
                    ++captures;
                }
            }
            scene.unmount();
            require(scene.vm.stats().live_nodes == caseelephant::probe_constants::gallery::unmounted_live_nodes,
                    "GPU original scene unmount");
            if (count++) {
                report << ',';
            }
            report << json_string(id);
            std::cout << " OK\n";
        }
    }
    // Translucent encoded texels must be decoded before bilinear filtering.
    // This is a separate CPU alpha/color oracle for the new conversion pass.
    caseelephant::ProbeSceneFrame alpha;
    alpha.root.box = {0, 0, 80, 80};
    alpha.root.tag = "div";
    alpha.root.identity = {1, 1, 900};
    alpha.root.style = {{"background-color", "transparent"}, {"overflow", "hidden"}};
    for (unsigned i = 0; i < 2; ++i) {
        auto child = alpha.root;
        child.identity.slot = i + 2;
        child.box = {float(10 + i * 15), 10, 32, 32};
        child.style = {{"background-color", i ? "rgba(0,80,255,.6)" : "rgba(255,40,0,.25)"}, {"border-radius", "4px"}};
        alpha.root.children.push_back(child);
    }
    caseelephant::SceneRasterProbe software;
    auto bitmap = software.raster(alpha, caseelephant::probe_constants::gallery::scene_raster_scale);
    auto& source = surfaces.paint(0, alpha);
    surfaces.publish();
    FramePacket expected;
    expected.assets.push_back(
        std::make_shared<const Asset>(Asset{bitmap.width, bitmap.height, Encoding::SrgbStraight, bitmap.rgba}));
    expected.draws.push_back({{0, 0, 80, 80}, {.1f, .2f, .3f, .6f}});
    expected.draws.push_back({{8.25f, 9.75f, 64, 64}, {1, 1, 1, 1}, 0, false, false, .7f});
    auto input = expected;
    input.assets.clear();
    std::array<bool, caseelephant::probe_constants::gallery::core_scene_count> visible{};
    visible[0] = true;
    auto serial = pipeline.submit(std::move(input),
                                  [] {
                                  },
                                  {}, {source.handle});
    surfaces.submitted(serial, visible);
    pipeline.drain();
    auto pixels = gallery_output(gpu);
    unsigned conversion_error{};
    for (int y = 3; y < 79; y += 3) {
        for (int x = 3; x < 79; x += 3) {
            auto reference = cheetah_sample(expected, x, y);
            auto actual = pixels[std::size_t(y) * gpu.width + x];
            for (unsigned c = 0; c < 4; ++c) {
                if (auto error = unsigned(std::abs(int(reference[c]) - actual[c])); error > conversion_error) {
                    conversion_error = error;
                    if (error > caseelephant::probe_constants::gallery::max_cpu_oracle_difference) {
                        std::cerr << "conversion (" << x << "," << y << "," << c << ") CPU=" << unsigned(reference[c])
                                  << " GPU=" << unsigned(actual[c]) << " difference=" << error << '\n';
                    }
                }
            }
        }
    }
    require(conversion_error <= caseelephant::probe_constants::gallery::max_cpu_oracle_difference,
            "GPU encoded-alpha conversion/filter oracle");
    auto full = surfaces.read(0);
    auto& cropped = surfaces.paint(0, alpha, caseelephant::probe_constants::gallery::scene_raster_scale,
                                   std::array<float, 4>{5, 4, 64, 70});
    surfaces.publish();
    input = expected;
    input.assets.clear();
    serial = pipeline.submit(std::move(input),
                             [] {
                             },
                             {}, {cropped.handle});
    surfaces.submitted(serial, visible);
    auto clipped = surfaces.read(0);
    for (unsigned y = 0; y < 160; ++y) {
        for (unsigned x = 0; x < 160; ++x) {
            auto offset = (std::size_t(y) * 160 + x) * 4;
            const bool inside = x >= 10 && x < 128 && y >= 8 && y < 140;
            for (unsigned c = 0; c < 4; ++c) {
                require(clipped.rgba[offset + c] == (inside ? full.rgba[offset + c] : 0),
                        "GPU viewport crop changed visible pixels or retained hidden pixels");
            }
        }
    }
    auto& restored = surfaces.paint(0, alpha);
    surfaces.publish();
    input = expected;
    input.assets.clear();
    serial = pipeline.submit(std::move(input),
                             [] {
                             },
                             {}, {restored.handle});
    surfaces.submitted(serial, visible);
    require(surfaces.read(0).rgba == full.rgba, "GPU scroll reveal retained cropped pixels");
    surfaces.release();
    gpu.evict_assets(0, true);
    gpu.check_debug_messages();
    require(count == caseelephant::probe_constants::gallery::core_scene_count && captures == 102 &&
                pipeline.submissions == pipeline.completions,
            "GPU original capture/completion coverage");
    if (optimized) {
        std::cout << "native=" << surfaces.native_work().passes << " fallback=" << surfaces.native_work().fallbacks
                  << " layers=" << surfaces.native_work().layer_builds << "/" << surfaces.native_work().layer_reuses
                  << "\n";
    }
    if (optimized) {
        require(surfaces.native_work().passes > 1000 && surfaces.native_work().fallbacks > 500 &&
                    surfaces.native_work().layer_reuses > 300,
                "original XHTML/SVG native and fallback paths unexercised");
    }
    report << "],\"optimized\":" << (optimized ? "true" : "false")
           << ",\"native_passes\":" << surfaces.native_work().passes
           << ",\"native_fallbacks\":" << surfaces.native_work().fallbacks
           << ",\"native_layer_builds\":" << surfaces.native_work().layer_builds
           << ",\"native_layer_reuses\":" << surfaces.native_work().layer_reuses << ",\"captures\":" << captures
           << ",\"frames\":" << pipeline.submissions << ",\"worst_channel_mean\":" << worst_mean
           << ",\"minimum_soft_iou\":" << minimum_iou << ",\"pool_slots\":" << surfaces.allocated
           << ",\"pool_reuses\":" << surfaces.reuses << ",\"retired_imports\":" << surfaces.retired
           << ",\"peak_in_flight\":" << pipeline.peak_in_flight
           << ",\"alpha_conversion_max_difference\":" << conversion_error
           << ",\"crop_reveal_verified\":true,\"debug_warnings\":0}";
    write_text(folder / "report.json", report.str());
    std::cout << "PASS GPU 26 CSS + 8 D3 / 102 unchanged Chromium references / actual completion retirement\n";
    return 0;
}
