#pragma once

#include "constants/common.hpp"
#include "constants/scene.hpp"

// Budgets, limits and validation tolerances of the renderer: the ResourceLifetime model, the
// experimental renderer C ABI (src/render/render_backend.cpp), the rounded clip mask, the D3D11
// reference renderer and its native scene pass.
namespace weft::constants::render {
    // Frame pipelining defaults shared by LifetimeConfig and the default weft_renderer_desc.
    inline constexpr int default_max_prepared_frames = 2;
    // Encoded but not yet submitted uses (ResourceLifetime use tokens).
    inline constexpr int default_max_encoded_uses = 2;
    // Encoded plus submitted uses whose submission has not completed yet.
    inline constexpr int default_max_in_flight_uses = 64;
    // Live ResourceLifetime resources (CPU leased or pinned by a frame or use).
    inline constexpr int default_max_resources = 4096;

    // Remaining default weft_renderer_desc budgets.
    inline constexpr int default_max_draws = 65536;
    inline constexpr int default_max_assets = 512;
    inline constexpr unsigned default_max_frame_asset_bytes = 16u * constants::bytes_per_mib;

    // Upper bounds a host may request; they cap the slot tables allocated up front.
    inline constexpr int max_prepared_frames_limit = 1024;
    inline constexpr int max_in_flight_uses_limit = 4096;
    inline constexpr int max_external_textures_limit = 4096;

    // Bits a backend may report: WEFT_RENDER_FEATURE_* (five features) and one bit per
    // weft_render_target_format (six formats).
    inline constexpr unsigned known_feature_bits = 31u;
    inline constexpr unsigned known_target_format_bits = 63u;

    // Accepted sdf_spread range, in texels, for signed distance assets.
    inline constexpr int min_sdf_spread = 2;
    inline constexpr int max_sdf_spread = 32;
    // Largest normalized UV origin + extent; the slack absorbs float rounding of a full extent.
    inline constexpr float max_uv_extent = 1.000001f;
    // A diagnostic clipped quad (Draw::clipped) is clipped this many pixels inside each edge.
    inline constexpr int diagnostic_clip_inset = 16;

    // Bytes per texel of the linear RGBA texture formats.
    inline constexpr int rgba16f_bytes_per_pixel = 8;
    inline constexpr int rgba32f_bytes_per_pixel = 16;

    // sRGB transfer function of IEC 61966-2-1, as in src/render/backends/d3d11/quad.hlsl.
    inline constexpr float srgb_decode_threshold = 0.04045f;
    inline constexpr float srgb_encode_threshold = 0.0031308f;
    inline constexpr float srgb_linear_slope = 12.92f;
    inline constexpr float srgb_offset = 0.055f;
    inline constexpr float srgb_scale = 1.055f;
    inline constexpr float srgb_gamma = 2.4f;
    // constants::color_channel_max as float, for byte <-> [0, 1] conversions.
    inline constexpr float color_channel_max_float = 255.0f;

    // D3D11 reference renderer and its C ABI adapter (src/render/backends/d3d11).
    namespace d3d11 {
        // Material ids of quad.hlsl. They mirror the Material* constants at the top of the shader, which
        // declares the ids its pixel shader branches on.
        // The vertex shader draws each instance as two triangles from SV_VertexID.
        inline constexpr int quad_vertex_count = 6;
        // Render targets are at most this many pixels wide and high; the backend reports the same
        // max_texture_size and max_target_size.
        inline constexpr int max_surface_dimension = 4096;

        // Host frames that may be encoded or in flight at once: the backend's encoded and in-flight use
        // limits and the TwoFramePipeline slots.
        inline constexpr int frames_in_flight = 2;
        // The backend holds one prepared frame; its native handle is always this value.
        inline constexpr int max_prepared_frames = 1;
        inline constexpr int prepared_frame_handle = 1;
        // CPU frame input budget of the backend. Three 1024x1536 RGBA images of a real game workload fit.
        inline constexpr unsigned max_frame_asset_bytes = 32u * constants::bytes_per_mib;
        // Backend capabilities: all five WEFT_RENDER_FEATURE_* bits, and the SRGB_RGBA8, LINEAR_RGBA8 and
        // LINEAR_RGBA32F bits of weft_render_target_format.
        inline constexpr int features = 31;
        inline constexpr int target_formats = 7;
        // External texture extension.
        inline constexpr int max_imported_textures = 128;
        inline constexpr int external_texture_capacity = 32;
        inline constexpr unsigned external_texture_max_bytes = 64u * constants::bytes_per_mib;

        // Device epoch and queue of the single D3D11 immediate context.
        inline constexpr int device_epoch = 1;
        inline constexpr int immediate_queue = 1;
        // A UI frame consumes two submission serials: its command list, then the host completion marker.
        // Host-only composition consumes one.
        inline constexpr int serials_per_ui_frame = 2;
        // Bounded wait for an actual GPU completion query.
        inline constexpr int completion_timeout_seconds = 5;
        inline constexpr int completion_poll_ms = 1;

        // GPU asset cache defaults.
        inline constexpr int asset_cache_max_resources = 1024;
        inline constexpr int asset_cache_max_bytes = 128 * constants::bytes_per_mib;
        // Asset identity metadata: expired entries are pruned from this size; live entries are bounded.
        inline constexpr int asset_identity_prune_threshold = 1024;
        inline constexpr int max_asset_identities = 2048;

    } // namespace d3d11

    // Scene passes (scene_pass.hpp and its backends). The shader-mirrored values match the static consts at the
    // top of scene.hlsl.
    namespace scene {
        // Paint::extra.z / extra.w flags: FlagEnabled, and its disabled counterpart.
        inline constexpr float flag_enabled = 1.f;
        inline constexpr float flag_disabled = 0.f;
        // MaxGradientStops and StopsPerOffsetVector.
        inline constexpr int max_gradient_stops = constants::scene::max_gradient_stops;
        inline constexpr int stops_per_offset_vector = 4;
        inline constexpr int gradient_offset_vectors = max_gradient_stops / stops_per_offset_vector;
        // Size of the shader Paint record (28 float4 registers).
        inline constexpr int record_bytes = 448;
        // Size of the shader Clip record (3 float4 registers).
        inline constexpr int clip_bytes = 48;
        // Size of the View constant buffer (one float4).
        inline constexpr int view_constants_bytes = 16;
        // Paint::mode.x of image items: ImageModeCoverage and ImageModeSignedDistance (other images are 0).
        inline constexpr float image_mode_coverage = 1.f;
        inline constexpr float image_mode_signed_distance = 2.f;
        inline constexpr int record_initial_capacity = 16;
        // Nested transparent groups available per slot.
        inline constexpr int max_group_depth = 4;
        // Rounded clips an item's chain may hold; the shader multiplies the coverage of each, so a longer chain is
        // refused (Diag::PaintList, or WEFT_RENDER_UNSUPPORTED_FEATURE at the C ABI). The CPU reference coverage
        // takes the same bound (rounded_mask_max_clips).
        inline constexpr int max_rounded_clips = 16;
        // Text and layer images keep a transparent margin around the node on each side.
        inline constexpr int image_margin = 2;
        inline constexpr int image_margins = 2 * image_margin;
        // Instance bounds grow by this many device pixels on each side for antialiasing, and stop as far past the clip.
        inline constexpr int record_bounds_outset = 2;
        // Initial extent of the bounds accumulation.
        inline constexpr float record_bounds_sentinel = 1e10f;
        // Overflow tolerances for text metrics and for layer descendants, in CSS pixels.
        inline constexpr float text_fit_tolerance = .01f;
        inline constexpr double layer_bounds_tolerance = .01;
        // Text/layer image cache: entry cap and byte budget. A single image must fit in the budget.
        inline constexpr int image_cache_max_entries = 128;
        inline constexpr int image_cache_budget_bytes = 16 * constants::bytes_per_mib;
        // Transparent-group scratch targets across all slots.
        inline constexpr int scratch_budget_bytes = 128 * constants::bytes_per_mib;
    } // namespace scene

    // Rounded clip probe work bounds.
    // Largest absolute rectangle coordinate, size or corner radius.
    inline constexpr int rounded_clip_max_magnitude = constants::max_logical_px_magnitude;
    inline constexpr int rounded_mask_max_clips = 16;
    inline constexpr int rounded_mask_max_dimension = 4096;
    inline constexpr int rounded_mask_max_pixels = 8 * 1024 * 1024;
    // Supersampling grid per axis for partially covered mask pixels.
    inline constexpr unsigned rounded_mask_samples_per_axis = 16;
} // namespace weft::constants::render
