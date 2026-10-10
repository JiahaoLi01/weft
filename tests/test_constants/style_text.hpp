#pragma once

// Test-only values of the style cascade, text/font and Canvas2D tests. Each constant has the
// type of the literal it replaced.
namespace caseelephant::test_constants::style_text {
    // Tolerance for float colors, advances and box sizes.
    inline constexpr float float_tolerance = 0.001f;

    // Custom properties whose values each reference the previous one twice, doubling the
    // substituted tokens per level until a cascade token bound rejects the sheet.
    inline constexpr int var_doubling_levels = 16;

    // Randomized cached/in-place versus full cascade differential.
    inline constexpr int memo_mutation_seed = 0xc0ffee;
    inline constexpr int memo_mutation_iterations = 512;
    // Mutation kinds chosen per iteration (style, class, attribute, inline style, move, generation).
    inline constexpr int memo_mutation_kinds = 6;
    // Iteration periods of clearing the rule memo and of reversing a sibling list.
    inline constexpr int memo_clear_period = 47;
    inline constexpr int sibling_reverse_period = 31;
    // Differential tree: view epoch, leaves per parent and the slot of the first leaf.
    inline constexpr int memo_view_epoch = 77;
    inline constexpr int memo_leaves_per_parent = 16;
    inline constexpr int memo_first_leaf_slot = 10;
    // Quota tree: children sharing the maximum custom property set, and its view epoch.
    inline constexpr int quota_children = 1024;
    inline constexpr int quota_view_epoch = 99;

    // Frames of the Chromium-compared style-cascade fixture.
    inline constexpr int cascade_fixture_frames = 3;

    // Distinct strings shaped to churn the fallback shape cache past its entry bound.
    inline constexpr int shape_cache_churn_inserts = 1050;
    // Largest accepted SDF error against the independent square-edge distance, in texels.
    inline constexpr float sdf_max_distance_error = 0.65f;

    // Per-channel tolerance of actual Canvas2D pixel checks.
    inline constexpr int canvas_pixel_tolerance = 2;
    // Randomized incremental versus batched snapshot differential on a square canvas.
    inline constexpr unsigned int canvas_batching_seed = 0xce2d2026;
    inline constexpr int canvas_batching_iterations = 256;
    inline constexpr int canvas_batching_size = 32;
    inline constexpr int canvas_batching_rect_size = 4;
    // Random rectangle origins stay below canvas size minus rect size.
    inline constexpr int canvas_batching_origin_range = 28;
    // Iteration periods of resizing, of clearing instead of filling, and of comparing snapshots.
    inline constexpr int canvas_batching_resize_period = 19;
    inline constexpr int canvas_batching_clear_period = 7;
    inline constexpr int canvas_batching_compare_period = 16;
    // Create/fill/collect cycles of the Canvas callback lifecycle stress.
    inline constexpr int canvas_lifecycle_cycles = 50;
} // namespace caseelephant::test_constants::style_text
