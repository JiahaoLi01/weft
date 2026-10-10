#pragma once

#include "render/backends/d3d11/backend.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace weft::render::d3d11 {
    // Owner-thread sample host. Two bounded submissions, actual event queries,
    // independent constant buffers and immutable host leases. The DOM is absent.
    // The shared offscreen RT is written/read in order on ONE D3D11 queue.
    class TwoFramePipeline {
        struct Slot {
            ComPtr<ID3D11Query> done;
            ComPtr<ID3D11Query> disjoint;
            ComPtr<ID3D11Query> start;
            ComPtr<ID3D11Query> end;
            ComPtr<ID3D11Buffer> constants;
            ComPtr<ID3D11CommandList> commands;
            std::shared_ptr<const void> host_lease;
            std::uint64_t serial{};
            bool ui{};
            bool timed{};
        };

        Renderer& gpu_;
        Backend& backend_;
        std::array<Slot, weft::constants::render::d3d11::frames_in_flight> slots_;
        std::size_t head_{};
        std::size_t count_{};
        Slot* timed_slot_{};
        bool timing_enabled_ = true;

        struct ConstantScope {
            Renderer& gpu;
            ComPtr<ID3D11Buffer> previous;

            ConstantScope(Renderer& g, const ComPtr<ID3D11Buffer>& buffer) : gpu(g), previous(g.constants) {
                g.constants = buffer;
            }

            ~ConstantScope() {
                gpu.constants = std::move(previous);
            }
        };

        struct CompletionMarker {
            Renderer& gpu;
            Slot& slot;

            ~CompletionMarker() {
                gpu.immediate->End(slot.done.Get());
                gpu.immediate->Flush();
            }
        };

        std::chrono::steady_clock::time_point clock() const;
        double elapsed(std::chrono::steady_clock::time_point start) const;
        Slot& reserve();
        void commit(Slot& slot, bool ui);
        void wait_oldest();

    public:
        std::size_t submissions{};
        std::size_t completions{};
        std::size_t peak_in_flight{};
        std::size_t capacity_waits{};
        std::size_t drains{};
        std::size_t polls{};
        double capacity_wait_ms{};
        double drain_wait_ms{};
        std::uint64_t completed_serial{};
        // Called after the corresponding real host query, outside the C ABI.
        std::function<void(std::uint64_t, double)> completed_timing;

        explicit TwoFramePipeline(Renderer& gpu);
        ~TwoFramePipeline();
        TwoFramePipeline(const TwoFramePipeline&) = delete;
        TwoFramePipeline& operator=(const TwoFramePipeline&) = delete;

        std::size_t in_flight() const {
            return count_;
        }

        // Retires every submission whose completion query has signaled, oldest first.
        void poll();
        // Optional producer-inclusive elapsed range. CPU scheduling gaps on this
        // queue are included; this is not a sum of pure shader execution time.
        void set_timing(bool enabled);
        void begin_frame_timing();
        void cancel_frame_timing();
        void drain();

        template <class Present>
        std::uint64_t submit(FramePacket packet, Present&& present, std::shared_ptr<const void> host_lease = {},
                             const std::vector<weft_render_handle>& external = {});

        template <class Present>
        std::uint64_t compose(Present&& present, std::shared_ptr<const void> host_lease = {});
    };

    template <class Present>
    std::uint64_t TwoFramePipeline::submit(FramePacket packet, Present&& present,
                                           std::shared_ptr<const void> host_lease,
                                           const std::vector<weft_render_handle>& external) {
        auto& slot = reserve();
        ensure(gpu_.window_serial <= UINT64_MAX - weft::constants::render::d3d11::serials_per_ui_frame,
               "pipeline serial bound");
        ConstantScope constants(gpu_, slot.constants);
        PacketView view(packet, gpu_.window_serial + 1, &gpu_);
        weft_render_handle frame{};
        weft_render_handle use{};
        const auto started = clock();
        if (external.empty()) {
            render_result(weft_renderer_prepare(backend_.renderer, &view.frame, &frame), "pipeline prepare");
        } else {
            const weft_render_external_frame input{
                sizeof(input),  WEFT_RENDER_EXTERNAL_ABI_VERSION, view.frame, uint32_t(external.size()), 0,
                external.data()};
            render_result(weft_renderer_prepare_external(backend_.renderer, &input, &frame),
                          "pipeline external prepare");
        }
        gpu_.prepare_ms = elapsed(started);
        void* native_commands{};
        weft_render_d3d11_pass native{};
        auto pass = backend_.pass(gpu_.deferred.Get(), gpu_.output.rtv.Get(), &native_commands, native);
        const auto encoding = clock();
        const auto encoded = weft_renderer_encode(backend_.renderer, frame, &pass, &use);
        render_result(weft_renderer_release_prepared(backend_.renderer, frame), "pipeline release prepared input");
        render_result(encoded, "pipeline encode");
        gpu_.encode_ms = elapsed(encoding);
        slot.commands.Attach(static_cast<ID3D11CommandList*>(native_commands));
        slot.host_lease = std::move(host_lease);
        packet = {};
        ensure(!timed_slot_ || timed_slot_ == &slot, "producer timed slot changed before submission");
        if (!timed_slot_) {
            slot.timed = timing_enabled_ && bool(slot.disjoint);
            if (slot.timed) {
                gpu_.immediate->Begin(slot.disjoint.Get());
                gpu_.immediate->End(slot.start.Get());
            }
        }
        timed_slot_ = nullptr;
        gpu_.immediate->ExecuteCommandList(slot.commands.Get(), TRUE);
        if (slot.timed) {
            gpu_.immediate->End(slot.end.Get());
            gpu_.immediate->End(slot.disjoint.Get());
        }
        commit(slot, true);
        CompletionMarker marker{gpu_, slot};
        render_result(weft_renderer_notify_submitted(backend_.renderer, use,
                                                     weft::constants::render::d3d11::immediate_queue, slot.serial - 1),
                      "pipeline actual UI submission");
        const auto presenting = clock();
        present();
        gpu_.present_ms = elapsed(presenting);
        return slot.serial;
    }

    template <class Present>
    std::uint64_t TwoFramePipeline::compose(Present&& present, std::shared_ptr<const void> host_lease) {
        auto& slot = reserve();
        ensure(submissions && gpu_.window_serial < UINT64_MAX, "pipeline composition requires submitted UI result");
        ConstantScope constants(gpu_, slot.constants);
        slot.host_lease = std::move(host_lease);
        slot.timed = false;
        commit(slot, false);
        CompletionMarker marker{gpu_, slot};
        present();
        return slot.serial;
    }
} // namespace weft::render::d3d11
