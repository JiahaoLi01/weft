#pragma once

#include <array>
#include <optional>
#include <string_view>

// CSS colors as the forgiving profile computes them (CSS Color 4 and 5): every <color> resolves to straight
// (unpremultiplied) gamma-encoded sRGB with alpha, which is what the scene draws. Colors outside the sRGB gamut are
// clipped channel by channel, as browsers on sRGB displays draw them.
namespace weft::color {
    // Gamma-encoded sRGB red, green, blue (0 to 1; outside before clip()) and alpha (0 to 1).
    using Rgba = std::array<double, 4>;

    // A named color (CSS Color 4 §6.1) by its ASCII lowercase name; transparent is one.
    std::optional<Rgba> named(std::string_view name);

    // The color spaces of color(), the color functions and color-mix(). Polar spaces keep their hue in degrees.
    enum class Space {
        Srgb,
        SrgbLinear,
        DisplayP3,
        A98Rgb,
        ProphotoRgb,
        Rec2020,
        XyzD50,
        XyzD65,
        Lab,
        Lch,
        Oklab,
        Oklch,
        Hsl,
        Hwb
    };

    // The space a color() or color-mix() names (ASCII lowercase): srgb, srgb-linear, display-p3, a98-rgb,
    // prophoto-rgb, rec2020, xyz (xyz-d65), xyz-d50, lab, lch, oklab, oklch, hsl, hwb.
    std::optional<Space> space(std::string_view name);

    // Whether a space is polar (its third component, or first for hsl and hwb, is a hue).
    bool polar(Space space);
    // The index of a polar space's hue component.
    int hue_index(Space space);

    // A color's components in a space, from gamma-encoded sRGB (red, green, blue) and back. Hsl and hwb components
    // are the CSS ones: hue in degrees, the others 0 to 100; lab and lch L 0 to 100; oklab and oklch L 0 to 1.
    std::array<double, 3> from_srgb(Space space, const std::array<double, 3>& srgb);
    std::array<double, 3> to_srgb(Space space, const std::array<double, 3>& components);

    // How color-mix() interpolates a hue (CSS Color 4 §12.4).
    enum class HueInterpolation {
        Shorter,
        Longer,
        Increasing,
        Decreasing
    };

    // color-mix(in space, a weight_a, b weight_b) with weights summing to 1 (CSS Color 5 §2.4): premultiplied
    // interpolation in the space, hues by the hue interpolation; the result's alpha is not yet scaled by a weight sum
    // below 1 (the caller does).
    Rgba mix(Space space, HueInterpolation hue, const Rgba& a, double weight_a, const Rgba& b, double weight_b);

    // Clipped into the sRGB gamut and alpha into 0 to 1.
    Rgba clip(Rgba color);
} // namespace weft::color
