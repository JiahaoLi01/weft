#pragma once

#include "text/font.hpp"

#include <array>
#include <cstdint>
#include <vector>

// A software canvas for color glyphs (COLRv1 paint graphs and COLRv0 layers): fills of solid colors and of linear,
// radial and sweep gradients inside nested clips of outlines and rectangles, in nested groups composited by the COLRv1
// modes. Colors are encoded sRGB, premultiplied in floats and blended in that space, as Chrome and DirectWrite blend
// color glyphs; gradients interpolate premultiplied colors, as the OpenType COLR table specifies. Clips cover by exact
// area: curves flatten within constants::text::color_flattening_tolerance pixels.
namespace weft {
    class ColorCanvas {
    public:
        // An affine map {xx, yx, xy, yy, dx, dy}: (x, y) -> (xx x + xy y + dx, yx x + yy y + dy).
        using Matrix = std::array<double, 6>;
        using Point = std::array<double, 2>;
        using Color = std::array<float, 4>;

        // The COLRv1 composite modes, in the order of the OpenType CompositeMode values.
        enum class Composite : std::uint8_t {
            Clear,
            Src,
            Dest,
            SrcOver,
            DestOver,
            SrcIn,
            DestIn,
            SrcOut,
            DestOut,
            SrcAtop,
            DestAtop,
            Xor,
            Plus,
            Screen,
            Overlay,
            Darken,
            Lighten,
            ColorDodge,
            ColorBurn,
            HardLight,
            SoftLight,
            Difference,
            Exclusion,
            Multiply,
            Hue,
            Saturation,
            Color,
            Luminosity
        };

        // How a color line continues past its first and last stops.
        enum class Extend : std::uint8_t {
            Pad,
            Repeat,
            Reflect
        };

        // A color stop: its offset on the color line and its straight color.
        struct Stop {
            float offset{};
            Color color{};
        };

        // A width x height pixel canvas whose paint space maps to pixels through base (y down, pixel centers at half
        // pixels). Throws std::invalid_argument past constants::text::max_color_glyph_pixels.
        ColorCanvas(std::uint32_t width, std::uint32_t height, const Matrix& base);

        // Points of m's space map through m, then through the current map, until the matching pop.
        void push_transform(const Matrix& m);
        void pop_transform();
        // Clips to an outline of the current space (nonzero, or even-odd by its flag), or to the rectangle
        // {x0, y0, x1, y1} there, inside the current clip.
        void push_clip(const GlyphOutline& outline);
        void push_clip(const std::array<double, 4>& rect);
        void pop_clip();
        // Paints the current clip of the current group, source over, with a straight color.
        void fill(const Color& color);
        // The COLRv1 gradients, of points in the current space: linear from p0 towards p1, its color lines parallel
        // to p0p2; radial between the circles (c0, r0) and (c1, r1), the largest parameter winning; sweep about
        // center from start to end, in radians from the space's x axis towards its y axis.
        void linear_gradient(std::vector<Stop> stops, Extend extend, Point p0, Point p1, Point p2);
        void radial_gradient(std::vector<Stop> stops, Extend extend, Point c0, double r0, Point c1, double r1);
        void sweep_gradient(std::vector<Stop> stops, Extend extend, Point center, double start, double end);
        // Paints into a new transparent group until the matching pop composites it onto the group below by mode.
        // Groups nested past constants::text::max_color_groups are not made: their paint lands in the group below
        // and overflowed() turns true. (Paint callbacks of C libraries call these, so they do not throw.)
        void push_group();
        void pop_group(Composite mode);

        bool overflowed() const {
            return skipped_groups_ || overflowed_;
        }

        std::uint32_t width() const {
            return width_;
        }

        std::uint32_t height() const {
            return height_;
        }

        // The base group as premultiplied encoded sRGB RGBA8.
        std::vector<std::uint8_t> pixels() const;

    private:
        std::uint32_t width_{};
        std::uint32_t height_{};
        std::vector<Matrix> transforms_;
        // Coverage of the current clip, innermost last.
        std::vector<std::vector<float>> clips_;
        // Premultiplied colors of each group, innermost last.
        std::vector<std::vector<Color>> groups_;
        // Groups pushed past the bound and not made, and whether any was.
        std::size_t skipped_groups_{};
        bool overflowed_{};

        // Coverage of polylines of pixel points.
        std::vector<float> coverage(const std::vector<std::vector<Point>>& polygons, bool evenodd) const;
        void intersect(const std::vector<float>& coverage);
        // Paints the current clip source over with the premultiplied color of each pixel center p (in the current
        // space): shade(p), or nothing when shade leaves it transparent.
        template <typename Shade>
        void paint(Shade shade);
    };

    // The premultiplied color of a color line at t: past its ends as extend continues it, between stops (sorted by
    // offset) interpolated premultiplied.
    ColorCanvas::Color color_line(const std::vector<ColorCanvas::Stop>& stops, ColorCanvas::Extend extend, double t);
} // namespace weft
