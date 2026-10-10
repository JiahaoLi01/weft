#pragma once

// Values of the shared test and probe support code (tests/support/).
namespace weft::test_constants::support {
    inline constexpr int microseconds_per_millisecond = 1000;
    // Chrome trace thread id of the UI owner; the render thread uses the next id.
    inline constexpr int owner_trace_tid = 1;
    // Each trace file retains its latest events up to this bound.
    inline constexpr int owner_trace_event_limit = 65536;
    // Digits that round-trip a double trace timestamp.
    inline constexpr int trace_timestamp_digits = 17;
} // namespace weft::test_constants::support
