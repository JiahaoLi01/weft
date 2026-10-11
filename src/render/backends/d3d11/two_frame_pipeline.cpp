#include "render/backends/d3d11/two_frame_pipeline.hpp"

#include <algorithm>
#include <exception>
#include <thread>

namespace weft::render::d3d11 {
    namespace {
        namespace limits = weft::constants::render::d3d11;
    } // namespace

    std::chrono::steady_clock::time_point TwoFramePipeline::clock() const {
        return timing_enabled_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    }

    double TwoFramePipeline::elapsed(std::chrono::steady_clock::time_point start) const {
        return timing_enabled_
                   ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()
                   : 0;
    }

    TwoFramePipeline::Slot& TwoFramePipeline::reserve() {
        gpu_.wait_ms = 0;
        if (count_ == limits::frames_in_flight) {
            poll();
            if (count_ == limits::frames_in_flight) {
                ++capacity_waits;
                const auto start = clock();
                wait_oldest();
                gpu_.wait_ms = elapsed(start);
                capacity_wait_ms += gpu_.wait_ms;
            }
        }
        ensure(count_ < limits::frames_in_flight, "two-frame bounded host capacity");
        return slots_[(head_ + count_) % limits::frames_in_flight];
    }

    void TwoFramePipeline::commit(Slot& slot, bool ui) {
        const auto increment = ui ? static_cast<unsigned>(limits::serials_per_ui_frame) : 1u;
        // Serial capacity is checked before any commands are executed.
        slot.serial = gpu_.window_serial + increment;
        slot.ui = ui;
        gpu_.window_serial = slot.serial;
        ++count_;
        gpu_.async_in_flight = count_;
        peak_in_flight = std::max(peak_in_flight, count_);
        ++submissions;
    }

    void TwoFramePipeline::wait_oldest() {
        gpu_.immediate->Flush();
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(static_cast<int>(limits::completion_timeout_seconds));
        const auto old_count = count_;
        while (count_ == old_count) {
            poll();
            if (count_ != old_count) {
                break;
            }
            ensure(std::chrono::steady_clock::now() < deadline, "two-frame actual completion deadline");
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(limits::completion_poll_ms)));
        }
    }

    TwoFramePipeline::TwoFramePipeline(Renderer& gpu) : gpu_(gpu), backend_(gpu.ensure_backend()) {
        ensure(gpu.cache_assets && !gpu.pipeline_enabled && (!gpu.submitted || gpu.acknowledged),
               "pipeline requires idle cached reference adapter");
        for (auto& slot : slots_) {
            D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
            throw_if_failed(gpu.device->CreateQuery(&query, &slot.done), "pipeline completion query");
            D3D11_BUFFER_DESC buffer{};
            buffer.ByteWidth = weft::constants::render::scene::view_constants_bytes;
            buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            throw_if_failed(gpu.device->CreateBuffer(&buffer, nullptr, &slot.constants),
                            "pipeline frame constant buffer");
            if (gpu.timing_disjoint) {
                query.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
                throw_if_failed(gpu.device->CreateQuery(&query, &slot.disjoint), "pipeline timing frequency");
                query.Query = D3D11_QUERY_TIMESTAMP;
                throw_if_failed(gpu.device->CreateQuery(&query, &slot.start), "pipeline timing start");
                throw_if_failed(gpu.device->CreateQuery(&query, &slot.end), "pipeline timing end");
            }
        }
        gpu.pipeline_enabled = true;
    }

    TwoFramePipeline::~TwoFramePipeline() {
        try {
            cancel_frame_timing();
            drain();
        } catch (...) {
            std::terminate();
        }
        gpu_.pipeline_enabled = false;
    }

    void TwoFramePipeline::poll() {
        ++polls;
        while (count_) {
            auto& slot = slots_[head_];
            BOOL done = FALSE;
            const auto result =
                gpu_.immediate->GetData(slot.done.Get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            throw_if_failed(result, "pipeline nonblocking completion query");
            if (result != S_OK || !done) {
                break;
            }
            double gpu_ms{};
            const auto serial = slot.serial;
            if (slot.ui && slot.timed) {
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT data{};
                UINT64 start{};
                UINT64 end{};
                ensure(gpu_.immediate->GetData(slot.disjoint.Get(), &data, sizeof(data),
                                               D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                           !data.Disjoint && data.Frequency,
                       "pipeline stable GPU timing");
                ensure(gpu_.immediate->GetData(slot.start.Get(), &start, sizeof(start),
                                               D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                           gpu_.immediate->GetData(slot.end.Get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) ==
                               S_OK &&
                           end >= start,
                       "pipeline completed GPU timestamps");
                gpu_ms = double(end - start) * weft::constants::milliseconds_per_second / data.Frequency;
            }
            render_result(weft_renderer_notify_completed(backend_.renderer, limits::device_epoch,
                                                         limits::immediate_queue, serial),
                          "pipeline host-confirmed completion");
            slot.commands.Reset();
            slot.host_lease.reset();
            slot.serial = 0;
            head_ = (head_ + 1) % limits::frames_in_flight;
            --count_;
            gpu_.async_in_flight = count_;
            ++completions;
            completed_serial = serial;
            if (completed_timing && slot.timed) {
                completed_timing(serial, gpu_ms);
            }
        }
    }

    void TwoFramePipeline::set_timing(bool enabled) {
        timing_enabled_ = enabled;
    }

    void TwoFramePipeline::begin_frame_timing() {
        ensure(!timed_slot_, "one timed producer batch");
        timed_slot_ = &reserve();
        timed_slot_->timed = timing_enabled_ && bool(timed_slot_->disjoint);
        if (timed_slot_->timed) {
            gpu_.immediate->Begin(timed_slot_->disjoint.Get());
            gpu_.immediate->End(timed_slot_->start.Get());
        }
    }

    void TwoFramePipeline::cancel_frame_timing() {
        if (timed_slot_ && timed_slot_->timed) {
            gpu_.immediate->End(timed_slot_->disjoint.Get());
        }
        timed_slot_ = nullptr;
    }

    void TwoFramePipeline::drain() {
        ++drains;
        const auto start = clock();
        while (count_) {
            wait_oldest();
        }
        drain_wait_ms += elapsed(start);
    }
} // namespace weft::render::d3d11
