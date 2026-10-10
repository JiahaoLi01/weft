#pragma once

#include "constants/common.hpp"

// Limits and encodings of the FreeType/HarfBuzz text probe (src/text/font.cpp): font
// resources, shaping input, the fallback shape cache and SDF glyph generation. Each constant
// has the type of the literal it replaced.
namespace weft::constants::text {
    // Accepted font sizes are (0, max_font_pixels].
    inline constexpr int max_font_pixels = 4096;
    inline constexpr int max_font_file_bytes = 32 * constants::bytes_per_mib;
    // Largest UTF-8 input of single-font shaping.
    inline constexpr int max_shape_input_bytes = constants::bytes_per_mib;
    // Ordered fonts of one fallback set and their total file bytes.
    inline constexpr int max_fallback_fonts = 8;
    inline constexpr int max_font_set_bytes = 64 * constants::bytes_per_mib;

    // FreeType and HarfBuzz positions are 26.6 fixed point.
    inline constexpr int fixed_26_6_units_per_pixel = 64;
    // At 72 dpi FreeType character sizes in points equal pixels.
    inline constexpr int freetype_pixel_dpi = 72;
    // FreeType reports this OS/2 version when the font has no usable OS/2 table.
    inline constexpr int os2_absent_version = 0xffff;
    // OS/2 fsSelection bit 7: USE_TYPO_METRICS.
    inline constexpr int os2_use_typo_metrics = 128;
    // num_grays of FreeType's 8-bit coverage renderer and of its SDF renderer.
    inline constexpr int coverage_num_grays = 256;
    inline constexpr int sdf_num_grays = 255;

    // Explicit bidi formatting characters, rejected until paragraph bidi exists:
    // LRE..RLO embeddings/overrides, LRI..PDI isolates, LRM, RLM and ALM.
    inline constexpr int bidi_embedding_first = 0x202a;
    inline constexpr int bidi_embedding_last = 0x202e;
    inline constexpr int bidi_isolate_first = 0x2066;
    inline constexpr int bidi_isolate_last = 0x2069;
    inline constexpr int left_to_right_mark = 0x200e;
    inline constexpr int right_to_left_mark = 0x200f;
    inline constexpr int arabic_letter_mark = 0x061c;

    // Fallback shape cache budget; the cache is cleared when either bound would be exceeded.
    inline constexpr int shape_cache_max_entries = 1024;
    inline constexpr int shape_cache_max_bytes = 4 * constants::bytes_per_mib;

    // SDF generation: accepted base sizes and spreads, defaults and generated bitmap bound.
    inline constexpr int sdf_min_base_ppem = 16;
    inline constexpr int sdf_max_base_ppem = 128;
    inline constexpr int sdf_min_spread = 2;
    inline constexpr int sdf_max_spread = 32;
    inline constexpr int sdf_default_base_ppem = 40;
    inline constexpr int sdf_default_spread = 8;
    inline constexpr int sdf_max_bitmap_dimension = 2048;
    // FreeType SDF byte for distance zero: distance = (byte - 128) * spread / 128.
    inline constexpr int sdf_zero_distance_byte = 128;
} // namespace weft::constants::text
