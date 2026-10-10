#pragma once

#include "probe_common.hpp"
#include "render/backends/d3d11/scene_renderer.hpp"
#include "gallery_scene_manifest.hpp"
#include "gallery_constants.hpp"
#include "../tests/scene_visual_compare.hpp"

namespace probes {
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
            weft::SceneRaster painter{weft::testing::thread_scene_text()};
            weft_render_handle handle{};
            std::uint64_t last_serial{};
            std::uint64_t version{};
            unsigned width{};
            unsigned height{};
            std::optional<std::array<float, 4>> clip;
            std::shared_ptr<const weft::SceneFrame> retained_frame;
            SceneRenderer::Slot native;
            bool last_native{};
        };

    private:
        Experiment& gpu_;
        TwoFramePipeline& pipeline_;
        Backend& api_;
        SceneRenderer native_;
        ComPtr<ID2D1Device> d2d_;
        ComPtr<ID3D11Buffer> constants_;
        std::array<std::array<std::unique_ptr<Slot>, weft::probe_constants::gallery::surface_versions_per_card>,
                   gallery_scene_count>
            slots_;
        std::array<Slot*, gallery_scene_count> current_{};
        weft::SceneRaster::Work retired_raster_{};
        std::uint64_t version_{};
        bool producer_pending{};
        std::vector<std::pair<unsigned, Slot*>> pending_;

        static void add_work(weft::SceneRaster::Work& total, const weft::SceneRaster::Work& value) {
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
            check_hresult(d2d_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &s.target),
                          "GPU scene D2D context");
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
            QuadConstants data{{0, 0, float(s.width), float(s.height)},
                               {float(s.width), float(s.height)},
                               weft::probe_constants::gallery::encoded_bgra_material,
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
            ctx->Draw(weft::probe_constants::gallery::quad_vertex_count, 0);
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

        const SceneRenderer& native_work() const {
            return native_;
        }

        SceneGpuSurfaces(Experiment& gpu, TwoFramePipeline& pipeline)
            : gpu_(gpu),
              pipeline_(pipeline),
              api_(gpu.ensure_backend()),
              native_(gpu, weft::testing::thread_scene_text()) {
            api_.enable_external(weft::probe_constants::gallery::external_texture_limit,
                                 weft::probe_constants::gallery::external_import_budget_bytes);
            ComPtr<IDXGIDevice> device;
            check_hresult(gpu.device.As(&device), "GPU scene D2D device identity");
            check_hresult(D2D1CreateDevice(device.Get(), nullptr, &d2d_), "GPU scene shared D2D device");
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = sizeof(QuadConstants);
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
                                render_result(weft_renderer_release_texture(api_.renderer, slot->handle),
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

        weft::SceneRaster::Work raster_work() const {
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

        Slot& paint(unsigned index, const weft::SceneFrame& frame,
                    float scale = weft::probe_constants::gallery::scene_raster_scale,
                    std::optional<std::array<float, 4>> clip = std::nullopt) {
            return paint_owned(index, std::make_shared<const weft::SceneFrame>(frame), scale, clip);
        }

        Slot& paint_owned(unsigned index, std::shared_ptr<const weft::SceneFrame> value,
                          float scale = weft::probe_constants::gallery::scene_raster_scale,
                          std::optional<std::array<float, 4>> clip = std::nullopt,
                          std::vector<weft::NodeIdentity> layers = {}) {
            require(bool(value), "owned scene snapshot missing");
            const auto& frame = *value;
            require(std::isfinite(scale) && scale > 0 && scale <= weft::probe_constants::gallery::max_surface_scale,
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
                render_result(weft_renderer_release_texture(api_.renderer, s.handle),
                              "GPU scene completed caller retirement");
                s.handle = {};
                ++retired;
                ++reuses;
            }
            const auto w = unsigned(std::ceil(frame.root.box[2] * scale));
            const auto h = unsigned(std::ceil(frame.root.box[3] * scale));
            require(w && h && std::uint64_t(w) * h <= weft::probe_constants::gallery::max_surface_pixels,
                    "GPU scene surface bound");
            if (!s.encoded || s.width != w || s.height != h) {
                create(s, w, h);
                s.retained_frame.reset();
            }
            peak_live_slots = std::max(peak_live_slots, live_slots());
            producer_pending = true;
            s.painter.set_layers(layers);
            auto damage = weft::SceneDamage{true, true, {0, 0, frame.root.box[2], frame.root.box[3]}};
            if (retain_paint && s.retained_frame && s.clip == clip) {
                damage = weft::scene_damage(*s.retained_frame, frame, scale);
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
                weft_render_info info{};
                info.struct_size = sizeof(info);
                render_result(weft_renderer_get_info(api_.renderer, &info), "GPU scene renderer epoch");
                weft_render_d3d11_texture payload{sizeof(payload), 1, s.linear.srv.Get(), 0};
                require(version_ != UINT64_MAX, "GPU scene version generation bound");
                s.version = ++version_;
                weft_render_external_texture description{sizeof(description),
                                                         1,
                                                         info.identity,
                                                         weft::probe_constants::gallery::external_texture_id_base +
                                                             index,
                                                         s.version,
                                                         w,
                                                         h,
                                                         WEFT_RENDER_TARGET_LINEAR_RGBA16F,
                                                         0,
                                                         0,
                                                         0,
                                                         0,
                                                         0,
                                                         &payload,
                                                         sizeof(payload),
                                                         nullptr,
                                                         nullptr};
                const auto result = weft_renderer_import_texture(api_.renderer, &description, &s.handle);
                weft_render_external_info external{sizeof(external)};
                render_result(weft_renderer_get_external_info(api_.renderer, &external), "GPU scene import accounting");
                peak_imported_bytes = std::max(peak_imported_bytes, external.texture_bytes);
                if (result != WEFT_RENDER_OK) {
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
        std::vector<weft_render_handle> dependencies(FramePacket& packet, const std::array<int, N>& indices,
                                                     const std::array<bool, N>& visible) {
            std::vector<int> mapping(packet.assets.size(), -1);
            std::vector<weft_render_handle> external;
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
                        render_result(weft_renderer_release_texture(api_.renderer, slot->handle),
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

        weft::SceneBitmap read(unsigned index) {
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
            weft::SceneBitmap out{s.width, s.height, std::vector<std::uint8_t>(std::size_t(s.width) * s.height * 4)};
            for (unsigned y = 0; y < s.height; ++y) {
                for (unsigned x = 0; x < s.width; ++x) {
                    auto p = static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(y) * mapped.RowPitch + x * 4;
                    auto q = out.rgba.data() + (std::size_t(y) * s.width + x) * 4;
                    unsigned alpha = p[3];
                    q[3] = std::uint8_t(alpha);
                    for (unsigned c = 0; c < 3; ++c) {
                        q[c] = alpha
                                   ? std::uint8_t(std::min(
                                         static_cast<unsigned>(weft::constants::color_channel_max),
                                         (unsigned(p[2 - c]) * weft::constants::color_channel_max + alpha / 2) / alpha))
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
                    std::chrono::seconds(weft::probe_constants::gallery::producer_completion_timeout_s);
                while (gpu_.immediate->GetData(done.Get(), &complete, sizeof(complete), 0) != S_OK || !complete) {
                    require(std::chrono::steady_clock::now() < deadline, "GPU producer-only completion deadline");
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(weft::probe_constants::gallery::completion_poll_ms));
                }
                producer_pending = false;
            }
            pipeline_.drain();
            for (auto& group : slots_) {
                for (auto& slot : group) {
                    if (slot && slot->handle.renderer_id) {
                        render_result(weft_renderer_release_texture(api_.renderer, slot->handle),
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
} // namespace probes
