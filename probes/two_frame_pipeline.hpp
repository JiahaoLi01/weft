#pragma once
#include <functional>
#include "probe_constants.hpp"

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

    Experiment& gpu_;
    ProbeBackend& backend_;
    std::array<Slot, caseelephant::probe_constants::frames_in_flight> slots_;
    std::size_t head_{};
    std::size_t count_{};
    Slot* timed_slot_{};
    bool timing_enabled_ = true;

    struct ConstantScope {
        Experiment& gpu;
        ComPtr<ID3D11Buffer> previous;

        ConstantScope(Experiment& g, const ComPtr<ID3D11Buffer>& buffer) : gpu(g), previous(g.constants) {
            g.constants = buffer;
        }

        ~ConstantScope() {
            gpu.constants = std::move(previous);
        }
    };

    struct CompletionMarker {
        Experiment& gpu;
        Slot& slot;

        ~CompletionMarker() {
            gpu.immediate->End(slot.done.Get());
            gpu.immediate->Flush();
        }
    };

    std::chrono::steady_clock::time_point clock() const {
        return timing_enabled_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    }

    double elapsed(std::chrono::steady_clock::time_point start) const {
        return timing_enabled_
                   ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()
                   : 0;
    }

    Slot& reserve() {
        gpu_.wait_ms = 0;
        if (count_ == caseelephant::probe_constants::frames_in_flight) {
            poll();
            if (count_ == caseelephant::probe_constants::frames_in_flight) {
                ++capacity_waits;
                const auto start = clock();
                wait_oldest();
                gpu_.wait_ms = elapsed(start);
                capacity_wait_ms += gpu_.wait_ms;
            }
        }
        require(count_ < caseelephant::probe_constants::frames_in_flight, "two-frame bounded host capacity");
        return slots_[(head_ + count_) % caseelephant::probe_constants::frames_in_flight];
    }

    void commit(Slot& slot, bool ui) {
        const auto increment = ui ? static_cast<unsigned>(caseelephant::probe_constants::serials_per_ui_frame) : 1u;
        // Serial capacity is checked before any commands are executed.
        slot.serial = gpu_.window_serial + increment;
        slot.ui = ui;
        gpu_.window_serial = slot.serial;
        ++count_;
        gpu_.async_in_flight = count_;
        peak_in_flight = std::max(peak_in_flight, count_);
        ++submissions;
    }

    void wait_oldest() {
        gpu_.immediate->Flush();
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(static_cast<int>(caseelephant::probe_constants::completion_timeout_seconds));
        const auto old_count = count_;
        while (count_ == old_count) {
            poll();
            if (count_ != old_count) {
                break;
            }
            require(std::chrono::steady_clock::now() < deadline, "two-frame actual completion deadline");
            std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<int>(caseelephant::probe_constants::completion_poll_ms)));
        }
    }

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

    explicit TwoFramePipeline(Experiment& gpu) : gpu_(gpu), backend_(gpu.ensure_backend()) {
        require(gpu.cache_assets && !gpu.pipeline_enabled && (!gpu.submitted || gpu.acknowledged),
                "pipeline requires idle cached reference adapter");
        for (auto& slot : slots_) {
            D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
            checked(gpu.device->CreateQuery(&query, &slot.done), "pipeline completion query");
            D3D11_BUFFER_DESC buffer{};
            buffer.ByteWidth = sizeof(Constants);
            buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            checked(gpu.device->CreateBuffer(&buffer, nullptr, &slot.constants), "pipeline frame constant buffer");
            if (gpu.timing_disjoint) {
                query.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
                checked(gpu.device->CreateQuery(&query, &slot.disjoint), "pipeline timing frequency");
                query.Query = D3D11_QUERY_TIMESTAMP;
                checked(gpu.device->CreateQuery(&query, &slot.start), "pipeline timing start");
                checked(gpu.device->CreateQuery(&query, &slot.end), "pipeline timing end");
            }
        }
        gpu.pipeline_enabled = true;
    }

    ~TwoFramePipeline() {
        try {
            cancel_frame_timing();
            drain();
        } catch (...) {
            std::terminate();
        }
        gpu_.pipeline_enabled = false;
    }

    TwoFramePipeline(const TwoFramePipeline&) = delete;
    TwoFramePipeline& operator=(const TwoFramePipeline&) = delete;

    std::size_t in_flight() const {
        return count_;
    }

    void poll() {
        ++polls;
        while (count_) {
            auto& slot = slots_[head_];
            BOOL done = FALSE;
            const auto result =
                gpu_.immediate->GetData(slot.done.Get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            checked(result, "pipeline nonblocking completion query");
            if (result != S_OK || !done) {
                break;
            }
            double gpu_ms{};
            const auto serial = slot.serial;
            if (slot.ui && slot.timed) {
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT data{};
                UINT64 start{};
                UINT64 end{};
                require(gpu_.immediate->GetData(slot.disjoint.Get(), &data, sizeof(data),
                                                D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                            !data.Disjoint && data.Frequency,
                        "pipeline stable GPU timing");
                require(gpu_.immediate->GetData(slot.start.Get(), &start, sizeof(start),
                                                D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                            gpu_.immediate->GetData(slot.end.Get(), &end, sizeof(end),
                                                    D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                            end >= start,
                        "pipeline completed GPU timestamps");
                gpu_ms = double(end - start) * caseelephant::constants::milliseconds_per_second / data.Frequency;
            }
            render_result(ce_renderer_notify_completed(backend_.renderer, caseelephant::probe_constants::device_epoch,
                                                       caseelephant::probe_constants::immediate_queue, serial),
                          "pipeline host-confirmed completion");
            slot.commands.Reset();
            slot.host_lease.reset();
            slot.serial = 0;
            head_ = (head_ + 1) % caseelephant::probe_constants::frames_in_flight;
            --count_;
            gpu_.async_in_flight = count_;
            ++completions;
            completed_serial = serial;
            if (completed_timing && slot.timed) {
                completed_timing(serial, gpu_ms);
            }
        }
    }

    // Optional producer-inclusive elapsed range. CPU scheduling gaps on this
    // queue are included; this is not a sum of pure shader execution time.
    void set_timing(bool enabled) {
        timing_enabled_ = enabled;
    }

    void begin_frame_timing() {
        require(!timed_slot_, "one timed producer batch");
        timed_slot_ = &reserve();
        timed_slot_->timed = timing_enabled_ && bool(timed_slot_->disjoint);
        if (timed_slot_->timed) {
            gpu_.immediate->Begin(timed_slot_->disjoint.Get());
            gpu_.immediate->End(timed_slot_->start.Get());
        }
    }

    void cancel_frame_timing() {
        if (timed_slot_ && timed_slot_->timed) {
            gpu_.immediate->End(timed_slot_->disjoint.Get());
        }
        timed_slot_ = nullptr;
    }

    void drain() {
        ++drains;
        const auto start = clock();
        while (count_) {
            wait_oldest();
        }
        drain_wait_ms += elapsed(start);
    }

    template <class Present>
    std::uint64_t submit(FramePacket packet, Present&& present, std::shared_ptr<const void> host_lease = {},
                         const std::vector<ce_render_handle>& external = {}) {
        auto& slot = reserve();
        require(gpu_.window_serial <= UINT64_MAX - caseelephant::probe_constants::serials_per_ui_frame,
                "pipeline serial bound");
        ConstantScope constants(gpu_, slot.constants);
        PacketView view(packet, gpu_.window_serial + 1, &gpu_);
        ce_render_handle frame{};
        ce_render_handle use{};
        const auto started = clock();
        if (external.empty()) {
            render_result(ce_renderer_prepare(backend_.renderer, &view.frame, &frame), "pipeline prepare");
        } else {
            const ce_render_external_frame input{
                sizeof(input),  CE_RENDER_EXTERNAL_ABI_VERSION, view.frame, uint32_t(external.size()), 0,
                external.data()};
            render_result(ce_renderer_prepare_external(backend_.renderer, &input, &frame), "pipeline external prepare");
        }
        gpu_.prepare_ms = elapsed(started);
        void* native_commands{};
        ce_render_d3d11_pass native{};
        auto pass = backend_.pass(gpu_.deferred.Get(), gpu_.output.rtv.Get(), &native_commands, native);
        const auto encoding = clock();
        const auto encoded = ce_renderer_encode(backend_.renderer, frame, &pass, &use);
        render_result(ce_renderer_release_prepared(backend_.renderer, frame), "pipeline release prepared input");
        render_result(encoded, "pipeline encode");
        gpu_.encode_ms = elapsed(encoding);
        slot.commands.Attach(static_cast<ID3D11CommandList*>(native_commands));
        slot.host_lease = std::move(host_lease);
        packet = {};
        require(!timed_slot_ || timed_slot_ == &slot, "producer timed slot changed before submission");
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
        render_result(ce_renderer_notify_submitted(backend_.renderer, use,
                                                   caseelephant::probe_constants::immediate_queue, slot.serial - 1),
                      "pipeline actual UI submission");
        const auto presenting = clock();
        present();
        gpu_.present_ms = elapsed(presenting);
        return slot.serial;
    }

    template <class Present>
    std::uint64_t compose(Present&& present, std::shared_ptr<const void> host_lease = {}) {
        auto& slot = reserve();
        require(submissions && gpu_.window_serial < UINT64_MAX, "pipeline composition requires submitted UI result");
        ConstantScope constants(gpu_, slot.constants);
        slot.host_lease = std::move(host_lease);
        slot.timed = false;
        commit(slot, false);
        CompletionMarker marker{gpu_, slot};
        present();
        return slot.serial;
    }
};
