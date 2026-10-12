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

    // sRGB transfer function of IEC 61966-2-1, as in src/render/backends/d3d11/srgb_convert.hlsl.
    inline constexpr float srgb_decode_threshold = 0.04045f;
    inline constexpr float srgb_encode_threshold = 0.0031308f;
    inline constexpr float srgb_linear_slope = 12.92f;
    inline constexpr float srgb_offset = 0.055f;
    inline constexpr float srgb_scale = 1.055f;
    inline constexpr float srgb_gamma = 2.4f;
    // constants::color_channel_max as float, for byte <-> [0, 1] conversions.
    inline constexpr float color_channel_max_float = 255.0f;

    // The texture cache every renderer shares (render/texture_cache.hpp).
    namespace texture_cache {
        // Bounds of a renderer's cache: textures and their bytes.
        inline constexpr int max_resources = 1024;
        inline constexpr int max_bytes = 128 * constants::bytes_per_mib;
        // Asset identities: expired entries are pruned from this size; live entries are bounded.
        inline constexpr int identity_prune_threshold = 1024;
        inline constexpr int max_identities = 2048;
        // Glyph atlas: R8 pages of this many pixels a side, at most this many pages, for coverage images of at most
        // atlas_max_image pixels a side, each placed with a border of this many texels that repeat its edge texels.
        inline constexpr int atlas_page_size = 2048;
        inline constexpr int atlas_max_pages = 4;
        inline constexpr int atlas_max_image = 128;
        inline constexpr int atlas_border = 1;
        // A cache key whose version has this bit names the texture of its own that an effect samples, never an atlas
        // region. Display list versions are small counters, and effects do not come through the C ABI's quad frames,
        // so it never meets a host's version.
        inline constexpr unsigned long long standalone_key_bit = 1ull << 63;
    } // namespace texture_cache

    // D3D11 reference renderer and its C ABI adapter (src/render/backends/d3d11).
    namespace d3d11 {
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

        // Constant buffer ranges bind at this granularity (16 constants), and a bound range holds at most
        // max_constant_buffer_bytes (D3D11.1 constant buffer offsetting).
        inline constexpr int constant_buffer_offset_bytes = 256;
        inline constexpr int max_constant_buffer_bytes = 65536;

        // Device epoch and queue of the single D3D11 immediate context.
        inline constexpr int device_epoch = 1;
        inline constexpr int immediate_queue = 1;
        // A UI frame consumes two submission serials: its command list, then the host completion marker.
        // Host-only composition consumes one.
        inline constexpr int serials_per_ui_frame = 2;
        // Bounded wait for an actual GPU completion query.
        inline constexpr int completion_timeout_seconds = 5;
        inline constexpr int completion_poll_ms = 1;

    } // namespace d3d11

    // The Vulkan backend (src/render/backends/vulkan).
    namespace vulkan {
        // Descriptor bindings of the shared scene shader's SPIR-V, after the register shifts of cmake/WeftSpirv.cmake:
        // the View constants b0, the records t0, the image t1, the rounded clips t2 and the sampler s0.
        inline constexpr int binding_view = 0;
        inline constexpr int binding_records = 10;
        inline constexpr int binding_image = 11;
        inline constexpr int binding_clips = 12;
        inline constexpr int binding_sampler = 20;
        // An effect's textures (scene.hlsl effectTexture0-3, t3-t6: one binding each from this one) and the sampler
        // that repeats them (effectRepeat, s1).
        inline constexpr int binding_effect_textures = 13;
        inline constexpr int binding_effect_sampler = 21;
        // Each instance draws two triangles.
        inline constexpr int quad_vertex_count = 6;
        // Descriptor sets one pass may allocate: one per batch.
        inline constexpr int max_pass_descriptor_sets = 4096;
        // Waits on a fence give up after this many nanoseconds.
        inline constexpr unsigned long long fence_timeout_ns = 10'000'000'000ull;
        // Render targets are at most this many pixels wide and high.
        inline constexpr int max_surface_dimension = 4096;
        // The C ABI adapter: the device epoch and queue it reports, its one prepared frame's handle, frames recorded or
        // in flight at once, the features of WEFT_RENDER_FEATURE_* it draws (solid, image, coverage, SDF, display
        // lists) and the CPU input bytes one quad frame may carry.
        inline constexpr int device_epoch = 1;
        inline constexpr int queue = 1;
        inline constexpr int prepared_frame_handle = 1;
        inline constexpr int max_prepared_frames = 1;
        inline constexpr int frames_in_flight = 2;
        inline constexpr int features = 31;
        inline constexpr unsigned max_frame_asset_bytes = 32u * constants::bytes_per_mib;
        // Completed frames give the renderer their buffers and descriptor pools: it keeps at most this many of each
        // kind, buffers of at most spare_buffer_max_bytes. A new buffer has room for at least spare_buffer_min_bytes,
        // a new pool for descriptor_pool_min_sets sets, both doubling past them.
        inline constexpr int spare_frame_resources = 4;
        inline constexpr unsigned spare_buffer_max_bytes = 16u * constants::bytes_per_mib;
        inline constexpr unsigned spare_buffer_min_bytes = 64u * constants::bytes_per_kib;
        inline constexpr int descriptor_pool_min_sets = 16;
        // What a scene slot keeps of its recordings for its next ones: buffers, scratch images (by size) and
        // descriptor pools.
        inline constexpr int scene_slot_spare_buffers = 64;
        inline constexpr int scene_slot_spare_images = 16;
        inline constexpr int scene_slot_spare_pools = 16;
        // Images of at most block_image_max_bytes share blocks of image_block_bytes of device-local memory: one
        // allocation per block instead of one per image. A device keeps one empty block of each memory type.
        inline constexpr unsigned image_block_bytes = 16u * constants::bytes_per_mib;
        inline constexpr unsigned block_image_max_bytes = 2u * constants::bytes_per_mib;
        // A swapchain asks for this many images beyond the surface's minimum, so that acquiring rarely waits.
        inline constexpr int swapchain_extra_images = 1;
    } // namespace vulkan

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
        // Size of the shader Clip record (5 float4 registers).
        inline constexpr int clip_bytes = 80;
        // Size of the View constant buffer (one float4).
        inline constexpr int view_constants_bytes = 16;
        // Paint::mode.x of image items: ImageModeCoverage and ImageModeSignedDistance (other images are 0).
        inline constexpr float image_mode_coverage = 1.f;
        inline constexpr float image_mode_signed_distance = 2.f;
        // Paint::mode.x of a plane: its content image seen through a homography (ImageModeProjective).
        inline constexpr float image_mode_projective = 3.f;
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
        // Items that cover pixel centers need no antialiasing, but their bounds reach this far past them, in device
        // pixels: a rasterizer may drop a pixel whose center lies exactly on an edge (Vulkan leaves the tie open).
        inline constexpr float pixel_center_outset = .5f;
        // Initial extent of the bounds accumulation.
        inline constexpr float record_bounds_sentinel = 1e10f;
        // Overflow tolerances for text metrics and for layer descendants, in CSS pixels.
        inline constexpr float text_fit_tolerance = .01f;
        inline constexpr double layer_bounds_tolerance = .01;
        // Each image cache of a scene preparer (text and layers, shadows, paths): the entries and bytes past which the
        // least recently used images of earlier passes go (SceneCacheBounds).
        inline constexpr int image_cache_max_entries = 1024;
        inline constexpr int image_cache_budget_bytes = 32 * constants::bytes_per_mib;
        // The bytes of one text, layer, shadow or path image.
        inline constexpr int image_max_bytes = 16 * constants::bytes_per_mib;
        // Transparent-group scratch targets across all slots.
        inline constexpr int scratch_budget_bytes = 128 * constants::bytes_per_mib;
    } // namespace scene

    // Vector paths (vector/path_raster.hpp).
    namespace path {
        // Largest coverage mask, in device pixels; its BGRA image fits the scene image budget.
        inline constexpr int max_mask_pixels = 2048 * 2048;
        // Coverage reaches this many device pixels past a path's bounds.
        inline constexpr int antialias_margin = 1;
        // Curves flatten for dashing within this many device pixels, and for the rasterizer within this many.
        inline constexpr double dash_flattening_tolerance = .1;
        inline constexpr double raster_flattening_tolerance = .01;
        // Most dashes one path may cut into.
        inline constexpr int max_dash_segments = 65536;
        // Deepest subdivision of one cubic while flattening.
        inline constexpr int max_flattening_depth = 16;
        // FreeType points per pixel (26.6 fixed point) and the unit of its 16.16 miter limit.
        inline constexpr int freetype_subpixels = 64;
        inline constexpr int freetype_fixed_one = 65536;
    } // namespace path

    // Rounded clip probe work bounds.
    // Largest absolute rectangle coordinate, size or corner radius.
    inline constexpr int rounded_clip_max_magnitude = constants::max_logical_px_magnitude;
    inline constexpr int rounded_mask_max_clips = 16;
    inline constexpr int rounded_mask_max_dimension = 4096;
    inline constexpr int rounded_mask_max_pixels = 8 * 1024 * 1024;
    // Supersampling grid per axis for partially covered mask pixels.
    inline constexpr unsigned rounded_mask_samples_per_axis = 16;
} // namespace weft::constants::render
