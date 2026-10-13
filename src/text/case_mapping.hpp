#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Case mapping for CSS text-transform: Unicode's full case mappings (SpecialCasing included, so one character may
// become several, and lowercasing's Final_Sigma condition; no language tailoring), from QuickJS-NG's Unicode tables.
namespace weft {
    enum class TextCase : std::uint8_t {
        Upper,
        Lower,
    };

    // The UTF-8 text in upper or lower case. Bytes that are not UTF-8 stay as they are.
    std::string case_mapped(std::string_view utf8, TextCase to);
} // namespace weft
