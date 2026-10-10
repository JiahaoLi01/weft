#pragma once

#include "caseelephant/constants/common.hpp"

// Budgets, limits and validation tolerances of the renderer: the ResourceLifetime model, the
// experimental renderer C ABI (src/render/render_backend.cpp) and the rounded clip probe.
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

    // Bits a backend may report: WEFT_RENDER_FEATURE_* (four features) and one bit per
    // weft_render_target_format (six formats).
    inline constexpr unsigned known_feature_bits = 15u;
    inline constexpr unsigned known_target_format_bits = 63u;

    // Accepted sdf_spread range, in texels, for signed distance assets.
    inline constexpr int min_sdf_spread = 2;
    inline constexpr int max_sdf_spread = 32;
    // Largest normalized UV origin + extent; the slack absorbs float rounding of a full extent.
    inline constexpr float max_uv_extent = 1.000001f;

    // Bytes per texel of the linear RGBA texture formats.
    inline constexpr int rgba16f_bytes_per_pixel = 8;
    inline constexpr int rgba32f_bytes_per_pixel = 16;

    // Rounded clip probe work bounds.
    // Largest absolute rectangle coordinate, size or corner radius.
    inline constexpr int rounded_clip_max_magnitude = constants::max_logical_px_magnitude;
    inline constexpr int rounded_mask_max_clips = 16;
    inline constexpr int rounded_mask_max_dimension = 4096;
    inline constexpr int rounded_mask_max_pixels = 8 * 1024 * 1024;
    // Supersampling grid per axis for partially covered mask pixels.
    inline constexpr unsigned rounded_mask_samples_per_axis = 16;
} // namespace weft::constants::render
