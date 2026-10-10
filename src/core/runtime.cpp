#include <caseelephant/ce.h>
#include <caseelephant/base/generational_slots.hpp>
#include <caseelephant/base/owner_thread.hpp>
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
} // namespace

struct weft_runtime {
    weft::OwnerThread owner;
    uint64_t id{};
    uint32_t live_views{};
    // View handles carry slot + 1, so a zeroed handle is never valid.
    weft::GenerationalSlots<weft_view_info> views;
};

namespace {
    weft_result validate_owner(weft_runtime* runtime) noexcept {
        if (!runtime) {
            return WEFT_INVALID_ARGUMENT;
        }
        return runtime->owner.is_current() ? WEFT_OK : WEFT_WRONG_THREAD;
    }

    weft_result find_view(weft_runtime* runtime, weft_view_handle handle, weft_view_info*& out) noexcept {
        if (const auto error = validate_owner(runtime); error != WEFT_OK) {
            return error;
        }
        if (handle.runtime_id != runtime->id || handle.slot == 0) {
            return WEFT_STALE_HANDLE;
        }
        out = runtime->views.find(handle.slot - 1, handle.generation);
        return out ? WEFT_OK : WEFT_STALE_HANDLE;
    }

    bool valid_view_desc(const weft_view_desc& desc) noexcept {
        return desc.struct_size >= sizeof(weft_view_desc) && desc.reserved == 0 && desc.reserved2 == 0 &&
               std::isfinite(desc.logical_width) && desc.logical_width > 0 && std::isfinite(desc.logical_height) &&
               desc.logical_height > 0 && std::isfinite(desc.device_scale) && desc.device_scale > 0;
    }
} // namespace

extern "C" weft_result weft_runtime_create(const weft_runtime_desc* desc, weft_runtime** out_runtime) {
    if (!out_runtime) {
        return WEFT_INVALID_ARGUMENT;
    }
    *out_runtime = nullptr;
    uint32_t max_views = weft::constants::core::default_max_views;
    if (desc) {
        if (desc->struct_size < sizeof(weft_runtime_desc)) {
            return WEFT_INVALID_ARGUMENT;
        }
        if (desc->abi_version != WEFT_ABI_VERSION) {
            return WEFT_UNSUPPORTED_ABI;
        }
        if (desc->reserved != 0 || desc->max_views == 0 || desc->max_views > weft::constants::core::hard_max_views) {
            return WEFT_INVALID_ARGUMENT;
        }
        max_views = desc->max_views;
    }
    try {
        auto runtime = std::make_unique<weft_runtime>();
        auto id = next_runtime_id.load(std::memory_order_relaxed);
        for (;;) {
            if (id == std::numeric_limits<uint64_t>::max()) {
                return WEFT_CAPACITY_EXCEEDED;
            }
            if (next_runtime_id.compare_exchange_weak(id, id + 1, std::memory_order_relaxed)) {
                break;
            }
        }
        runtime->id = id;
        runtime->views = weft::GenerationalSlots<weft_view_info>(max_views);
        *out_runtime = runtime.release();
        return WEFT_OK;
    } catch (const std::bad_alloc&) {
        return WEFT_OUT_OF_MEMORY;
    } catch (...) {
        return WEFT_INTERNAL_ERROR;
    }
}

extern "C" weft_result weft_runtime_destroy(weft_runtime* runtime) {
    if (const auto error = validate_owner(runtime); error != WEFT_OK) {
        return error;
    }
    if (runtime->live_views != 0) {
        return WEFT_BUSY;
    }
    delete runtime;
    return WEFT_OK;
}

extern "C" weft_result weft_runtime_get_info(weft_runtime* runtime, weft_runtime_info* out_info) {
    if (const auto error = validate_owner(runtime); error != WEFT_OK) {
        return error;
    }
    if (!out_info || out_info->struct_size < sizeof(weft_runtime_info)) {
        return WEFT_INVALID_ARGUMENT;
    }
    *out_info = {sizeof(weft_runtime_info), WEFT_ABI_VERSION, runtime->id, runtime->live_views,
                 static_cast<uint32_t>(runtime->views.size())};
    return WEFT_OK;
}

extern "C" weft_result weft_view_create(weft_runtime* runtime, const weft_view_desc* desc, weft_view_handle* out_view) {
    if (!out_view) {
        return WEFT_INVALID_ARGUMENT;
    }
    *out_view = {};
    if (const auto error = validate_owner(runtime); error != WEFT_OK) {
        return error;
    }
    const weft_view_desc defaults{sizeof(weft_view_desc),
                                  0,
                                  weft::constants::core::default_view_logical_width,
                                  weft::constants::core::default_view_logical_height,
                                  1,
                                  0};
    if (!desc) {
        desc = &defaults;
    }
    if (desc->struct_size < sizeof(weft_view_desc) || !valid_view_desc(*desc)) {
        return WEFT_INVALID_ARGUMENT;
    }
    const auto index = runtime->views.first_free();
    if (index == runtime->views.size()) {
        return WEFT_CAPACITY_EXCEEDED;
    }
    runtime->views[index] = {
        sizeof(weft_view_info), 0, desc->logical_width, desc->logical_height, desc->device_scale, 0, 0, 0};
    const auto generation = runtime->views.occupy(index);
    ++runtime->live_views;
    *out_view = {runtime->id, static_cast<uint32_t>(index + 1), generation};
    return WEFT_OK;
}

extern "C" weft_result weft_view_destroy(weft_runtime* runtime, weft_view_handle view) {
    weft_view_info* info{};
    if (const auto error = find_view(runtime, view, info); error != WEFT_OK) {
        return error;
    }
    runtime->views.vacate(view.slot - 1);
    --runtime->live_views;
    return WEFT_OK;
}

extern "C" weft_result weft_view_get_info(weft_runtime* runtime, weft_view_handle view, weft_view_info* out_info) {
    weft_view_info* info{};
    if (const auto error = find_view(runtime, view, info); error != WEFT_OK) {
        return error;
    }
    if (!out_info || out_info->struct_size < sizeof(weft_view_info)) {
        return WEFT_INVALID_ARGUMENT;
    }
    *out_info = *info;
    return WEFT_OK;
}

extern "C" weft_result weft_view_record_tick(weft_runtime* runtime, weft_view_handle view, double time_seconds) {
    weft_view_info* info{};
    if (const auto error = find_view(runtime, view, info); error != WEFT_OK) {
        return error;
    }
    if (!std::isfinite(time_seconds) || time_seconds < 0) {
        return WEFT_INVALID_ARGUMENT;
    }
    if (info->tick_count != 0 && time_seconds < info->last_time_seconds) {
        return WEFT_TIME_REGRESSION;
    }
    if (info->tick_count == std::numeric_limits<uint64_t>::max()) {
        return WEFT_CAPACITY_EXCEEDED;
    }
    ++info->tick_count;
    info->last_time_seconds = time_seconds;
    return WEFT_OK;
}

extern "C" const char* weft_result_string(weft_result result) {
    switch (result) {
        case WEFT_OK:
            return "ok";
        case WEFT_INVALID_ARGUMENT:
            return "invalid argument";
        case WEFT_UNSUPPORTED_ABI:
            return "unsupported ABI";
        case WEFT_OUT_OF_MEMORY:
            return "out of memory";
        case WEFT_WRONG_THREAD:
            return "wrong owner thread";
        case WEFT_BUSY:
            return "resources still live";
        case WEFT_STALE_HANDLE:
            return "stale handle";
        case WEFT_CAPACITY_EXCEEDED:
            return "capacity exceeded";
        case WEFT_TIME_REGRESSION:
            return "host time regressed";
        case WEFT_INTERNAL_ERROR:
            return "internal error";
        default:
            return "unknown result";
    }
}
