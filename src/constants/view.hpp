#pragma once

#include <cstddef>
#include <cstdint>

#include "constants/script.hpp"

// A View (src/view, docs/engine-communication.md section 8): its VM and what it keeps for the host.
namespace weft::constants::view {
    // The VM's stack: React and D3 bundles recurse deeply. The owner thread needs native stack beyond it (the host
    // gives the thread at least owner_stack_bytes).
    inline constexpr std::size_t vm_stack_bytes = std::size_t(constants::script::max_vm_stack_bytes);
    inline constexpr std::size_t owner_stack_bytes = 8 * std::size_t(constants::bytes_per_mib);
    // One script evaluation, task batch, animation frame or delivery.
    inline constexpr std::uint32_t execution_budget_ms = std::uint32_t(constants::script::max_execution_budget_ms);
    // Task batches an advance runs before it reports a page whose tasks keep scheduling tasks.
    inline constexpr unsigned max_task_turns = unsigned(constants::script::max_task_pump_turns);
    // Diagnostics kept until the host takes them (later ones are counted, not kept), and the bytes of one message.
    inline constexpr std::size_t max_diagnostics = 256;
    inline constexpr std::size_t max_diagnostic_bytes = 4096;
    // Where a pointer that left the View is: outside every element.
    inline constexpr float outside = -1e6f;
} // namespace weft::constants::view
