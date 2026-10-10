#include "probe_constants.hpp"

int run_backend(bool warp, const std::filesystem::path& folder) {
    std::filesystem::create_directories(folder);
    unsigned retired{};
    unsigned max_difference{};
    std::string adapter;
    bool debug = true;
    Color linear_sample{};
    for (unsigned mode = 0; mode < 4; ++mode) {
        FramePacket packet;
        if (mode == 0) {
            packet = scene();
            std::erase_if(packet.draws, [](const Draw& d) {
                return d.group || d.clipped;
            });
        } else if (mode == 1) {
            packet = sdf_scene().packet;
        } else {
            packet.draws.push_back({{0, 0, caseelephant::probe_constants::color_fixture_width,
                                     caseelephant::probe_constants::color_fixture_height},
                                    {0, 1, 0, 0.5f}});
        }
        std::vector<Pixel> baseline;
        if (mode < 2) {
            Experiment reference(warp);
            baseline = reference.execute(false, packet);
            reference.check_debug_messages();
            debug = debug && reference.debug_layer;
        }
        Experiment gpu(warp);
        adapter = gpu.adapter;
        auto& backend = gpu.ensure_backend();
        ComPtr<ID3D11DeviceContext> context;
        checked(gpu.device->CreateDeferredContext(0, &context), "host supplied deferred context");
        auto target = mode == 2 ? gpu.surface(DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT)
                                : gpu.surface(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        PacketView input(packet, 1);
        ce_render_handle prepared{};
        ce_render_handle use{};
        render_result(ce_renderer_prepare(backend.renderer, &input.frame, &prepared), "API complete input snapshot");
        packet = {}; // All call-scoped source pixels and draw arrays can disappear.
        void* native_commands{};
        ce_render_d3d11_pass native{};
        auto pass = backend.pass(context.Get(), target.rtv.Get(), &native_commands, native);
        if (mode == 2) {
            pass.target_format = CE_RENDER_TARGET_LINEAR_RGBA32F;
        }
        if (mode == 3) {
            const float red[4]{1, 0, 0, 1};
            gpu.immediate->ClearRenderTargetView(target.rtv.Get(), red);
            native.clear_target = 0; // UI overlays a target already owned by host.
        }
        if (mode == 0) {
            const auto before = gpu.uploads;
            auto invalid = pass;
            invalid.sample_count = 4;
            gpu.test(ce_renderer_encode(backend.renderer, prepared, &invalid, &use) == CE_RENDER_UNSUPPORTED_FEATURE &&
                         !use.renderer_id,
                     "unsupported target rejected before recording");
            {
                Experiment foreign(warp);
                native.deferred_context = foreign.deferred.Get();
                gpu.test(ce_renderer_encode(backend.renderer, prepared, &pass, &use) == CE_RENDER_WRONG_BACKEND &&
                             !native_commands,
                         "foreign native device rejected");
                native.deferred_context = context.Get();
            }
            gpu.test(gpu.uploads == before, "invalid native pass wrote no upload commands");
            render_result(ce_renderer_encode(backend.renderer, prepared, &pass, &use), "API cancelled recording");
            ComPtr<ID3D11CommandList> cancelled;
            cancelled.Attach(static_cast<ID3D11CommandList*>(native_commands));
            cancelled.Reset();
            native_commands = nullptr;
            render_result(ce_renderer_discard_encoded(backend.renderer, use),
                          "API discard AFTER native list destruction");
            backend.inject_recording_failure = true;
            gpu.test(ce_renderer_encode(backend.renderer, prepared, &pass, &use) == CE_RENDER_BACKEND_ERROR &&
                         !use.renderer_id && !native_commands,
                     "partial recording fully rolled back before unpin");
            backend.inject_recording_failure = false;
            for (const auto& asset : gpu.assets) {
                ResourceInfo info;
                lifetime(gpu.tracker.InspectResource(asset.id, info), "API cancelled resource");
                gpu.test(info.phase == ResourcePhase::Created && !info.encoded_pins,
                         "cancelled API upload never Ready");
            }
        }
        render_result(ce_renderer_encode(backend.renderer, prepared, &pass, &use), "API native encode");
        render_result(ce_renderer_release_prepared(backend.renderer, prepared),
                      "API release snapshot before submission");
        ComPtr<ID3D11CommandList> commands;
        commands.Attach(static_cast<ID3D11CommandList*>(native_commands));
        const D3D11_VIEWPORT host_viewport{3, 4, 17, 19, 0, 1};
        gpu.immediate->RSSetViewports(1, &host_viewport);
        gpu.immediate->ExecuteCommandList(commands.Get(), TRUE);
        gpu.immediate->End(gpu.ui_done.Get());
        gpu.immediate->CopyResource(mode == 2 ? gpu.alpha_staging.Get() : gpu.staging.Get(), target.texture.Get());
        gpu.immediate->End(gpu.host_done.Get());
        gpu.submitted = true;
        gpu.pending_completion = 2;
        render_result(
            ce_renderer_notify_submitted(backend.renderer, use, caseelephant::probe_constants::immediate_queue, 1),
            "API acknowledge actual host submission");
        commands.Reset();
        gpu.test(ce_renderer_discard_encoded(backend.renderer, use) == CE_RENDER_INVALID_STATE,
                 "submitted API use cannot be cancelled");
        gpu.test(ce_renderer_destroy(backend.renderer) == CE_RENDER_BUSY, "API destroy requires actual GPU completion");
        gpu.test(!gpu.native_releases, "API release snapshot did not retire submitted textures");
        gpu.complete();
        ce_render_info info{};
        info.struct_size = sizeof(info);
        render_result(ce_renderer_get_info(backend.renderer, &info), "API completion state");
        gpu.test(info.completed_serial == 2 && !info.submitted_uses, "host-only copy serial completes UI use");
        D3D11_VIEWPORT restored{};
        UINT count = 1;
        gpu.immediate->RSGetViewports(&count, &restored);
        gpu.test(count == 1 && std::memcmp(&restored, &host_viewport, sizeof(restored)) == 0,
                 "host context viewport preserved");
        D3D11_MAPPED_SUBRESOURCE mapped{};
        checked(
            gpu.immediate->Map(mode == 2 ? gpu.alpha_staging.Get() : gpu.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
            "API completed target readback");
        if (mode != 2) {
            std::vector<Pixel> pixels(width * height);
            for (UINT y = 0; y < height; ++y) {
                std::memcpy(pixels.data() + size_t(y) * width,
                            static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch,
                            width * sizeof(Pixel));
            }
            gpu.immediate->Unmap(gpu.staging.Get(), 0);
            if (mode < 2) {
                unsigned difference{};
                for (size_t i = 0; i < pixels.size(); ++i) {
                    for (size_t c = 0; c < 4; ++c) {
                        difference = std::max(difference, unsigned(std::abs(int(pixels[i][c]) - int(baseline[i][c]))));
                    }
                }
                gpu.test(difference == 0, "backend API matches existing actual GPU reference");
                max_difference = std::max(max_difference, difference);
            } else {
                gpu.test(pixels[32 * width + 32] == Pixel{188, 188, 0, 255},
                         "API preserves host red target under half-green overlay");
            }
            save_png(folder / (mode == 0 ? "color.png" : mode == 1 ? "sdf.png" : "preserve.png"), pixels);
        } else {
            std::memcpy(linear_sample.data(), mapped.pData, sizeof(Color));
            gpu.immediate->Unmap(gpu.alpha_staging.Get(), 0);
            gpu.test(linear_sample == Color{0, 0.5f, 0, 0.5f}, "API host float target preserves premultiplied alpha");
        }
        gpu.check_debug_messages();
        debug = debug && gpu.debug_layer;
        retired += unsigned(gpu.native_releases);
        gpu.test(gpu.native_releases == input.assets.size(), "API native registry retirement after actual completion");
    }
    std::ostringstream report;
    report
        << "{\"fixture\":\"caseelephant-render-backend-v1\",\"driver\":" << json_string(warp ? "warp" : "hardware")
        << ",\"adapter\":" << json_string(adapter)
        << ",\"abi_version\":1,\"frames\":4,\"native_textures_retired\":" << retired
        << ",\"reference_max_difference\":" << max_difference
        << ",\"host_linear_target_sample\":" << linear_rgba(linear_sample)
        << ",\"host_context_target_verified\":true,\"host_contents_preserved\":true,\"snapshot_copy_verified\":true,\"cancel_failure_verified\":true,\"actual_completion_verified\":true"
        << ",\"debug_layer\":" << (debug ? "true" : "false")
        << ",\"debug_warning_error_count\":" << (debug ? "0" : "null")
        << ",\"vulkan_backend_verified\":false,\"metal_backend_verified\":false,\"device_removal_verified\":false,\"passed\":true}";
    write_text(folder / "report.json", report.str());
    std::cout
        << "PASS D3D11 backend C ABI: external host context/target, color/SDF parity, float alpha, cancel/failure, actual completion; "
        << retired << " textures retired\n";
    return 0;
}
