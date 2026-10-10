#pragma once

#include "constants/common.hpp"
#include "constants/script.hpp"

#include <array>

// Test-only values of the layout, page and input tests. Each constant has the type of the
// literal it replaced.
namespace weft::test_constants::layout {
    // Native stack reserved for the JS VM by the React page/input tests.
    inline constexpr int script_stack_bytes = weft::constants::script::probe_vm_stack_bytes;
    // Wall-time budget per eval while WEFT_REACT_GC_STRESS collects at every allocation.
    inline constexpr int gc_stress_execution_budget_ms = weft::constants::script::max_execution_budget_ms;
    // __ceHost.pump rounds before host tasks must have drained.
    inline constexpr int max_pump_rounds = 32;
    // Full React bootstrap/GC/reset cycles of the page probe test.
    inline constexpr int react_page_cycles = 8;

    // Mutation frames of the auto, flow and flow-input fixtures.
    inline constexpr int fixture_frames = 3;
    // Standalone scenes reported per frame by the auto-layout and flow-layout fixtures.
    inline constexpr int auto_fixture_scenes = 16;
    inline constexpr int flow_fixture_scenes = 8;
    // Create/scroll/detach/reset cycles of the scroll geometry lifecycle test.
    inline constexpr int scroll_lifecycle_cycles = 16;

    // Geometry tolerances.
    inline constexpr float document_geometry_tolerance = .002f;
    inline constexpr double geometry_tolerance = .001;
    inline constexpr double scale_tolerance = .00001;
    inline constexpr double fps_tolerance = .001;

    // Flow-input hit grid: one sample at the centre of every 16 px cell.
    inline constexpr int hit_grid_step = 16;
    inline constexpr int hit_grid_first = 8;
    // Per frame, the centres of the #outer and #inner inline fragments' hit regions at Chromium capture time
    // (tests/fixtures/flow_input_browser_reference.json). Fixed so a layout change cannot move the oracle points.
    inline constexpr std::array<std::array<std::array<double, 2>, 3>, 3> flow_input_fragment_points{{
        {{{383.969, 122}, {349.734, 146}, {349.734, 146}}},
        {{{395.969, 122}, {362.164, 147.5}, {362.164, 147.5}}},
        {{{395.969, 122}, {362.164, 147.5}, {362.164, 147.5}}},
    }};
    // Deterministic scroll/z-order mutations of the flow-input oracle.
    inline constexpr unsigned flow_input_seed = 0xCE802026u;
    inline constexpr int flow_input_mutations = 64;
} // namespace weft::test_constants::layout
