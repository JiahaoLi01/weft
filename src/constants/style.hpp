#pragma once

#include "constants/common.hpp"

// Bounds of the M0 author-style evaluator (src/style/style.cpp): parser and cascade
// work quotas plus the StyleMemo cache budget. Each constant has the type of the literal
// it replaced.
namespace weft::constants::style {
    // One past the last ASCII code point. Raw non-ASCII bytes outside the scene profile and
    // selector escapes at or above it are rejected.
    inline constexpr int ascii_end = 128;

    // Lexer and serializer bounds per declaration value.
    inline constexpr int max_function_depth = 32;
    inline constexpr int max_value_tokens = 4096;
    inline constexpr int max_resolved_tokens = 4096;
    inline constexpr int max_resolved_value_bytes = 8192;
    inline constexpr int max_declaration_bytes = 8192;
    // Rules per stylesheet, counting each selector of a selector list.
    inline constexpr int max_rules = 256;
    // "#rrggbb", the only hex color form in the profile.
    inline constexpr int hex_color_length = 7;

    // Cascade rank: importance, origin, inline, three specificity parts, then source order.
    inline constexpr int cascade_rank_size = 7;
    inline constexpr int cascade_rank_order_index = 6;

    // Evaluation quotas of one style computation.
    inline constexpr int max_token_work = 1000000;
    inline constexpr int max_computed_output_bytes = 16 * constants::bytes_per_mib;
    inline constexpr int max_custom_properties = 128;
    inline constexpr int max_registered_properties = 128;

    // Per-cache bounds of StyleMemo (rule winners and computed values each).
    inline constexpr int memo_max_entries = 2048;
    inline constexpr int memo_max_bytes = 2 * constants::bytes_per_mib;
} // namespace weft::constants::style
