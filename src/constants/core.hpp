#pragma once

#include <cstdint>

// Limits and defaults of the experimental M0 runtime C ABI (src/core/runtime.cpp).
namespace weft::constants::core {
    // View slots allocated when weft_runtime_create receives a null descriptor.
    inline constexpr std::uint32_t default_max_views = 64;
    // Largest max_views a host may request; bounds the fixed slot table allocation.
    inline constexpr std::uint32_t hard_max_views = 65536;

    // Logical size of a View created with a null weft_view_desc. This is the C ABI default,
    // not the 640x420 probe view in common.hpp.
    inline constexpr int default_view_logical_width = 640;
    inline constexpr int default_view_logical_height = 480;
} // namespace weft::constants::core
