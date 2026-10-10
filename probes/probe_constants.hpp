#pragma once

#include "constants/common.hpp"
#include "constants/render.hpp"

#include <cstddef>

// Constants of the D3D11 host probes (weft_d3d11_probe). Each constant has the type of the literal it
// replaced (see docs/coding-style.md).
//
// std::min, std::max, std::clamp and the std::chrono::duration constructor take const references.
// Pass a constant to them as a static_cast<T>(constant) prvalue, like the literal it replaced:
// binding the reference to the inline variable itself changes MSVC code generation.
namespace weft::probe_constants {
    // Material ids of the D3D11 reference renderer's quad shader.
    namespace shader_material = constants::render::d3d11::shader_material;

    inline constexpr int quad_vertex_count = constants::render::d3d11::quad_vertex_count;
    inline constexpr int max_surface_dimension = constants::render::d3d11::max_surface_dimension;
    // Color fixture target, also the default Experiment and PNG size.
    inline constexpr int color_fixture_width = 256;
    inline constexpr int color_fixture_height = 128;
    // The color fixture's scissor test clips this many pixels from each edge of its quad.
    inline constexpr int scissor_test_inset = constants::render::d3d11::diagnostic_clip_inset;
    // Single-channel distance atlas of the SDF fixture.
    inline constexpr int sdf_atlas_width = 256;
    inline constexpr int sdf_atlas_height = 128;
    // The SDF straight-edge oracle scans this far around each square and skips the corner
    // neighborhoods where both edges are closer than the exclusion distance.
    inline constexpr int sdf_edge_scan_margin = 4;
    inline constexpr int sdf_corner_exclusion = 2;
    // Square target of the two-frame pipeline and render cache probes.
    inline constexpr int small_target_size = 64;
    inline constexpr int small_target_pixels = small_target_size * small_target_size;

    // D3D11 reference renderer and backend limits (constants::render::d3d11), under their probe names.
    inline constexpr int frames_in_flight = constants::render::d3d11::frames_in_flight;
    inline constexpr int backend_max_prepared_frames = constants::render::d3d11::max_prepared_frames;
    inline constexpr int backend_prepared_frame_handle = constants::render::d3d11::prepared_frame_handle;
    // Assets of one FramePacket: the asset limit that the backend requests.
    inline constexpr int max_frame_assets = constants::render::default_max_assets;
    inline constexpr unsigned backend_max_frame_asset_bytes = constants::render::d3d11::max_frame_asset_bytes;
    inline constexpr int backend_features = constants::render::d3d11::features;
    inline constexpr int backend_target_formats = constants::render::d3d11::target_formats;
    inline constexpr int max_imported_textures = constants::render::d3d11::max_imported_textures;
    inline constexpr int external_texture_capacity = constants::render::d3d11::external_texture_capacity;
    inline constexpr unsigned external_texture_max_bytes = constants::render::d3d11::external_texture_max_bytes;
    inline constexpr int device_epoch = constants::render::d3d11::device_epoch;
    inline constexpr int immediate_queue = constants::render::d3d11::immediate_queue;
    inline constexpr int serials_per_ui_frame = constants::render::d3d11::serials_per_ui_frame;
    // Color fixture serials: the UI command list, then the host-owned copies that follow it.
    inline constexpr int fixture_ui_serial = 42;
    inline constexpr int fixture_host_serial = 43;
    inline constexpr int completion_timeout_seconds = constants::render::d3d11::completion_timeout_seconds;
    inline constexpr int completion_poll_ms = constants::render::d3d11::completion_poll_ms;
    inline constexpr int gpu_cache_max_resources = constants::render::d3d11::asset_cache_max_resources;
    inline constexpr int gpu_cache_max_bytes = constants::render::d3d11::asset_cache_max_bytes;
    inline constexpr int asset_identity_prune_threshold = constants::render::d3d11::asset_identity_prune_threshold;
    inline constexpr int max_asset_identities = constants::render::d3d11::max_asset_identities;
    // Glyph textures of one React page frame.
    inline constexpr int page_glyph_max_bytes = 4 * constants::bytes_per_mib;
    // CheetahPacketCache budgets; a cache is cleared when a bound would be exceeded.
    inline constexpr int glyph_cache_max_entries = 2048;
    inline constexpr int glyph_cache_max_bytes = 16 * constants::bytes_per_mib;
    inline constexpr int mask_cache_max_entries = 1024;
    inline constexpr int mask_cache_max_bytes = 32 * constants::bytes_per_mib;
    // Rounded-clip mask bytes prepared for one packet.
    inline constexpr int packet_mask_max_bytes = 32 * constants::bytes_per_mib;
    // Diagnostic HUD overlay of the window presenter.
    inline constexpr int overlay_max_draws = 4096;
    inline constexpr int overlay_max_bytes = 16 * constants::bytes_per_mib;

    // Win32 sample host. Window messages only queue values; the queue resets on overflow.
    inline constexpr int max_queued_input = 1024;
    inline constexpr int max_messages_per_pump = 4096;
    inline constexpr int swap_chain_buffer_count = 2;
    inline constexpr int present_sync_interval = 1;
    // Low latency swapchains: three flip buffers, at most two queued frames, and a bounded wait for the next one.
    // One queued frame halves the rate (120 of 240 Hz) whenever a frame's CPU work exceeds one refresh; two absorb
    // such frames at one refresh of added latency (measured in docs/gallery-interaction-profile.md).
    inline constexpr int low_latency_buffer_count = 3;
    inline constexpr int low_latency_queued_frames = 2;
    inline constexpr int frame_latency_wait_ms = 100;
    // IDC_ARROW, spelled for LoadCursorW without UNICODE.
    inline constexpr int arrow_cursor_resource = 32512;
    // MsgWaitForMultipleObjectsEx timeout of an idle host loop, and the visible NineSlice poll.
    inline constexpr int idle_wait_ms = 16;
    inline constexpr int visible_poll_ms = 8;
    // Windows default wheel settings (lines and characters per notch), the self-test policy.
    inline constexpr int default_wheel_scroll_lines = 3;
    inline constexpr int default_wheel_scroll_chars = 3;
    // Larger wheel settings are clamped to this many lines or characters per notch.
    inline constexpr unsigned max_wheel_scroll_setting = 1000u;
    // Sample-host logical pixels per wheel line or character; a page is one logical view.
    inline constexpr double wheel_line_px = 24.;
    inline constexpr double wheel_char_px = 8.;
    inline constexpr double wheel_page_height_px = constants::default_view_height;
    inline constexpr double wheel_page_width_px = constants::default_view_width;

    // sRGB transfer function and byte scale (constants::render).
    inline constexpr float srgb_decode_threshold = constants::render::srgb_decode_threshold;
    inline constexpr float srgb_encode_threshold = constants::render::srgb_encode_threshold;
    inline constexpr float srgb_linear_slope = constants::render::srgb_linear_slope;
    inline constexpr float srgb_offset = constants::render::srgb_offset;
    inline constexpr float srgb_scale = constants::render::srgb_scale;
    inline constexpr float srgb_gamma = constants::render::srgb_gamma;
    inline constexpr float color_channel_max_float = constants::render::color_channel_max_float;
    // Significant digits of floating values in probe reports; 9 round-trip a float.
    inline constexpr int report_significant_digits = 9;
    // Significant digits that round-trip a double through decimal text.
    inline constexpr int double_round_trip_digits = 17;

    // Pixel tolerances. RGBA8 values are channel differences.
    // Direct versus render-to-texture output; alpha must match exactly.
    inline constexpr int rgba8_parity_tolerance = 2;
    // GPU pixels versus an independent CPU sample oracle.
    inline constexpr int cpu_oracle_tolerance = 3;
    // Pipelined or cached frames versus the full path.
    inline constexpr int pipeline_pixel_tolerance = 1;
    // Linear premultiplied float readback of the color fixture.
    inline constexpr float linear_alpha_tolerance = 0.00001f;
    // Linear coverage error of SDF straight edges and strokes.
    inline constexpr float sdf_edge_tolerance = 0.12f;
    // Rounded mask versus a dense elliptical area integration.
    inline constexpr int ellipse_area_tolerance = 5;
    // Linear float composite of external textures; filtering has finite subtexel precision.
    inline constexpr float external_float_tolerance = 1.f / 256;
    // Logical scroll offset and animated position, and the time-based motion speed checks.
    inline constexpr double scroll_tolerance_px = .001;
    inline constexpr double motion_speed_tolerance_px = .002;

    // Fixture scripts and fonts.
    // Simulated time between fixture frames.
    inline constexpr int fixture_frame_ms = 16;
    // React, NineSlice and flow fixtures: the initial state and two interactions.
    inline constexpr int interaction_frames = 3;
    // AlertBar fixture states.
    inline constexpr int alert_frames = 8;
    // Font index of the CJK fallback in the Roboto + Noto CJK font set.
    inline constexpr int cjk_fallback_font_id = 1;
    // Original CaseCheetah art PNG size.
    inline constexpr int cheetah_art_width = 1024;
    inline constexpr int cheetah_art_height = 1536;

    // 4K flow window performance and motion profiles.
    inline constexpr int perf_warmup_frames = 8;
    inline constexpr int perf_sample_frames = 120;
    inline constexpr int motion_tick_ms = 4;
    // Left edge of the 4K FPS HUD; the pixel oracle stops before it.
    inline constexpr int fps_hud_x = 3300;
    // Raster text above this font size counts as high-ppem glyphs.
    inline constexpr int high_ppem_font_size = 40;
    // ScrollTimings columns; this column holds the GPU UI time.
    inline constexpr int scroll_timing_columns = 9;
    inline constexpr int scroll_timing_gpu_ui_column = 7;

    // Deterministic mutation probes.
    inline constexpr unsigned two_frame_seed = 0xCE102026;
    inline constexpr int two_frame_rounds = 24;
    inline constexpr int pipeline_cache_max_resources = 16;
    inline constexpr unsigned render_cache_seed = 0xCE902026;
    inline constexpr int render_cache_mutation_frames = 48;
    // Deliberately tiny cache of the render cache probe.
    inline constexpr int stress_cache_max_resources = 8;
    // Asset identity churn: large transient bitmaps, then short-lived 1x1 assets.
    inline constexpr int identity_traffic_bitmaps = 24;
    inline constexpr int identity_churn_assets = 3000;
    // Packet states that the seeded mutations draw from.
    inline constexpr int mutation_states = 31;
    inline constexpr unsigned external_seed = 0xce112026;
    // External texture batches, each with frames_in_flight submissions.
    inline constexpr int external_batches = 24;

    // Owner stack sampler (sampling_profiler.cpp): deepest unwound stack, return addresses reserved up front (8M,
    // 64 MiB: a suspended thread may hold the heap lock), samples reserved, rows per report table and callers shown
    // per row.
    inline constexpr unsigned sampler_max_depth = 128;
    inline constexpr std::size_t sampler_frame_capacity = std::size_t(1) << 23;
    inline constexpr std::size_t sampler_sample_capacity = sampler_frame_capacity / 16;
    inline constexpr std::size_t sampler_report_rows = 60;
    inline constexpr std::size_t sampler_callers_shown = 4;
} // namespace weft::probe_constants
