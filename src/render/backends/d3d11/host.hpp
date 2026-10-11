#pragma once

#include "render/backends/d3d11/backend.hpp"

#include <chrono>
#include <utility>

// Synchronous D3D11 sample host: one frame at a time through the renderer C ABI, executed on the
// immediate context and completed by an actual query before the next frame. TwoFramePipeline is the
// pipelined host.
namespace weft::render::d3d11 {
    // Prepares and encodes packet into renderer.output, executes it, calls present() after the UI work
    // and waits for completion. Times each stage into renderer.*_ms, and the GPU UI time when timing is
    // enabled.
    template <class Present>
    void render_window(Renderer& r, FramePacket packet, Present&& present) {
        ensure(!r.pipeline_enabled, "synchronous and asynchronous window hosts cannot be mixed");
        ensure(!r.submitted || r.acknowledged, "previous window frame must complete before reuse");
        auto& api = r.ensure_backend();
        const auto count = packet.list ? packet.list->assets.size() : packet.assets.size();
        PacketView view(packet, r.window_serial + 1, r.cache_assets ? &r : nullptr);
        weft_render_handle frame{};
        weft_render_handle use{};
        const auto started = std::chrono::steady_clock::now();
        auto milliseconds = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        render_result(view.prepare(api.renderer, frame), "backend prepare window");
        const auto prepared_at = std::chrono::steady_clock::now();
        r.prepare_ms = milliseconds(started, prepared_at);
        void* native_commands{};
        weft_render_d3d11_pass native{};
        auto pass = api.pass(r.deferred.Get(), r.output.rtv.Get(), &native_commands, native);
        const auto result = weft_renderer_encode(api.renderer, frame, &pass, &use);
        render_result(weft_renderer_release_prepared(api.renderer, frame), "backend release CPU/prepared window");
        render_result(result, "backend encode window");
        ComPtr<ID3D11CommandList> commands;
        commands.Attach(static_cast<ID3D11CommandList*>(native_commands));
        const auto encoded_at = std::chrono::steady_clock::now();
        r.encode_ms = milliseconds(prepared_at, encoded_at);
        // Source packet can be freed now; backend-use pins protect actual GPU data.
        packet = {};
        r.pending_completion = r.window_serial + weft::constants::render::d3d11::serials_per_ui_frame;
        r.acknowledged = false;
        if (r.timing_disjoint) {
            r.immediate->Begin(r.timing_disjoint.Get());
            r.immediate->End(r.timing_start.Get());
        }
        r.immediate->ExecuteCommandList(commands.Get(), TRUE);
        if (r.timing_disjoint) {
            r.immediate->End(r.timing_end.Get());
            r.immediate->End(r.timing_disjoint.Get());
        }
        r.immediate->End(r.ui_done.Get());
        r.submitted = true;
        {
            struct Marker {
                Renderer& r;

                ~Marker() {
                    r.immediate->End(r.host_done.Get());
                }
            } marker{r};

            render_result(weft_renderer_notify_submitted(
                              api.renderer, use, weft::constants::render::d3d11::immediate_queue, r.window_serial + 1),
                          "backend actual submission");
            commands.Reset();
            present();
        }
        const auto presented_at = std::chrono::steady_clock::now();
        r.present_ms = milliseconds(encoded_at, presented_at);
        r.complete();
        r.wait_ms = milliseconds(presented_at, std::chrono::steady_clock::now());
        r.window_serial += weft::constants::render::d3d11::serials_per_ui_frame;
        if (r.timing_disjoint) {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT data{};
            UINT64 start{};
            UINT64 end{};
            ensure(r.immediate->GetData(r.timing_disjoint.Get(), &data, sizeof(data), D3D11_ASYNC_GETDATA_DONOTFLUSH) ==
                           S_OK &&
                       !data.Disjoint && data.Frequency,
                   "stable GPU timing frequency");
            ensure(r.immediate->GetData(r.timing_start.Get(), &start, sizeof(start), D3D11_ASYNC_GETDATA_DONOTFLUSH) ==
                           S_OK &&
                       r.immediate->GetData(r.timing_end.Get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) ==
                           S_OK &&
                       end >= start,
                   "completed GPU timestamp interval");
            r.gpu_ui_ms = double(end - start) * weft::constants::milliseconds_per_second / data.Frequency;
        }
        ensure(r.cache_assets || count <= r.native_releases, "window glyph textures retired after actual completion");
        r.immediate->ClearState();
    }

    // Draws a display list through the renderer directly, without the C ABI: prepares it, encodes it into
    // renderer.output, executes and submits it, calls present() after the UI work and waits for the actual
    // completion. Only for a renderer without a C ABI backend or pipelined host.
    template <class Present>
    void render_list(Renderer& r, paint::DisplayList list, Present&& present) {
        ensure(!r.backend && !r.pipeline_enabled, "the display list host drives the renderer directly");
        ensure(!r.submitted || r.acknowledged, "previous frame must complete before reuse");
        auto check = [](LifetimeResult result, const char* operation) {
            ensure(result == LifetimeResult::Ok, operation);
        };
        r.assets.clear();
        auto frame = r.prepare(std::move(list));
        Encoded encoded;
        try {
            encoded = r.encode(frame, false);
        } catch (...) {
            check(r.tracker.ReleasePreparedFrame(frame.id), "release a list that failed to record");
            for (const auto& a : r.assets) {
                check(r.tracker.ReleaseResource(a.id), "release the leases of a list that failed to record");
            }
            r.collect();
            throw;
        }
        // Recording pins the assets; the CPU leases and the prepared frame can go now.
        check(r.tracker.ReleasePreparedFrame(frame.id), "release the prepared list");
        for (const auto& a : r.assets) {
            check(r.tracker.ReleaseResource(a.id), "release the list's asset leases");
        }
        r.pending_completion = r.window_serial + weft::constants::render::d3d11::serials_per_ui_frame;
        r.acknowledged = false;
        r.immediate->ExecuteCommandList(encoded.commands.Get(), TRUE);
        r.immediate->End(r.ui_done.Get());
        r.submitted = true;
        {
            struct Marker {
                Renderer& r;

                ~Marker() {
                    r.immediate->End(r.host_done.Get());
                }
            } marker{r};

            check(r.tracker.NotifySubmitted(encoded.token, weft::constants::render::d3d11::immediate_queue,
                                            r.window_serial + 1),
                  "list submission");
            encoded.commands.Reset();
            present();
        }
        r.complete();
        r.window_serial += weft::constants::render::d3d11::serials_per_ui_frame;
        r.immediate->ClearState();
    }

    // The completed offscreen result is persistent. Host-only composition advances
    // the same actual completion serial without inventing a new DOM/UI packet.
    template <class Present>
    void compose_window(Renderer& r, Present&& present) {
        ensure(!r.pipeline_enabled, "synchronous and asynchronous composition cannot be mixed");
        ensure(r.backend && r.submitted && r.acknowledged, "composition requires completed UI output");
        r.pending_completion = r.window_serial + 1;
        r.acknowledged = false;
        r.immediate->End(r.ui_done.Get());
        {
            struct Marker {
                Renderer& r;

                ~Marker() {
                    r.immediate->End(r.host_done.Get());
                }
            } marker{r};

            present();
        }
        r.complete();
        ++r.window_serial;
        r.immediate->ClearState();
    }
} // namespace weft::render::d3d11
