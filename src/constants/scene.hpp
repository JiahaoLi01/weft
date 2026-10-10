#pragma once

#include "constants/common.hpp"

// Scene layer values: CSS scene profile, typed paint, damage, native paint compiler and the
// Windows D2D raster adapter (see docs/coding-style.md).
//
// Each constant has the type of the literals it replaced (a plain integer literal is int), so
// arithmetic, comparisons and generated code at every use site stay exactly as before.
namespace weft::constants::scene {
    // Parsing and profile bounds.
    // Largest magnitude accepted for any parsed scene number.
    inline constexpr double max_numeric_magnitude = 1e7;
    // Most functions in one typed transform list.
    inline constexpr int max_paint_transform_operations = 32;
    // Significant digits when sampled animation values are printed back into style strings.
    inline constexpr int numeric_output_precision = 12;
    // Largest viewport edge accepted by the scene solver.
    inline constexpr int max_viewport_dimension = constants::max_viewport_dimension_px;
    // Most equal 1fr tracks in grid-template-columns.
    inline constexpr int max_grid_columns = 8;
    // Children of an overflow:auto box may overhang it by this much without needing scrolling.
    inline constexpr float overflow_tolerance_px = .03f;

    // CSS defaults.
    // Default object size of an SVG root without width/height.
    inline constexpr const char* default_object_width = "300";
    inline constexpr const char* default_object_height = "150";
    // UA font-weight of b and strong.
    inline constexpr const char* bold_font_weight = "700";
    // Numeric weights of the normal and bold font-weight keywords.
    inline constexpr int normal_font_weight_value = 400;
    inline constexpr int bold_font_weight_value = 700;
    // Fallback line-height multiplier of the raster adapter.
    inline constexpr const char* default_line_height_multiplier = "1.5";
    // linear-gradient() without a direction points "to bottom".
    inline constexpr int default_linear_gradient_angle = 180;

    // Animation and interpolation.
    // CSS discrete interpolation switches to the end value at the midpoint.
    inline constexpr double discrete_interpolation_midpoint = .5;
    // Control points of the CSS easing keywords: ease is cubic-bezier(.25, .1, .25, 1),
    // ease-in starts with x1 = .42 and ease-out ends with x2 = .58.
    inline constexpr double ease_x1 = .25;
    inline constexpr double ease_y1 = .1;
    inline constexpr double ease_x2 = .25;
    inline constexpr double ease_in_x1 = .42;
    inline constexpr double ease_out_x2 = .58;
    // Bisection steps that invert x(t) of a cubic-bezier timing function.
    inline constexpr int bezier_bisection_steps = 32;
    // Owner caches of parsed interpolation endpoints.
    inline constexpr int interpolation_cache_max_entries = 512;
    inline constexpr int interpolation_cache_max_bytes = 512 * constants::bytes_per_kib;
    inline constexpr int paint_pair_cache_max_entries = 512;
    inline constexpr int paint_pair_cache_max_bytes = 512 * constants::bytes_per_kib;

    // Slots of ScenePaintValues::values, in scene_paint_properties() order.
    inline constexpr int paint_property_count = 8;
    inline constexpr int paint_slot_opacity = 0;
    inline constexpr int paint_slot_transform = 1;
    inline constexpr int paint_slot_background_color = 2;
    inline constexpr int paint_slot_color = 3;
    // First of the four border colors, in top, right, bottom, left order.
    inline constexpr int paint_slot_border_color = 4;

    // Damage and raster scale.
    // Largest device scale accepted by the damage and raster adapters.
    inline constexpr int max_raster_scale = 4;
    // Damage grows by this many CSS px on each side for antialiasing and flattening.
    inline constexpr int damage_guard_px = 2;
    // Damage covering more than this fraction of the surface repaints all of it.
    inline constexpr float damage_full_repaint_ratio = .65f;

    // Native paint compiler.
    // NativePaint::gradient codes, mirrored by shaders/scene_native_probe.hlsl.
    inline constexpr unsigned gradient_none = 0u;
    inline constexpr unsigned gradient_linear = 1u;
    inline constexpr unsigned gradient_radial = 2u;
    inline constexpr unsigned gradient_conic = 3u;
    // Mirrors MaxGradientStops in shaders/scene_native_probe.hlsl.
    inline constexpr int max_gradient_stops = 8;
    inline constexpr int max_native_paint_commands = 2048;
    // Deepest nesting of isolated opacity groups.
    inline constexpr int max_opacity_group_depth = 4;
    // Elements whose transform determinant is smaller than this paint nothing.
    inline constexpr double singular_determinant_epsilon = 1e-8;
    // Largest sum of the transform's column lengths drawn by native passes and border geometry.
    inline constexpr int max_geometry_transform_scale = 16;
    // Off-axis matrix terms below this still count as an axis-aligned clip.
    inline constexpr float axis_aligned_epsilon = .00001f;
    // Text made only of printable ASCII [first, end) can use the single-channel glyph mask.
    inline constexpr int printable_ascii_first = 32;
    inline constexpr int printable_ascii_end = 127;

    // D2D raster adapter: text.
    // Layout extent when the box does not constrain a text line.
    inline constexpr int text_layout_max_extent = constants::max_logical_px_magnitude;
    // Family that maps to the system message font.
    inline constexpr const char* system_font_family = "Segoe UI";
    inline constexpr int text_cache_max_entries = 256;
    inline constexpr int text_cache_max_bytes = 2 * constants::bytes_per_mib;
    // UTF-8 lead bytes of four-byte sequences (supplementary planes, including color emoji).
    inline constexpr int utf8_four_byte_lead_mask = 0xf8;
    inline constexpr int utf8_four_byte_lead = 0xf0;
    // Color glyph runs are cached as bitmaps up to this device pixel area.
    inline constexpr int color_text_max_pixels = 2 * 1024 * 1024;
    inline constexpr int color_text_cache_max_entries = 32;
    inline constexpr int color_text_cache_max_bytes = 8 * constants::bytes_per_mib;

    // D2D raster adapter: geometry.
    inline constexpr int max_svg_path_bytes = 256 * constants::bytes_per_kib;
    inline constexpr int max_svg_path_segments = 65536;
    // Characters of path source quoted in a parse diagnostic.
    inline constexpr int path_diagnostic_excerpt_chars = 40;
    inline constexpr int path_cache_max_entries = 128;
    inline constexpr int path_cache_max_bytes = 2 * constants::bytes_per_mib;
    inline constexpr int round_box_cache_max_entries = 256;
    inline constexpr int shape_cache_max_entries = 1024;
    inline constexpr int shape_cache_max_bytes = 4 * constants::bytes_per_mib;
    inline constexpr int stroke_cache_max_entries = 128;
    inline constexpr int stroke_cache_max_bytes = 2 * constants::bytes_per_mib;
    inline constexpr int max_stroke_dashes = 64;
    // Caches of widened border rings and side wedges.
    inline constexpr int border_geometry_cache_max_entries = 256;
    // Flattening tolerance when border geometry is widened and intersected.
    inline constexpr float geometry_flattening_tolerance = .01f;
    // Side index that selects the whole border ring instead of one side wedge.
    inline constexpr int border_side_all = 4;
    // Border widths closer than this share the box center as the wedge apex.
    inline constexpr double border_width_epsilon = 1e-5;

    // D2D raster adapter: shadows, gradients and bitmaps.
    inline constexpr int max_box_shadow_layers = 8;
    // Largest box-shadow blur radius and spread magnitude.
    inline constexpr int max_box_shadow_extent_px = 64;
    // Gaussian shadow kernels extend this many standard deviations.
    inline constexpr int shadow_kernel_sigmas = 3;
    // Extra device pixels around a blurred shadow bitmap.
    inline constexpr int shadow_guard_px = 2;
    inline constexpr int max_shadow_bitmap_pixels = 1024 * 1024;
    inline constexpr int shadow_cache_max_entries = 32;
    inline constexpr int shadow_cache_max_bytes = 16 * constants::bytes_per_mib;
    // A linear gradient tile within this of the box still uses the native brush.
    inline constexpr float gradient_tile_epsilon = .001f;
    inline constexpr int gradient_cache_max_entries = 32;
    inline constexpr int gradient_cache_max_bytes = 32 * constants::bytes_per_mib;
    // Largest software backing, gradient or projected bitmap.
    inline constexpr int max_bitmap_pixels = 4096 * 4096;

    // D2D raster adapter: projective planes and hit geometry.
    inline constexpr int projective_cache_max_entries = 16;
    inline constexpr int projective_cache_max_bytes = 16 * constants::bytes_per_mib;
    // Planes whose rotation cosine is below this are edge-on and paint nothing.
    inline constexpr double edge_on_plane_epsilon = 1e-5;
    // Inverse projection skips samples whose denominator is below this.
    inline constexpr double projective_denominator_epsilon = 1e-6;
    // Seed of the projected corner bounds; exceeds every accepted coordinate.
    inline constexpr double projective_bounds_seed = 1e7;
    // Homogeneous w and determinants below this are treated as singular.
    inline constexpr double plane_singular_epsilon = 1e-10;
    // Image of a point that projects to infinity.
    inline constexpr double plane_far_point = 1e10;
    // Seed of the running min/max bounds in geometry().
    inline constexpr double geometry_bounds_seed = 1e10;

    // D2D raster adapter: paint programs and retained layers.
    inline constexpr int paint_program_cache_max_entries = 256;
    inline constexpr int paint_program_cache_max_bytes = 2 * constants::bytes_per_mib;
    inline constexpr int max_d2d_layers = 512;
    inline constexpr int max_explicit_layers = 32;
    // Transparent margin around a retained layer bitmap.
    inline constexpr int layer_padding_px = 2;
    // Text or descendant boxes may exceed a retained layer root by this much.
    inline constexpr float layer_text_fit_tolerance_px = .01f;
    inline constexpr double layer_bounds_tolerance_px = .01;
    inline constexpr int retained_layer_cache_max_entries = 32;
    inline constexpr int retained_layer_cache_max_bytes = 32 * constants::bytes_per_mib;
    // D2D DIPs per inch at scale 1.
    inline constexpr int d2d_default_dpi = 96;
} // namespace weft::constants::scene
