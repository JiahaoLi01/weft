// GPU producer -> external lease -> UI. Only final validation targets are read
// back; producer pixels never cross a CPU readback/upload boundary.
#include "probe_constants.hpp"

struct ExternalRetirement {
    unsigned count{};
    unsigned reason{};

    static void notify(void* p, uint32_t reason) noexcept {
        auto& s = *static_cast<ExternalRetirement*>(p);
        ++s.count;
        s.reason = reason;
    }
};

ComPtr<ID3D11Buffer> external_constants(Experiment& gpu) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = sizeof(Constants);
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> result;
    check_hresult(gpu.device->CreateBuffer(&d, nullptr, &result), "external independent constants");
    return result;
}

ComPtr<ID3D11Texture2D> external_readback(Experiment& gpu, DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = gpu.width;
    d.Height = gpu.height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> result;
    check_hresult(gpu.device->CreateTexture2D(&d, nullptr, &result), "external final target staging");
    return result;
}

std::array<Color, 4> external_colors(unsigned variation, uint32_t format = WEFT_RENDER_TARGET_LINEAR_RGBA32F) {
    auto colors = variation % 2 ? std::array<Color, 4>{Color{0, .5f, .5f, .5f}, Color{.25f, 0, .25f, .25f},
                                                       Color{1, 1, 0, 1}, Color{0, 0, 0, 0}}
                                : std::array<Color, 4>{Color{.5f, 0, 0, .5f}, Color{0, .25f, 0, .25f},
                                                       Color{0, 0, 1, 1}, Color{0, 0, 0, 0}};
    if (format == WEFT_RENDER_TARGET_LINEAR_RGBA8) {
        for (auto& c : colors) {
            for (auto& component : c) {
                component =
                    std::round(component * weft::constants::color_channel_max) / weft::constants::color_channel_max;
            }
        }
    }
    return colors;
}

void external_produce(Experiment& gpu, const Surface& target, unsigned variation, const ComPtr<ID3D11Buffer>& buffer) {
    struct Restore {
        Experiment& gpu;
        ComPtr<ID3D11Buffer> old;

        ~Restore() {
            gpu.constants = std::move(old);
        }
    } restore{gpu, gpu.constants};

    gpu.constants = buffer;
    auto* ctx = gpu.deferred.Get();
    ctx->ClearState();
    auto* rtv = target.rtv.Get();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    const float clear[4]{};
    ctx->ClearRenderTargetView(rtv, clear);
    const D3D11_VIEWPORT viewport{0, 0, float(gpu.width), float(gpu.height), 0, 1};
    ctx->RSSetViewports(1, &viewport);
    ctx->RSSetState(gpu.raster.Get());
    ctx->OMSetDepthStencilState(gpu.depth.Get(), 0);
    ctx->OMSetBlendState(gpu.blend.Get(), nullptr, D3D11_DEFAULT_SAMPLE_MASK);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(gpu.vs.Get(), nullptr, 0);
    ctx->PSSetShader(gpu.ps.Get(), nullptr, 0);
    auto* cb = buffer.Get();
    ctx->VSSetConstantBuffers(0, 1, &cb);
    ctx->PSSetConstantBuffers(0, 1, &cb);
    auto* sampler = gpu.sampler.Get();
    ctx->PSSetSamplers(0, 1, &sampler);
    const auto colors = external_colors(variation);
    for (unsigned q = 0; q < 4; ++q) {
        auto straight = colors[q];
        if (straight[3]) {
            for (unsigned c = 0; c < 3; ++c) {
                straight[c] /= straight[3];
            }
        }
        gpu.draw(
            {{float(q % 2) * gpu.width / 2, float(q / 2) * gpu.height / 2, float(gpu.width) / 2, float(gpu.height) / 2},
             straight},
            nullptr, weft::probe_constants::shader_material::solid);
    }
    ctx->ClearState();
    ComPtr<ID3D11CommandList> commands;
    check_hresult(ctx->FinishCommandList(FALSE, &commands), "external producer commands");
    gpu.immediate->ExecuteCommandList(commands.Get(), TRUE);
    // Same immediate queue orders producer first; no Flush/GetData/CPU wait.
}

weft_render_external_texture external_gpu_description(ProbeBackend& api, const Surface& source,
                                                      weft_render_d3d11_texture& payload, ExternalRetirement& retired,
                                                      uint64_t version, bool bottom = false,
                                                      uint32_t format = WEFT_RENDER_TARGET_LINEAR_RGBA32F) {
    weft_render_info info{};
    info.struct_size = sizeof(info);
    render_result(weft_renderer_get_info(api.renderer, &info), "external identity");
    payload = {sizeof(payload), WEFT_RENDER_EXTERNAL_ABI_VERSION, source.srv.Get(), 0};
    return {sizeof(weft_render_external_texture),
            WEFT_RENDER_EXTERNAL_ABI_VERSION,
            info.identity,
            501,
            version,
            api.gpu.width,
            api.gpu.height,
            format,
            0,
            uint32_t(bottom),
            0,
            0,
            0,
            &payload,
            sizeof(payload),
            &retired,
            ExternalRetirement::notify};
}

Color external_sample(unsigned variation, bool bottom, uint32_t format, float u, float v, UINT w, UINT h) {
    if (bottom) {
        v = 1 - v;
    }
    const float x = u * w - .5f;
    const float y = v * h - .5f;
    const int ix = int(std::floor(x));
    const int iy = int(std::floor(y));
    const float fx = x - ix;
    const float fy = y - iy;
    const auto colors = external_colors(variation, format);
    Color result{};
    for (int yy = 0; yy < 2; ++yy) {
        for (int xx = 0; xx < 2; ++xx) {
            const int px = std::clamp(ix + xx, 0, int(w) - 1);
            const int py = std::clamp(iy + yy, 0, int(h) - 1);
            const auto& c = colors[(py >= int(h) / 2 ? 2 : 0) + (px >= int(w) / 2 ? 1 : 0)];
            const float weight = (xx ? fx : 1 - fx) * (yy ? fy : 1 - fy);
            for (unsigned channel = 0; channel < 4; ++channel) {
                result[channel] += weight * c[channel];
            }
        }
    }
    return result;
}

Color external_pixel(const weft_render_frame& frame, unsigned variation, bool bottom, uint32_t format, int x, int y,
                     UINT w, UINT h) {
    Color dst{};
    for (uint32_t i = 0; i < frame.draw_count; ++i) {
        const auto& d = frame.draws[i];
        const float px = x + .5f;
        const float py = y + .5f;
        if (px < d.x || py < d.y || px >= d.x + d.width || py >= d.y + d.height) {
            continue;
        }
        Color source{};
        if (d.asset_index == WEFT_RENDER_SOLID_ASSET || d.asset_index < frame.asset_count) {
            source[3] = d.srgb[3];
            if (d.asset_index != WEFT_RENDER_SOLID_ASSET) {
                source[3] *= frame.assets[d.asset_index].bytes[0] / weft::probe_constants::color_channel_max_float;
            }
            for (unsigned c = 0; c < 3; ++c) {
                source[c] = decode(d.srgb[c]) * source[3];
            }
        } else {
            source = external_sample(variation, bottom, format, d.uv[0] + (px - d.x) / d.width * d.uv[2],
                                     d.uv[1] + (py - d.y) / d.height * d.uv[3], w, h);
        }
        for (auto& c : source) {
            c *= d.opacity;
        }
        for (unsigned c = 0; c < 4; ++c) {
            dst[c] = source[c] + dst[c] * (1 - source[3]);
        }
    }
    return dst;
}

int run_external_texture(bool warp, const std::filesystem::path& folder) {
    std::filesystem::create_directories(folder);
    // Callback storage outlives gpu's exception drain and normal completion.
    std::array<ExternalRetirement, weft::probe_constants::external_batches * weft::probe_constants::frames_in_flight>
        retired;
    ExternalRetirement invalid_retirement;
    Experiment gpu(warp);
    gpu.cache_assets = true;
    auto& api = gpu.ensure_backend();
    api.enable_external();
    gpu.pipeline_enabled = true;
    unsigned maximum{};
    unsigned samples{};
    unsigned submissions{};
    unsigned peak{};
    float float_error{};
    uint32_t random = weft::probe_constants::external_seed;
    auto producer_buffer = external_constants(gpu);
    std::array<ComPtr<ID3D11Buffer>, weft::probe_constants::frames_in_flight> buffers{external_constants(gpu),
                                                                                      external_constants(gpu)};
    std::array<ComPtr<ID3D11Query>, weft::probe_constants::frames_in_flight> queries;
    const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
    for (auto& q : queries) {
        check_hresult(gpu.device->CreateQuery(&query, &q), "external actual query");
    }
    auto float_target = gpu.surface(DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT);
    std::array<ComPtr<ID3D11Texture2D>, weft::probe_constants::frames_in_flight> reads{
        external_readback(gpu, DXGI_FORMAT_R8G8B8A8_TYPELESS), external_readback(gpu, DXGI_FORMAT_R32G32B32A32_FLOAT)};
    weft_render_d3d11_texture payload{};
    weft_render_handle ignored{};
    {
        Experiment foreign(warp);
        auto d = external_gpu_description(api, foreign.group, payload, invalid_retirement, 900);
        gpu.test(weft_renderer_import_texture(api.renderer, &d, &ignored) == WEFT_RENDER_WRONG_BACKEND,
                 "external foreign native device rejected");
        d = external_gpu_description(api, gpu.output, payload, invalid_retirement, 901);
        gpu.test(weft_renderer_import_texture(api.renderer, &d, &ignored) == WEFT_RENDER_UNSUPPORTED_FEATURE,
                 "external encoded sRGB view rejected");
        gpu.test(invalid_retirement.count == 0, "failed native import transfers no lease");
    }
    for (unsigned batch = 0; batch < weft::probe_constants::external_batches; ++batch) {
        std::array<Surface, weft::probe_constants::frames_in_flight> producers;
        std::array<std::vector<weft_render_draw>, weft::probe_constants::frames_in_flight> expected_draws;
        std::array<weft_render_frame, weft::probe_constants::frames_in_flight> expected_frames;
        std::array<uint32_t, weft::probe_constants::frames_in_flight> formats;
        std::array<weft_render_handle, weft::probe_constants::frames_in_flight> leases{};
        const uint8_t coverage = 128;
        const weft_render_asset cpu_asset{1001, batch + 1, 1, 1, WEFT_RENDER_COVERAGE, 0, &coverage, 1, 0, 0};
        for (unsigned slot = 0; slot < weft::probe_constants::frames_in_flight; ++slot) {
            const auto index = batch * weft::probe_constants::frames_in_flight + slot;
            const auto serial = uint64_t(index) + 1;
            constexpr uint32_t types[]{WEFT_RENDER_TARGET_LINEAR_RGBA32F, WEFT_RENDER_TARGET_LINEAR_RGBA16F,
                                       WEFT_RENDER_TARGET_LINEAR_RGBA8};
            formats[slot] = types[index % 3];
            const auto native_format = index % 3 == 0   ? DXGI_FORMAT_R32G32B32A32_FLOAT
                                       : index % 3 == 1 ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                                        : DXGI_FORMAT_R8G8B8A8_UNORM;
            producers[slot] = gpu.surface(native_format, native_format);
            external_produce(gpu, producers[slot], index, producer_buffer);
            auto d = external_gpu_description(api, producers[slot], payload, retired[index], serial, slot == 1,
                                              formats[slot]);
            render_result(weft_renderer_import_texture(api.renderer, &d, &leases[slot]), "import GPU producer");
            if (index == 0) {
                ++d.content_version;
                gpu.test(weft_renderer_import_texture(api.renderer, &d, &ignored) == WEFT_RENDER_INVALID_STATE,
                         "duplicate native texture version rejected");
            }
            random = random * weft::constants::lcg_multiplier + weft::constants::lcg_increment;
            const auto inset = unsigned(random % 20);
            auto& draws = expected_draws[slot];
            draws.push_back({0,
                             0,
                             float(gpu.width),
                             float(gpu.height),
                             {.08f, .12f, .2f, .5f},
                             WEFT_RENDER_SOLID_ASSET,
                             1,
                             {0, 0, 1, 1},
                             0,
                             {}});
            draws.push_back({float(8 + inset),
                             8,
                             float(gpu.width - 32),
                             float(gpu.height - 16),
                             {1, 1, 1, 1},
                             1,
                             .75f,
                             {.07f, .11f, .86f, .78f},
                             0,
                             {}});
            draws.push_back({98, 40, 64, 44, {1, 1, 1, 1}, 1, .4f, {.25f, .2f, .5f, .6f}, 0, {}});
            draws.push_back({30, 20, 35, 60, {1, 1, 0, .5f}, 0, .8f, {0, 0, 1, 1}, 0, {}});
            expected_frames[slot] = {sizeof(weft_render_frame),
                                     WEFT_RENDER_ABI_VERSION,
                                     draws.data(),
                                     uint32_t(draws.size()),
                                     1,
                                     &cpu_asset};
            weft_render_external_frame input{
                sizeof(input), WEFT_RENDER_EXTERNAL_ABI_VERSION, expected_frames[slot], 1, 0, &leases[slot]};
            weft_render_handle prepared{};
            weft_render_handle use{};
            render_result(weft_renderer_prepare_external(api.renderer, &input, &prepared),
                          "prepare complete external packet");
            if (index == 0) {
                void* commands{};
                weft_render_d3d11_pass native{};
                auto bad = api.pass(gpu.deferred.Get(), producers[slot].rtv.Get(), &commands, native);
                bad.target_format = WEFT_RENDER_TARGET_LINEAR_RGBA32F;
                gpu.test(weft_renderer_encode(api.renderer, prepared, &bad, &use) == WEFT_RENDER_INVALID_STATE &&
                             !commands && !use.renderer_id,
                         "input target alias rejected before commands");
                bad = api.pass(gpu.deferred.Get(), gpu.output.rtv.Get(), &commands, native);
                render_result(weft_renderer_encode(api.renderer, prepared, &bad, &use), "external cancelled encode");
                ComPtr<ID3D11CommandList> cancelled;
                cancelled.Attach(static_cast<ID3D11CommandList*>(commands));
                cancelled.Reset();
                commands = nullptr;
                render_result(weft_renderer_discard_encoded(api.renderer, use),
                              "external cancel after command destruction");
                api.inject_recording_failure = true;
                gpu.test(weft_renderer_encode(api.renderer, prepared, &bad, &use) == WEFT_RENDER_BACKEND_ERROR &&
                             !commands && !use.renderer_id,
                         "external atomic failure rollback");
                api.inject_recording_failure = false;
            }
            const auto old = gpu.constants;
            gpu.constants = buffers[slot];
            void* commands{};
            weft_render_d3d11_pass native{};
            auto pass =
                api.pass(gpu.deferred.Get(), slot ? float_target.rtv.Get() : gpu.output.rtv.Get(), &commands, native);
            if (slot) {
                pass.target_format = WEFT_RENDER_TARGET_LINEAR_RGBA32F;
            }
            const auto result = weft_renderer_encode(api.renderer, prepared, &pass, &use);
            gpu.constants = old;
            render_result(result, "external encode");
            render_result(weft_renderer_release_prepared(api.renderer, prepared), "release external prepared");
            render_result(weft_renderer_release_texture(api.renderer, leases[slot]),
                          "release caller before actual submission");
            producers[slot] = {};
            payload = {};
            d = {};
            gpu.test(retired[index].count == 0, "encoded pins survive all original native views");
            ComPtr<ID3D11CommandList> list;
            list.Attach(static_cast<ID3D11CommandList*>(commands));
            gpu.immediate->ExecuteCommandList(list.Get(), TRUE);
            render_result(
                weft_renderer_notify_submitted(api.renderer, use, weft::probe_constants::immediate_queue, serial),
                "actual external submission");
            list.Reset();
            gpu.immediate->CopyResource(reads[slot].Get(),
                                        slot ? float_target.texture.Get() : gpu.output.texture.Get());
            gpu.immediate->End(queries[slot].Get());
            if (slot == 1) {
                gpu.immediate->End(gpu.ui_done.Get());
                gpu.immediate->End(gpu.host_done.Get());
                gpu.pending_completion = serial;
                gpu.submitted = true;
                gpu.acknowledged = false;
            }
            ++submissions;
            weft_render_info state{};
            state.struct_size = sizeof(state);
            render_result(weft_renderer_get_info(api.renderer, &state), "external in-flight count");
            peak = std::max(peak, state.submitted_uses);
        }
        gpu.test(retired[batch * weft::probe_constants::frames_in_flight].count == 0 &&
                     retired[batch * weft::probe_constants::frames_in_flight + 1].count == 0,
                 "two immutable leases survive before acknowledgment");
        gpu.immediate->Flush();
        for (unsigned slot = 0; slot < weft::probe_constants::frames_in_flight; ++slot) {
            const auto index = batch * weft::probe_constants::frames_in_flight + slot;
            const auto deadline =
                std::chrono::steady_clock::now() +
                std::chrono::seconds(static_cast<int>(weft::probe_constants::completion_timeout_seconds));
            BOOL done = FALSE;
            for (;;) {
                const auto hr =
                    gpu.immediate->GetData(queries[slot].Get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH);
                check_hresult(hr, "external completion query");
                if (hr == S_OK && done) {
                    break;
                }
                require(std::chrono::steady_clock::now() < deadline, "external query deadline");
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(static_cast<int>(weft::probe_constants::completion_poll_ms)));
            }
            render_result(weft_renderer_notify_completed(api.renderer, weft::probe_constants::device_epoch,
                                                         weft::probe_constants::immediate_queue, uint64_t(index) + 1),
                          "retire after real external completion");
            gpu.test(retired[index].count == 1 && retired[index].reason == WEFT_RENDER_EXTERNAL_RETIRED,
                     "external retirement exactly once");
            if (slot == 0) {
                gpu.test(retired[index + 1].count == 0, "earlier serial cannot retire later texture");
            }
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check_hresult(gpu.immediate->Map(reads[slot].Get(), 0, D3D11_MAP_READ, 0, &mapped),
                          "final external composite readback");
            std::vector<Pixel> png(size_t(gpu.width) * gpu.height);
            for (int y = 0; y < int(gpu.height); ++y) {
                for (int x = 0; x < int(gpu.width); ++x) {
                    const auto expected = external_pixel(expected_frames[slot], index, slot == 1, formats[slot], x, y,
                                                         gpu.width, gpu.height);
                    auto& pixel = png[size_t(y) * gpu.width + x];
                    if (slot) {
                        Color actual{};
                        std::memcpy(actual.data(),
                                    static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch +
                                        size_t(x) * sizeof(Color),
                                    sizeof(actual));
                        for (unsigned c = 0; c < 4; ++c) {
                            float_error = std::max(float_error, std::abs(actual[c] - expected[c]));
                            const float e = c == 3 ? actual[c]
                                            : actual[c] <= weft::probe_constants::srgb_encode_threshold
                                                ? actual[c] * weft::probe_constants::srgb_linear_slope
                                                : weft::probe_constants::srgb_scale *
                                                          std::pow(actual[c], 1 / weft::probe_constants::srgb_gamma) -
                                                      weft::probe_constants::srgb_offset;
                            pixel[c] =
                                uint8_t(std::lround(std::clamp(e, 0.f, 1.f) * weft::constants::color_channel_max));
                        }
                    } else {
                        std::memcpy(pixel.data(),
                                    static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch +
                                        size_t(x) * weft::constants::rgba8_bytes_per_pixel,
                                    weft::constants::rgba8_bytes_per_pixel);
                        for (unsigned c = 0; c < 4; ++c) {
                            const float e = c == 3 ? expected[c]
                                            : expected[c] <= weft::probe_constants::srgb_encode_threshold
                                                ? expected[c] * weft::probe_constants::srgb_linear_slope
                                                : weft::probe_constants::srgb_scale *
                                                          std::pow(expected[c], 1 / weft::probe_constants::srgb_gamma) -
                                                      weft::probe_constants::srgb_offset;
                            maximum = std::max(
                                maximum, unsigned(std::abs(int(pixel[c]) -
                                                           int(std::lround(e * weft::constants::color_channel_max)))));
                        }
                    }
                    ++samples;
                }
            }
            gpu.immediate->Unmap(reads[slot].Get(), 0);
            if (batch == 0 || batch == weft::probe_constants::external_batches - 1) {
                save_png(folder / ("batch-" + std::to_string(batch) + (slot ? "-bottom-linear.png" : "-top-srgb.png")),
                         png, gpu.width, gpu.height);
            }
        }
        gpu.complete();
        gpu.immediate->ClearState();
    }
    gpu.pipeline_enabled = false;
    // The mixed CPU coverage input follows the same cached two-frame ownership
    // path as the window host, and is evicted only after the actual drain.
    gpu.evict_assets(0, true);
    gpu.test(gpu.native_releases == weft::probe_constants::external_batches,
             "mixed CPU coverage retires after actual two-frame drain");
    std::cout << "external oracle observed: sRGB max " << maximum << ", float max " << float_error << '\n';
    // D3D11 texture filtering uses finite subtexel precision. The independent
    // float oracle bounds linear error as well as the nonlinear sRGB readback.
    gpu.test(maximum <= weft::probe_constants::cpu_oracle_tolerance &&
                 float_error <= weft::probe_constants::external_float_tolerance,
             "external independent bilinear alpha/color oracle");
    gpu.test(
        peak == weft::probe_constants::frames_in_flight &&
            api.imported_count == weft::probe_constants::external_batches * weft::probe_constants::frames_in_flight &&
            api.released_imports == weft::probe_constants::external_batches * weft::probe_constants::frames_in_flight,
        "external two-frame registry fully drained");
    weft_render_external_info state{};
    state.struct_size = sizeof(state);
    render_result(weft_renderer_get_external_info(api.renderer, &state), "external final registry");
    gpu.test(!state.live_textures && !state.caller_leases && !state.texture_bytes, "no residual external lease");
    gpu.check_debug_messages();
    std::ostringstream report;
    report
        << "{\"fixture\":\"external-texture-lease-v1\",\"driver\":" << json_string(warp ? "warp" : "hardware")
        << ",\"adapter\":" << json_string(gpu.adapter)
        << ",\"target\":[256,128],\"seed\":" << weft::probe_constants::external_seed << ",\"frames\":" << submissions
        << ",\"peak_in_flight\":" << peak << ",\"external_textures_retired\":" << api.released_imports
        << ",\"cpu_coverage_textures_retired\":" << gpu.native_releases << ",\"cpu_pixel_samples\":" << samples
        << ",\"srgb_max_difference\":" << maximum << ",\"linear_float_max_error\":" << float_error
        << ",\"producer_gpu_only\":true,\"source_formats\":[\"RGBA8\",\"RGBA16F\",\"RGBA32F\"],\"actual_completion_queries\":true,\"top_bottom_origin_verified\":true,\"alias_cancel_failure_verified\":true"
        << ",\"debug_layer\":" << (gpu.debug_layer ? "true" : "false")
        << ",\"debug_warning_error_count\":" << (gpu.debug_layer ? "0" : "null")
        << ",\"encoded_srgb_input_supported\":false,\"angle_integrated\":false,\"mobile_verified\":false,\"performance_verified\":false,\"passed\":true}";
    write_text(folder / "report.json", report.str());
    std::cout << "PASS external GPU leases: 48 frames / two in flight / actual retirement; " << samples
              << " oracle pixels, sRGB max " << maximum << ", float max " << float_error << '\n';
    return 0;
}
