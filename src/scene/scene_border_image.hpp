#pragma once

#include "style/values.hpp"

#include <array>
#include <vector>

// border-image (CSS Backgrounds 3 §6) as the scene draws it: the image (border-image-source, a url() image) sliced in
// nine by border-image-slice and drawn over the border image area (the border box grown by border-image-outset)
// instead of the border styles. The outer pieces are as wide as border-image-width says (scaled down together when
// opposite ones would overlap), each piece stretched over its place (border-image-repeat: stretch; repeat, round and
// space are drawn as stretch), the middle one only with `fill`. The border's widths still lay the box out.
namespace weft {
    struct SceneBorderImagePiece {
        // In image px: x, y, width, height.
        std::array<float, 4> source{};
        // In the box's px from its border box's top-left: x, y, width, height.
        std::array<float, 4> target{};

        friend bool operator==(const SceneBorderImagePiece&, const SceneBorderImagePiece&) = default;
    };

    // The pieces of an image of image_width x image_height px over a border box of box_width x box_height px, laid out
    // by the style's border widths (border-*-width, as the scene computes them) and border-image-slice, -width, -outset;
    // pieces of no area are left out. Throws std::invalid_argument for a value outside the profile: slices of numbers
    // and percentages (and fill), widths of numbers, px, percentages and auto, outsets of numbers and px.
    std::vector<SceneBorderImagePiece> scene_border_image(const StyleValues& style, float box_width, float box_height,
                                                          float image_width, float image_height);

    // Whether border-image-repeat is stretch on both axes, as the scene draws every keyword.
    bool scene_border_image_stretches(const StyleValues& style);
} // namespace weft
