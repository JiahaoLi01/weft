#pragma once

#include <cstddef>

#include "constants/common.hpp"

// The engine bridge (src/bridge, docs/engine-communication.md section 6): what a value, a name and a View's queues may
// hold.
namespace weft::constants::bridge {
    // Bytes of a name (an event, call, function or view model): 1 to this many, UTF-8.
    inline constexpr std::size_t max_name_bytes = 256;
    // Bytes of one value, and how deep arrays and maps nest in one.
    inline constexpr std::size_t max_value_bytes = std::size_t(constants::bytes_per_mib);
    inline constexpr unsigned max_value_depth = 64;
    // What a View's channel holds for its page before the owner delivers it: events and call completions, and the
    // bytes of their values and of view-model fields.
    inline constexpr std::size_t max_pending_inputs = 4096;
    inline constexpr std::size_t max_pending_bytes = 16 * std::size_t(constants::bytes_per_mib);
    // Calls a page may have open at once, and the messages and their bytes it may send before the host takes them.
    inline constexpr std::size_t max_open_calls = 4096;
    inline constexpr std::size_t max_pending_messages = 4096;
    inline constexpr std::size_t max_pending_message_bytes = 16 * std::size_t(constants::bytes_per_mib);
    // A map or array header written before its count is known: the 32-bit form, patched once the count is.
    inline constexpr std::size_t container_header_bytes = 5;
} // namespace weft::constants::bridge
