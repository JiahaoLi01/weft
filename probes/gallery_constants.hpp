#pragma once

#include "constants/common.hpp"
#include "constants/scene.hpp"
#include "constants/script.hpp"
#include "probe_constants.hpp"
#include "../tests/test_constants/support.hpp"

// Values shared by the gallery and scene GPU probes (probes/gallery_*, probes/scene_*, complex_css_probe.cpp
// and render_stages_probe.cpp) and the scene tests.
//
// Each constant has the type of the literals it replaced (a plain integer literal is int), so
// arithmetic, comparisons and generated code at every use site stay exactly as before.
namespace weft::probe_constants::gallery {
    // --- Gallery inventory (examples/crpg-gallery/native/case-list.js) ---

    // Entries and sections of the fixed gallery catalogue.
    inline constexpr int catalog_case_count = 72;
    inline constexpr int catalog_group_count = 10;
    // CSS_TESTS and D3_TESTS scenes come first in the scene manifest; cluster scenes follow them.
    inline constexpr int css_scene_count = 26;
    inline constexpr int d3_scene_count = 8;
    inline constexpr int core_scene_count = css_scene_count + d3_scene_count;
    // Original WEBGL_TESTS Canvas cases hosted natively.
    inline constexpr int canvas_case_count = 2;
    // Authored card sizes of the CSS_TESTS and D3_TESTS groups, in shell pixels.
    inline constexpr int css_card_width = 387;
    inline constexpr int css_card_height = 288;
    inline constexpr int d3_card_size = 384;

    // Scene manifest indices driven by scripted audits, benchmarks and scroll tests.
    inline constexpr int css_hover_scene = 4;
    inline constexpr int css_toggle_scene = 7;
    // D3_INTERACTIVE_TESTS 0, the largest card and the first cluster scene.
    inline constexpr int sunburst_scene = 34;
    // Zoomable SVG scatter plot; it receives wheel input instead of the shell scroller.
    inline constexpr int scatter_scene = 37;
    inline constexpr int kline_scene = 38;
    inline constexpr int anime_drag_scene = 58;

    // --- Script hosts ---

    // ScriptDom native stack reservations. Cluster scenes load larger original bundles.
    inline constexpr int vm_stack_bytes = weft::constants::bytes_per_mib;
    inline constexpr int cluster_vm_stack_bytes = weft::constants::script::max_vm_stack_bytes;
    // Wall-time budget per eval for the shell, registry and Canvas hosts, and for original scenes.
    inline constexpr int host_vm_execution_budget_ms = 5000;
    inline constexpr int scene_vm_execution_budget_ms = weft::constants::script::max_execution_budget_ms;
    // Ready-task turns pumped per tick before the host reports a runaway task loop.
    inline constexpr int task_pump_turn_limit = weft::constants::script::max_task_pump_turns;
    // Live native nodes left once an original case has unmounted and its document was reset.
    inline constexpr int unmounted_live_nodes = 3;
    // Client coordinate outside every card; moving there ends hover on the previous card.
    inline constexpr int offscreen_pointer = -10;
    // GetNumberFormatEx output buffer, in UTF-16 code units, and the largest accepted code unit.
    inline constexpr int number_format_buffer_chars = 128;
    inline constexpr int max_ascii_code_unit = 127;

    // --- Clocks and sampling ---

    // Deterministic runs advance a fixed 60 Hz clock.
    inline constexpr int fixed_clock_hz = 60;
    inline constexpr double fixed_clock_step_ms = 1000. / fixed_clock_hz;
    // Fixed-clock benchmark and interaction profile length, and the frames dropped before warm statistics.
    inline constexpr int fixed_clock_run_frames = 240;
    inline constexpr int warmup_frames = 16;
    inline constexpr int fixed_clock_warm_frames = fixed_clock_run_frames - warmup_frames;
    // An owner tick longer than one 60 Hz frame counts as a stall.
    inline constexpr double stall_threshold_ms = 16.666667;
    // First timestamp given to a gallery owner or a Canvas session.
    inline constexpr double initial_clock_ms = .25;
    // Offset added to host timestamps so that they never coincide with a rounded timer value.
    inline constexpr double host_clock_offset_ms = .2500006;
    // Isolated catalogue observation: animation opportunities per case and their irregular step.
    inline constexpr int observation_frames = 12;
    inline constexpr double observation_step_ms = 16.123456789;
    // weft_gallery_probe (probes/gallery_probe.cpp) observes the same opportunities on a regular 16 ms step;
    // the exactly representable fraction keeps timestamps off rounded timer values.
    inline constexpr int native_observation_frame_ms = 16;
    inline constexpr double native_observation_time_fraction_ms = 0.25;
    // Sliding window of retained samples per series.
    inline constexpr int sample_window = 600;
    // HUD and live profiler plots are redrawn at most five times per second.
    inline constexpr double hud_refresh_interval_s = .2;
    inline constexpr int microseconds_per_millisecond = weft::test_constants::support::microseconds_per_millisecond;

    // --- Owner thread and traces ---

    // The inspection view uses one owner epoch; a reset creates a fresh executor instead.
    inline constexpr int owner_epoch = 1;
    // Chrome trace thread ids of the UI owner and the render thread.
    inline constexpr int owner_trace_tid = weft::test_constants::support::owner_trace_tid;
    inline constexpr int render_trace_tid = 2;
    // Hidden-scene pumps shorter than this are not traced.
    inline constexpr double hidden_pump_trace_threshold_ms = .25;
    // Largest mouse button mask (five buttons) and test-only owner stall accepted by the owner.
    inline constexpr int max_mouse_buttons_mask = 31;
    inline constexpr int max_test_delay_ms = 50;
    // Host waits for a requested owner publication.
    inline constexpr int owner_wait_s = 5;
    inline constexpr int owner_self_test_wait_s = 60;

    // --- Inspection window ---

    // Authored inspection shell (examples/crpg-gallery/preview.css), shown at 2x on the 4K surface.
    inline constexpr int shell_width = 1920;
    inline constexpr int shell_height = 1080;
    inline constexpr int shell_device_scale = 2;
    // Visible band of the shell scroller between its header and footer, in shell pixels.
    inline constexpr int shell_viewport_top = 104;
    inline constexpr int shell_viewport_bottom = 1020;
    // Original Canvas cases choose an 800x600 backing.
    inline constexpr int canvas_backing_width = 800;
    inline constexpr int canvas_backing_height = 600;
    // Sleep while the window is minimized and while the preview is not visible.
    inline constexpr int minimized_poll_ms = 10;
    inline constexpr int hidden_frame_sleep_ms = weft::probe_constants::idle_wait_ms;
    // WM_KEYDOWN lParam bit 30: the key was already down (auto-repeat).
    inline constexpr long long key_repeat_flag = 1LL << 30;
    // Wheel lines per notch configured for the scroll regression.
    inline constexpr int scroll_test_wheel_lines = weft::probe_constants::default_wheel_scroll_lines;
    // Asset cache budget of the 4K preview renderer.
    inline constexpr int asset_cache_budget_bytes = 512 * weft::constants::bytes_per_mib;

    // --- Live profiler HUD ---

    // Plotted history and its horizontal resolution.
    inline constexpr int profiler_plot_seconds = 5;
    inline constexpr int profiler_plot_buckets = 128;
    // Reference lines in milliseconds: a low line, one 120 Hz frame and one 60 Hz frame.
    inline constexpr double profiler_low_line_ms = 2.;
    inline constexpr double profiler_120hz_frame_ms = 8.33;
    inline constexpr double profiler_60hz_frame_ms = 16.67;
    // The plot scale is rounded up to a multiple of this many milliseconds.
    inline constexpr int profiler_scale_step_ms = 8;
    // Most expensive cases listed after the last owner update.
    inline constexpr int profiler_hot_case_count = 3;

    // --- Pixel oracles ---

    // Largest channel difference accepted between GPU output and the CPU packet oracle.
    inline constexpr int max_cpu_oracle_difference = weft::probe_constants::cpu_oracle_tolerance;
    // 4K preview oracle grid: starts at 17 px with prime strides and stops left of the HUD.
    inline constexpr int oracle_grid_origin = 17;
    inline constexpr int oracle_grid_stride_x = 211;
    inline constexpr int oracle_grid_stride_y = 193;
    inline constexpr int oracle_grid_right = 2900;
    // Canvas probe oracle grid strides.
    inline constexpr int canvas_oracle_stride_x = 13;
    inline constexpr int canvas_oracle_stride_y = 11;
    // Declared Chromium difference limits for original Canvas bitmaps (RGBA channel order).
    inline constexpr int canvas_max_channel_difference = 85;
    inline constexpr int canvas_max_mean_difference_r = 9;
    inline constexpr double canvas_max_mean_difference_g = .4;
    inline constexpr double canvas_max_mean_difference_b = .4;
    inline constexpr double canvas_max_mean_difference_a = .03;
    inline constexpr int canvas_max_bright_mean_difference = 6;
    inline constexpr int canvas_min_bright_pixels = 500;
    inline constexpr double canvas_min_bright_iou = .90;
    // A pixel is bright when one of its color channels exceeds this value.
    inline constexpr int canvas_bright_channel_threshold = 40;

    // --- Canvas probe run ---

    inline constexpr int canvas_frames_per_case = 64;
    inline constexpr int canvas_frame_step_ms = 16;
    // First timestamp after the recorded frames, used to check that unmount cancels animation.
    inline constexpr double canvas_unmount_time_ms = 1024.25;

    // --- Scene GPU surfaces ---

    // Bounded versions per card: three allow two UI submissions in flight.
    inline constexpr int surface_versions_per_card = 3;
    // External texture imports accepted by the renderer backend: count and byte budget.
    inline constexpr int external_texture_limit = weft::probe_constants::max_imported_textures;
    inline constexpr unsigned external_import_budget_bytes = 512u * weft::constants::bytes_per_mib;
    // External texture ids are this base plus the scene index.
    inline constexpr int external_texture_id_base = 1000;
    // Largest card raster scale and surface area (4096 x 4096 pixels).
    inline constexpr int max_surface_scale = 4;
    inline constexpr int max_surface_pixels = 16777216;
    // Default device scale of scene card rasters ("CSS / SVG GPU at 2x").
    inline constexpr int scene_raster_scale = 2;
    // Deadline and polling period for retiring a producer-only submission.
    inline constexpr int producer_completion_timeout_s = weft::probe_constants::completion_timeout_seconds;
    inline constexpr int completion_poll_ms = weft::probe_constants::completion_poll_ms;
    // MaterialEncodedBgra in src/render/backends/d3d11/quad.hlsl: decode the encoded BGRA card target.
    inline constexpr int encoded_bgra_material = static_cast<int>(weft::probe_constants::shader_material::encoded_bgra);
    // Two triangles per instanced quad.
    inline constexpr int quad_vertex_count = weft::probe_constants::quad_vertex_count;
    // Adds half the divisor before dividing by color_channel_max, rounding to nearest.
    inline constexpr int color_channel_rounding = weft::constants::color_channel_max / 2;

    // --- Native scene shader (shaders/scene_native_probe.hlsl) ---

    // These mirror the static consts at the top of shaders/scene_native_probe.hlsl.
    // Record::extra.z / extra.w flags: FlagEnabled, and its disabled counterpart.
    inline constexpr float flag_enabled = 1.f;
    inline constexpr float flag_disabled = 0.f;
    // MaxGradientStops and StopsPerOffsetVector.
    inline constexpr int max_gradient_stops = weft::constants::scene::max_gradient_stops;
    inline constexpr int stops_per_offset_vector = 4;
    inline constexpr int gradient_offset_vectors = max_gradient_stops / stops_per_offset_vector;
    // Size of the shader Paint record (24 float4 registers).
    inline constexpr int native_record_bytes = 384;
    // Size of the View constant buffer (one float4).
    inline constexpr int native_view_constants_bytes = 16;
    inline constexpr int native_record_initial_capacity = 16;
    // Nested transparent groups available per card.
    inline constexpr int native_max_group_depth = 4;
    // Text and layer sources keep a transparent margin around the node on each side.
    inline constexpr int text_source_margin = 2;
    inline constexpr int text_source_margins = 2 * text_source_margin;
    // Instance bounds grow by this many device pixels on each side for antialiasing.
    inline constexpr int record_bounds_outset = 2;
    inline constexpr int record_bounds_outsets = 2 * record_bounds_outset;
    // Initial extent of the bounds accumulation.
    inline constexpr float record_bounds_sentinel = 1e10f;
    // Overflow tolerances for text metrics and for layer descendants, in CSS pixels.
    inline constexpr float text_fit_tolerance = .01f;
    inline constexpr double layer_bounds_tolerance = .01;
    // Text/layer image cache: entry cap and byte budget. A single source must fit in the budget.
    inline constexpr int native_image_cache_max_entries = 128;
    inline constexpr int native_image_cache_budget_bytes = 16 * weft::constants::bytes_per_mib;
    // Transparent-group scratch targets across all cards.
    inline constexpr int native_scratch_budget_bytes = 128 * weft::constants::bytes_per_mib;

    // --- GPU scene tests ---

    // Surface of the per-card GPU tests (run_scene_gpu_test, run_gallery_cluster_gpu).
    inline constexpr int scene_test_surface_size = 800;
    // Last frame with a browser reference: 121 CSS/D3 frames, 301 cluster frames.
    inline constexpr int scene_reference_last_frame = 120;
    inline constexpr int cluster_reference_last_frame = 300;
    // run_scene_surface_retirement: surface size and transparent-group re-entry visits, more than
    // the 128 MiB scratch budget holds at 1 MiB each.
    inline constexpr int retirement_surface_size = 80;
    inline constexpr int scratch_reentry_visits = 140;
    // run_complex_css_test: scene, its 2x surface, and the length of each phase.
    inline constexpr int complex_css_scene_width = 180;
    inline constexpr int complex_css_scene_height = 140;
    inline constexpr int complex_css_surface_width = 360;
    inline constexpr int complex_css_surface_height = 280;
    inline constexpr int complex_css_projective_frames = 64;
    inline constexpr int complex_css_border_frames = 96;
    inline constexpr int complex_css_angle_frames = 24;
    inline constexpr int complex_css_typed_frames = 128;
    inline constexpr int complex_css_typed_frame_ms = 17;
    inline constexpr int complex_css_capacity_frames = 4;
    // More paint programs than one painter retains.
    inline constexpr int complex_css_capacity_children = 300;
    // run_render_stages: scene, its 2x surface, frames, the first native-pass frame and node count.
    inline constexpr int render_stages_scene_width = 320;
    inline constexpr int render_stages_scene_height = 180;
    inline constexpr int render_stages_surface_width = 640;
    inline constexpr int render_stages_surface_height = 360;
    inline constexpr int render_stages_frames = 320;
    inline constexpr int render_stages_native_frame = 160;
    inline constexpr int render_stages_node_count = 16;
    // run_owner_async: a 120 Hz synthetic host drawing CSS scenes [8, 24) in a 4x4 grid on 4K.
    inline constexpr int owner_async_frames = 240;
    inline constexpr int owner_async_period_us = 8333;
    inline constexpr int owner_async_first_scene = 8;
    inline constexpr int owner_async_scene_end = 24;
    inline constexpr int owner_async_grid_columns = 4;
    inline constexpr int owner_async_cell_width = 960;
    inline constexpr int owner_async_cell_height = 540;
    // CSS card width at 2x.
    inline constexpr int owner_async_card_width = 774;
} // namespace weft::probe_constants::gallery
