#pragma once

#include "base/result.hpp"
#include "paint/display_list.hpp"

#include <optional>
#include <string_view>

// backdrop-filter as scene elements take it (Filter Effects 2): none, or filter functions in order: at most one blur()
// (a length in px, its standard deviation) and the color functions brightness(), contrast(), grayscale(), invert(),
// opacity(), saturate() and sepia() (a number or a percentage, 1 when left out; grayscale, invert, opacity and sepia
// capped at 1) and hue-rotate() (an angle in deg, rad, grad or turn). The color functions before the blur make the
// matrix applied before it, those after it the one applied after, each matrix their composition in order: color values
// clamp to [0, 1] once per matrix, not after each function.
namespace weft {
    struct SceneBackdropFilter {
        float blur{};
        paint::ColorMatrix pre = paint::identity_color_matrix;
        paint::ColorMatrix post = paint::identity_color_matrix;

        friend bool operator==(const SceneBackdropFilter&, const SceneBackdropFilter&) = default;
    };

    // nullopt for none (or an empty value). Diag::SceneProfile for a function the scene does not take, a second blur(),
    // a negative or non-finite value, a blur past constants::max_backdrop_blur, or other text.
    Result<std::optional<SceneBackdropFilter>> scene_backdrop_filter(std::string_view value);
} // namespace weft
