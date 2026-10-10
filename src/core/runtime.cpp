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

struct ce_runtime {
    caseelephant::OwnerThread owner;
    uint64_t id{};
    uint32_t live_views{};
    // View handles carry slot + 1, so a zeroed handle is never valid.
    caseelephant::GenerationalSlots<ce_view_info> views;
};

namespace {
    ce_result validate_owner(ce_runtime* runtime) noexcept {
        if (!runtime) {
            return CE_INVALID_ARGUMENT;
        }
        return runtime->owner.is_current() ? CE_OK : CE_WRONG_THREAD;
    }

    ce_result find_view(ce_runtime* runtime, ce_view_handle handle, ce_view_info*& out) noexcept {
        if (const auto error = validate_owner(runtime); error != CE_OK) {
            return error;
        }
        if (handle.runtime_id != runtime->id || handle.slot == 0) {
            return CE_STALE_HANDLE;
        }
        out = runtime->views.find(handle.slot - 1, handle.generation);
        return out ? CE_OK : CE_STALE_HANDLE;
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
        runtime->views = caseelephant::GenerationalSlots<ce_view_info>(max_views);
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
                 static_cast<uint32_t>(runtime->views.size())};
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
    const auto index = runtime->views.first_free();
    if (index == runtime->views.size()) {
        return CE_CAPACITY_EXCEEDED;
    }
    runtime->views[index] = {
        sizeof(ce_view_info), 0, desc->logical_width, desc->logical_height, desc->device_scale, 0, 0, 0};
    const auto generation = runtime->views.occupy(index);
    ++runtime->live_views;
    *out_view = {runtime->id, static_cast<uint32_t>(index + 1), generation};
    return CE_OK;
}

extern "C" ce_result ce_view_destroy(ce_runtime* runtime, ce_view_handle view) {
    ce_view_info* info{};
    if (const auto error = find_view(runtime, view, info); error != CE_OK) {
        return error;
    }
    runtime->views.vacate(view.slot - 1);
    --runtime->live_views;
    return CE_OK;
}

extern "C" ce_result ce_view_get_info(ce_runtime* runtime, ce_view_handle view, ce_view_info* out_info) {
    ce_view_info* info{};
    if (const auto error = find_view(runtime, view, info); error != CE_OK) {
        return error;
    }
    if (!out_info || out_info->struct_size < sizeof(ce_view_info)) {
        return CE_INVALID_ARGUMENT;
    }
    *out_info = *info;
    return CE_OK;
}

extern "C" ce_result ce_view_record_tick(ce_runtime* runtime, ce_view_handle view, double time_seconds) {
    ce_view_info* info{};
    if (const auto error = find_view(runtime, view, info); error != CE_OK) {
        return error;
    }
    if (!std::isfinite(time_seconds) || time_seconds < 0) {
        return CE_INVALID_ARGUMENT;
    }
    if (info->tick_count != 0 && time_seconds < info->last_time_seconds) {
        return CE_TIME_REGRESSION;
    }
    if (info->tick_count == std::numeric_limits<uint64_t>::max()) {
        return CE_CAPACITY_EXCEEDED;
    }
    ++info->tick_count;
    info->last_time_seconds = time_seconds;
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
