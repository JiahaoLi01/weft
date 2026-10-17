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

    // Cascade rank: importance, origin, inline, cascade layer, three specificity parts, then source order.
    inline constexpr int cascade_rank_size = 8;
    inline constexpr int cascade_rank_layer_index = 3;
    inline constexpr int cascade_rank_order_index = 7;

    // Evaluation quotas of one style computation.
    inline constexpr int max_token_work = 1000000;
    inline constexpr int max_computed_output_bytes = 16 * constants::bytes_per_mib;
    inline constexpr int max_custom_properties = 128;
    inline constexpr int max_registered_properties = 256;

    // Bounds of style sheets taken as browsers take them (StyleProfile::forgiving), whose generated utilities run to
    // thousands of rules: bytes and rules of a sheet, cascade layers, the custom properties of an element (a theme on
    // the root holds hundreds) and @property registrations, and the warnings a sheet and its cascades keep.
    inline constexpr int max_forgiving_stylesheet_bytes = 8 * constants::bytes_per_mib;
    inline constexpr int max_forgiving_rules = 16384;
    inline constexpr int max_forgiving_custom_properties = 2048;
    inline constexpr int max_forgiving_layers = 256;
    inline constexpr int max_registered_custom_properties = 1024;
    inline constexpr int max_style_warnings = 256;
    // At-rules nest at most this deep (@layer in @media in @supports...).
    inline constexpr int max_at_rule_depth = 16;

    // Per-cache bounds of StyleMemo (rule winners and computed values each).
    inline constexpr int memo_max_entries = 2048;
    inline constexpr int memo_max_bytes = 2 * constants::bytes_per_mib;
} // namespace weft::constants::style
