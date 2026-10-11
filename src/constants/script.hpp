#pragma once

#include <cstddef>

#include "constants/common.hpp"

// Budgets, limits and Web constants of the QuickJS VM, the native DOM and the owner executor.
// Each constant has the type of the literal it replaced (see docs/coding-style.md).
namespace weft::constants::script {
    // QuickJS VM. The host must reserve native stack headroom above the VM stack limit.
    inline constexpr std::size_t vm_memory_limit = 64 * constants::bytes_per_mib;
    inline constexpr int default_vm_stack_bytes = 512 * constants::bytes_per_kib;
    inline constexpr int min_vm_stack_bytes = 128 * constants::bytes_per_kib;
    inline constexpr int max_vm_stack_bytes = 2 * constants::bytes_per_mib;
    // VM stack used by probes and tests that run original application bundles.
    inline constexpr int probe_vm_stack_bytes = constants::bytes_per_mib;
    // Wall-time budget shared by one eval or host batch and its microtask checkpoints.
    inline constexpr int default_execution_budget_ms = 2000;
    inline constexpr int min_execution_budget_ms = 10;
    inline constexpr int max_execution_budget_ms = 30000;
    // Microtask jobs one checkpoint may run before the View faults.
    inline constexpr int max_microtask_jobs = 10000;

    // Native DOM bounds. They are independent of the smaller layout document profile.
    inline constexpr int max_live_nodes = 32768;
    inline constexpr int max_native_dom_depth = 256;
    inline constexpr int max_listeners_per_node = 1024;
    // Ring journal of recent mutations; an older cursor requires a full snapshot.
    inline constexpr int dom_change_journal_capacity = 1024;
    // The last listener exception kept in the stats is truncated to this many bytes.
    inline constexpr int max_listener_error_bytes = 8192;

    // Significant digits of a translucent fillStyle alpha in its rgba() serialization.
    inline constexpr int canvas_alpha_serialization_digits = 6;

    // Event.eventPhase values.
    inline constexpr int event_phase_none = 0;
    inline constexpr int event_phase_capturing = 1;
    inline constexpr int event_phase_at_target = 2;
    inline constexpr int event_phase_bubbling = 3;
    // Node.compareDocumentPosition() bits.
    inline constexpr int document_position_disconnected = 1;
    inline constexpr int document_position_preceding = 2;
    inline constexpr int document_position_following = 4;
    inline constexpr int document_position_contains = 8;
    inline constexpr int document_position_contained_by = 16;
    inline constexpr int document_position_implementation_specific = 32;
    // WheelEvent.DOM_DELTA_PIXEL: host wheel deltas are logical pixels.
    inline constexpr int wheel_delta_mode_pixel = 0;
    // The single mouse pointer of the M0 input profile.
    inline constexpr int mouse_pointer_id = 1;
    // MouseEvent.buttons bit of the primary button, the only button the M0 profile accepts.
    inline constexpr int primary_mouse_buttons_mask = 1;

    // Host geometry and wheel input: largest accepted magnitude, in logical px.
    inline constexpr int max_logical_pixel_magnitude = constants::max_logical_px_magnitude;
    inline constexpr double max_geometry_matrix_component = 1e7;
    // Client rect fragments per element.
    inline constexpr int max_client_rects = 65536;
    // Computed-style host replies and the owner cache of them.
    inline constexpr int max_computed_style_properties = 512;
    inline constexpr int max_computed_style_name_bytes = 128;
    inline constexpr int max_computed_style_value_bytes = 4096;
    inline constexpr int computed_style_cache_max_entries = 128;
    inline constexpr int computed_style_cache_max_bytes = 2 * constants::bytes_per_mib;
    // Number.MAX_SAFE_INTEGER; the integer format host only receives exact integers.
    inline constexpr double max_safe_integer = 9007199254740991.;
    inline constexpr int max_formatted_integer_bytes = 128;

    // Contract with probes/web_host_bootstrap.js and probes/animation_frame_bootstrap.js.
    // Ready callbacks in one timer or animation-frame batch.
    inline constexpr int max_host_batch_callbacks = 1000;
    // Pending timers or animation-frame callbacks.
    inline constexpr int max_host_pending_callbacks = 4096;
    // pumpNative() returns callbacks * radix + ready; ready never exceeds the pending capacity.
    inline constexpr int host_task_pack_radix = max_host_pending_callbacks + 1;
    // Ready-timer turns a host runs per opportunity before it reports a runaway timer chain.
    inline constexpr int max_task_pump_turns = 32;

    // OwnerExecutor reliable input queue and publication wait.
    inline constexpr int default_owner_input_capacity = 256;
    inline constexpr int max_owner_input_capacity = 4096;
    // Publications kept for the host: the newest frame and the one before it.
    inline constexpr int max_queued_publications = 2;
    inline constexpr int default_owner_wait_seconds = 5;
    inline constexpr int max_owner_wait_seconds = 60;

} // namespace weft::constants::script
