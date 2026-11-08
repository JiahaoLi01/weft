#pragma once

// Values used by the script, owner-executor, animation-frame and trace tests.
// Each constant has the type of the literal it replaced (see docs/coding-style.md).
namespace weft::test_constants::script {
    // Live nodes of a fresh document: #document, <html> and <body>.
    inline constexpr int bootstrap_document_nodes = 3;

    // Host timestamps. This fraction is the visible Canvas host counterexample: std::to_string
    // rounds it UP, so a six-decimal timer stamp lands after the full-precision rAF time.
    inline constexpr double legacy_rounding_time_ms = .2500006;
    // Deliberately irregular frame intervals, so timestamps exercise full double precision.
    inline constexpr double clock_frame_interval_ms = 16.123456789;
    inline constexpr double scheduler_frame_interval_ms = 3.123456789;

    // Execution budget that a spinning timer callback must exceed.
    inline constexpr int spin_execution_budget_ms = 300;

    // Stress lengths and periods.
    inline constexpr int lifetime_pressure_rounds = 24;
    inline constexpr int reset_cycles = 12;
    inline constexpr int scheduler_differential_frames = 320;
    inline constexpr int hidden_frame_period = 13;
    inline constexpr int scheduler_gc_period = 17;
    inline constexpr int mutation_differential_steps = 512;
    inline constexpr int mutation_gc_period = 16;
    // Inline style declarations against the JS reference (inline_style_tests): random runs and their steps.
    inline constexpr unsigned inline_style_seeds = 24;
    inline constexpr int inline_style_steps = 1500;

    // Owner executor differential: inputs, publication wait period and owner GC period.
    inline constexpr int owner_differential_inputs = 512;
    inline constexpr int owner_wait_period = 16;
    inline constexpr int owner_gc_input_period = 13;
    // A game-thread queue call must return within this while a frame is being retired.
    // A wait() that must not return during a progressive tick that is still blocked.
    inline constexpr int owner_early_wait_ms = 50;
    inline constexpr int queue_api_timeout_seconds = 1;

    // Owner trace event limit (65536) plus four events that must evict the oldest ones.
    inline constexpr int trace_ring_overflow_events = 65540;
} // namespace weft::test_constants::script
