#include <caseelephant/ce.h>
#include <caseelephant/constants/core.hpp>

#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <thread>
#include <vector>

namespace {
    std::atomic<uint64_t> next_runtime_id{1};

    struct ViewSlot {
        uint32_t generation{1};
        bool live{false};
        bool retired{false};
        ce_view_info info{};
    };
} // namespace

struct ce_runtime {
    std::thread::id owner{std::this_thread::get_id()};
    uint64_t id{};
    uint32_t live_views{};
    std::vector<ViewSlot> slots;
};

namespace {
    ce_result validate_owner(ce_runtime* runtime) noexcept {
        if (!runtime) {
            return CE_INVALID_ARGUMENT;
        }
        return runtime->owner == std::this_thread::get_id() ? CE_OK : CE_WRONG_THREAD;
    }

    ce_result find_view(ce_runtime* runtime, ce_view_handle handle, ViewSlot*& out) noexcept {
        if (const auto error = validate_owner(runtime); error != CE_OK) {
            return error;
        }
        if (handle.runtime_id != runtime->id || handle.slot == 0 || handle.slot > runtime->slots.size()) {
            return CE_STALE_HANDLE;
        }
        auto& slot = runtime->slots[handle.slot - 1];
        if (!slot.live || slot.generation != handle.generation) {
            return CE_STALE_HANDLE;
        }
        out = &slot;
        return CE_OK;
    }

    bool valid_view_desc(const ce_view_desc& desc) noexcept {
        return desc.struct_size >= sizeof(ce_view_desc) && desc.reserved == 0 && desc.reserved2 == 0 &&
               std::isfinite(desc.logical_width) && desc.logical_width > 0 && std::isfinite(desc.logical_height) &&
               desc.logical_height > 0 && std::isfinite(desc.device_scale) && desc.device_scale > 0;
    }
} // namespace

extern "C" ce_result ce_runtime_create(const ce_runtime_desc* desc, ce_runtime** out_runtime) {
    if (!out_runtime) {
        return CE_INVALID_ARGUMENT;
    }
    *out_runtime = nullptr;
    uint32_t max_views = caseelephant::constants::core::default_max_views;
    if (desc) {
        if (desc->struct_size < sizeof(ce_runtime_desc)) {
            return CE_INVALID_ARGUMENT;
        }
        if (desc->abi_version != CE_ABI_VERSION) {
            return CE_UNSUPPORTED_ABI;
        }
        if (desc->reserved != 0 || desc->max_views == 0 ||
            desc->max_views > caseelephant::constants::core::hard_max_views) {
            return CE_INVALID_ARGUMENT;
        }
        max_views = desc->max_views;
    }
    try {
        auto runtime = std::make_unique<ce_runtime>();
        auto id = next_runtime_id.load(std::memory_order_relaxed);
        for (;;) {
            if (id == std::numeric_limits<uint64_t>::max()) {
                return CE_CAPACITY_EXCEEDED;
            }
            if (next_runtime_id.compare_exchange_weak(id, id + 1, std::memory_order_relaxed)) {
                break;
            }
        }
        runtime->id = id;
        runtime->slots.resize(max_views);
        *out_runtime = runtime.release();
        return CE_OK;
    } catch (const std::bad_alloc&) {
        return CE_OUT_OF_MEMORY;
    } catch (...) {
        return CE_INTERNAL_ERROR;
    }
}

extern "C" ce_result ce_runtime_destroy(ce_runtime* runtime) {
    if (const auto error = validate_owner(runtime); error != CE_OK) {
        return error;
    }
    if (runtime->live_views != 0) {
        return CE_BUSY;
    }
    delete runtime;
    return CE_OK;
}

extern "C" ce_result ce_runtime_get_info(ce_runtime* runtime, ce_runtime_info* out_info) {
    if (const auto error = validate_owner(runtime); error != CE_OK) {
        return error;
    }
    if (!out_info || out_info->struct_size < sizeof(ce_runtime_info)) {
        return CE_INVALID_ARGUMENT;
    }
    *out_info = {sizeof(ce_runtime_info), CE_ABI_VERSION, runtime->id, runtime->live_views,
                 static_cast<uint32_t>(runtime->slots.size())};
    return CE_OK;
}

extern "C" ce_result ce_view_create(ce_runtime* runtime, const ce_view_desc* desc, ce_view_handle* out_view) {
    if (!out_view) {
        return CE_INVALID_ARGUMENT;
    }
    *out_view = {};
    if (const auto error = validate_owner(runtime); error != CE_OK) {
        return error;
    }
    const ce_view_desc defaults{sizeof(ce_view_desc),
                                0,
                                caseelephant::constants::core::default_view_logical_width,
                                caseelephant::constants::core::default_view_logical_height,
                                1,
                                0};
    if (!desc) {
        desc = &defaults;
    }
    if (desc->struct_size < sizeof(ce_view_desc) || !valid_view_desc(*desc)) {
        return CE_INVALID_ARGUMENT;
    }
    for (uint32_t i = 0; i < runtime->slots.size(); ++i) {
        auto& slot = runtime->slots[i];
        if (slot.live || slot.retired) {
            continue;
        }
        slot.info = {sizeof(ce_view_info), 0, desc->logical_width, desc->logical_height, desc->device_scale, 0, 0, 0};
        slot.live = true;
        ++runtime->live_views;
        *out_view = {runtime->id, i + 1, slot.generation};
        return CE_OK;
    }
    return CE_CAPACITY_EXCEEDED;
}

extern "C" ce_result ce_view_destroy(ce_runtime* runtime, ce_view_handle view) {
    ViewSlot* slot{};
    if (const auto error = find_view(runtime, view, slot); error != CE_OK) {
        return error;
    }
    slot->live = false;
    --runtime->live_views;
    if (slot->generation == std::numeric_limits<uint32_t>::max()) {
        slot->retired = true;
    } else {
        ++slot->generation;
    }
    return CE_OK;
}

extern "C" ce_result ce_view_get_info(ce_runtime* runtime, ce_view_handle view, ce_view_info* out_info) {
    ViewSlot* slot{};
    if (const auto error = find_view(runtime, view, slot); error != CE_OK) {
        return error;
    }
    if (!out_info || out_info->struct_size < sizeof(ce_view_info)) {
        return CE_INVALID_ARGUMENT;
    }
    *out_info = slot->info;
    return CE_OK;
}

extern "C" ce_result ce_view_record_tick(ce_runtime* runtime, ce_view_handle view, double time_seconds) {
    ViewSlot* slot{};
    if (const auto error = find_view(runtime, view, slot); error != CE_OK) {
        return error;
    }
    if (!std::isfinite(time_seconds) || time_seconds < 0) {
        return CE_INVALID_ARGUMENT;
    }
    if (slot->info.tick_count != 0 && time_seconds < slot->info.last_time_seconds) {
        return CE_TIME_REGRESSION;
    }
    if (slot->info.tick_count == std::numeric_limits<uint64_t>::max()) {
        return CE_CAPACITY_EXCEEDED;
    }
    ++slot->info.tick_count;
    slot->info.last_time_seconds = time_seconds;
    return CE_OK;
}

extern "C" const char* ce_result_string(ce_result result) {
    switch (result) {
        case CE_OK:
            return "ok";
        case CE_INVALID_ARGUMENT:
            return "invalid argument";
        case CE_UNSUPPORTED_ABI:
            return "unsupported ABI";
        case CE_OUT_OF_MEMORY:
            return "out of memory";
        case CE_WRONG_THREAD:
            return "wrong owner thread";
        case CE_BUSY:
            return "resources still live";
        case CE_STALE_HANDLE:
            return "stale handle";
        case CE_CAPACITY_EXCEEDED:
            return "capacity exceeded";
        case CE_TIME_REGRESSION:
            return "host time regressed";
        case CE_INTERNAL_ERROR:
            return "internal error";
        default:
            return "unknown result";
    }
}
