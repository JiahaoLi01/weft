#pragma once

#include "constants/common.hpp"
#include "constants/render.hpp"

// Bounds and tolerances of the M0 layout profiles (flex adapter, inline formatter and the
// page/positioned/auto/flow builders). Each constant has the type of the literal it replaced.
namespace weft::constants::layout {

    // Code points that drive whitespace collapsing and control-character rejection.
    inline constexpr int codepoint_tab = 9;
    inline constexpr int codepoint_line_feed = 10;
    inline constexpr int codepoint_carriage_return = 13;
    // Space; every code point below it is a C0 control.
    inline constexpr int codepoint_space = 32;
    // DEL, the first code point after printable ASCII and the start of the C1 control block.
    inline constexpr int codepoint_delete = 127;
    inline constexpr int codepoint_c1_control_last = 159;
    inline constexpr int codepoint_soft_hyphen = 0x00ad;

    // Initial font size and line height of a bare flex probe tree (no CSS profile defaults).
    inline constexpr int flex_default_font_size_px = 20;
    inline constexpr int flex_default_line_height_px = 20;
    // Largest absolute numeric value accepted by the flex adapter.
    inline constexpr int max_flex_number_magnitude = 1000000;
    // Largest single style value, in bytes, accepted by the flex adapter.
    inline constexpr int max_style_value_bytes = 128;

    // Text budgets. Aggregate text bounds a whole tree; work bounds repeated shaping.
    inline constexpr int max_aggregate_text_bytes = constants::bytes_per_mib;
    inline constexpr int max_line_measure_work_bytes = constants::bytes_per_mib;
    inline constexpr int max_inline_shaping_work_bytes = 8 * constants::bytes_per_mib;
    // Each inline run comes from one document node.
    inline constexpr int max_inline_runs = constants::max_document_nodes;

    // Largest font size and px length (padding, line height, page profile values).
    inline constexpr int max_font_size_px = 256;
    inline constexpr int max_length_px = 4096;
    // Largest viewport width or height of the page profiles.
    inline constexpr int max_viewport_size_px = constants::max_viewport_dimension_px;
    // Largest absolute CSS number, offset or box coordinate in the positioned/auto/flow profiles.
    inline constexpr int max_length_magnitude_px = constants::max_logical_px_magnitude;
    inline constexpr int max_z_index_magnitude = 1024;

    // Absolute calc() dependencies: at most this many full relayouts, and a bound on
    // relayouts times box count.
    inline constexpr int max_calc_relayout_passes = 15;
    inline constexpr int max_calc_relayout_box_work = 16384;

    // Nested rounded overflow clips carried by one paint item.
    inline constexpr int max_rounded_clip_depth = constants::render::rounded_mask_max_clips;
    // Float tolerance when testing whether text fits the available line width.
    inline constexpr float line_fit_epsilon = 0.0001f;
    // Float tolerance when joining adjacent inline fragments of one element.
    inline constexpr float fragment_merge_epsilon = .0001f;
    // Shaped single-line text may exceed the content box by this much before it counts as overflow.
    inline constexpr float text_overflow_tolerance_px = 0.025f;
} // namespace weft::constants::layout
