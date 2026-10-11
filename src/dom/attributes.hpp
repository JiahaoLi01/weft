#pragma once

#include "base/diagnostic.hpp"
#include "base/result.hpp"

#include <cstdint>
#include <string>

// Values of HTML attributes that native elements read.
namespace weft::dom {
    enum class CanvasSide : std::uint8_t {
        Width,
        Height
    };

    // One side of a canvas element's bitmap from its width or height attribute (null when absent): the attribute
    // under HTML's rules for parsing non-negative integers, or the default 300 by 150 when it is absent or not such an
    // integer. A side above max_canvas_dimension is outside Weft's canvas profile (Diag::CanvasSizeLimit).
    Result<std::uint32_t> canvas_dimension(const std::string* attribute, CanvasSide side);
} // namespace weft::dom
