#include "text/color_canvas.hpp"
#include "support/check.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <numbers>

// The color glyph canvas: exact-area clips of outlines and rectangles under transforms, the COLRv1 gradients and
// extend modes interpolating premultiplied colors, the composite modes of groups, and a real COLRv1 glyph.
namespace {
    using weft::testing::require;
    using Canvas = weft::ColorCanvas;
    using Color = Canvas::Color;
    constexpr Canvas::Matrix identity{1, 0, 0, 1, 0, 0};

    Color at(const Canvas& canvas, const std::vector<std::uint8_t>& pixels, unsigned x, unsigned y) {
        const auto* p = pixels.data() + (std::size_t(y) * canvas.width() + x) * 4;
        return {p[0] / 255.f, p[1] / 255.f, p[2] / 255.f, p[3] / 255.f};
    }

    bool near(const Color& a, const Color& b, float tolerance = 1.5f / 255) {
        for (int c = 0; c < 4; ++c) {
            if (std::abs(a[c] - b[c]) > tolerance) {
                return false;
            }
        }
        return true;
    }

    // A circle of radius r about (cx, cy) in four cubics.
    weft::GlyphOutline circle(float cx, float cy, float r) {
        const float k = .5522847498f * r;
        weft::GlyphOutline o;
        using Verb = weft::GlyphOutline::Verb;
        o.verbs = {Verb::Move, Verb::Cubic, Verb::Cubic, Verb::Cubic, Verb::Cubic, Verb::Close};
        o.points = {{cx + r, cy},     {cx + r, cy + k}, {cx + k, cy + r}, {cx, cy + r},     {cx - k, cy + r},
                    {cx - r, cy + k}, {cx - r, cy},     {cx - r, cy - k}, {cx - k, cy - r}, {cx, cy - r},
                    {cx + k, cy - r}, {cx + r, cy - k}, {cx + r, cy}};
        return o;
    }

    void clips() {
        // A rectangle covers whole pixels inside it and the covered fraction of a pixel it cuts.
        Canvas c(8, 8, identity);
        c.push_clip(std::array<double, 4>{2, 2, 6.5, 6});
        c.fill({1, 0, 0, 1});
        auto p = c.pixels();
        require(near(at(c, p, 3, 3), {1, 0, 0, 1}) && near(at(c, p, 1, 3), {}) &&
                    near(at(c, p, 6, 3), {.5f, 0, 0, .5f}),
                "rectangle clip coverage");
        // Nested clips multiply their coverage; popping restores the outer clip.
        Canvas nested(4, 4, identity);
        nested.push_clip(std::array<double, 4>{0, 0, 1.5, 4});
        nested.push_clip(std::array<double, 4>{0, 0, 4, 1.5});
        nested.fill({0, 0, 1, 1});
        nested.pop_clip();
        nested.push_clip(std::array<double, 4>{0, 3, 4, 4});
        nested.fill({0, 1, 0, 1});
        p = nested.pixels();
        require(near(at(nested, p, 1, 1), {0, 0, .25f, .25f}) && near(at(nested, p, 1, 3), {0, .5f, 0, .5f}) &&
                    near(at(nested, p, 3, 3), {}),
                "nested clips");
        // An outline covers by exact area: a disc of radius 10 covers pi r^2 pixels (the cubics' own area is
        // 0.03% larger), and an edge outside the canvas still covers the pixels right of it.
        Canvas disc(24, 24, identity);
        disc.push_clip(circle(12, 12, 10));
        disc.fill({1, 1, 1, 1});
        double area{};
        for (const auto v : disc.pixels()) {
            area += v;
        }
        area /= 255 * 4;
        require(std::abs(area / (std::numbers::pi * 100) - 1) < .002, "disc area");
        Canvas wide(4, 2, identity);
        weft::GlyphOutline slanted;
        using Verb = weft::GlyphOutline::Verb;
        slanted.verbs = {Verb::Move, Verb::Line, Verb::Line, Verb::Line, Verb::Close};
        slanted.points = {{-2, 0}, {6, 0}, {6, 2}, {-1, 2}};
        wide.push_clip(slanted);
        wide.fill({1, 1, 1, 1});
        p = wide.pixels();
        require(near(at(wide, p, 0, 0), {1, 1, 1, 1}) && near(at(wide, p, 3, 1), {1, 1, 1, 1}),
                "edges outside the canvas");
        // An edge entering the canvas inside a pixel row: left of x = 0 it runs along the canvas's side, so the
        // first pixel is three quarters covered.
        Canvas crossing(4, 1, identity);
        weft::GlyphOutline entering;
        entering.verbs = {Verb::Move, Verb::Line, Verb::Line, Verb::Line, Verb::Close};
        entering.points = {{-1, 0}, {1, 1}, {4, 1}, {4, 0}};
        crossing.push_clip(entering);
        crossing.fill({1, 1, 1, 1});
        p = crossing.pixels();
        require(near(at(crossing, p, 0, 0), {.75f, .75f, .75f, .75f}), "an edge entering the canvas");
        // Transforms compose: a rectangle in a space scaled by 2 and moved by 1 covers twice its size.
        Canvas moved(8, 8, {1, 0, 0, 1, 1, 0});
        moved.push_transform({2, 0, 0, 2, 0, 0});
        moved.push_clip(std::array<double, 4>{1, 1, 2, 2});
        moved.fill({1, 1, 1, 1});
        p = moved.pixels();
        require(near(at(moved, p, 3, 2), {1, 1, 1, 1}) && near(at(moved, p, 4, 3), {1, 1, 1, 1}) &&
                    near(at(moved, p, 2, 2), {}) && near(at(moved, p, 5, 2), {}),
                "transformed clip");
        // A degenerate space paints nothing.
        Canvas flat(4, 4, identity);
        flat.push_transform({0, 0, 0, 0, 0, 0});
        flat.fill({1, 1, 1, 1});
        p = flat.pixels();
        require(near(at(flat, p, 1, 1), {}), "degenerate space");
    }

    void gradients() {
        using Extend = Canvas::Extend;
        const std::vector<Canvas::Stop> line{{0, {1, 0, 0, 1}}, {1, {0, 0, 1, 1}}};
        // Extend modes past the ends of the color line; premultiplied interpolation keeps a fading red red.
        require(near(weft::color_line(line, Extend::Pad, 1.25), {0, 0, 1, 1}) &&
                    near(weft::color_line(line, Extend::Repeat, 1.25), {.75f, 0, .25f, 1}) &&
                    near(weft::color_line(line, Extend::Reflect, 1.25), {.25f, 0, .75f, 1}) &&
                    near(weft::color_line(line, Extend::Reflect, -.25), {.75f, 0, .25f, 1}),
                "extend modes");
        require(near(weft::color_line({{0, {1, 0, 0, 1}}, {1, {0, 0, 1, 0}}}, Extend::Pad, .5), {.5f, 0, 0, .5f}),
                "premultiplied interpolation");
        // Linear: t along p0p1 projected across p0p2; colors constant along p0p2.
        Canvas linear(8, 8, identity);
        linear.linear_gradient(line, Extend::Pad, {0, 0}, {8, 0}, {0, 8});
        auto p = linear.pixels();
        require(near(at(linear, p, 3, 0), {1 - 3.5f / 8, 0, 3.5f / 8, 1}) &&
                    near(at(linear, p, 3, 6), at(linear, p, 3, 0)),
                "linear gradient");
        Canvas turned(8, 8, identity);
        turned.linear_gradient(line, Extend::Pad, {0, 0}, {8, 0}, {8, 8});
        p = turned.pixels();
        require(near(at(turned, p, 4, 2), at(turned, p, 5, 3)) && near(at(turned, p, 2, 1), at(turned, p, 6, 5)) &&
                    !near(at(turned, p, 4, 2), at(turned, p, 5, 2)),
                "linear gradient turned by p2");
        // Radial between concentric circles: t is the distance over the outer radius.
        Canvas radial(8, 8, identity);
        radial.radial_gradient(line, Extend::Pad, {4, 4}, 0, {4, 4}, 4);
        p = radial.pixels();
        const float d = std::hypot(2.5f, .5f) / 4;
        require(near(at(radial, p, 6, 4), {1 - d, 0, d, 1}), "radial gradient");
        // Two circles: points outside every circle with a non-negative radius stay transparent.
        Canvas cone(8, 8, identity);
        cone.radial_gradient(line, Extend::Pad, {2, 4}, 1, {6, 4}, 1);
        p = cone.pixels();
        // At (4.5, 4.5) the circles of t = 0.4085 and t = 0.8415 pass: the larger wins.
        require(near(at(cone, p, 4, 0), {}) && near(at(cone, p, 4, 4), {1 - .8415f, 0, .8415f, 1}),
                "two-circle gradient");
        // Sweep: the angle from the space's x axis towards its y axis.
        Canvas sweep(8, 8, identity);
        sweep.sweep_gradient(line, Extend::Pad, {4, 4}, 0, 2 * std::numbers::pi);
        p = sweep.pixels();
        const float quarter = float(std::atan2(2.5, .5) / (2 * std::numbers::pi));
        require(near(at(sweep, p, 4, 6), {1 - quarter, 0, quarter, 1}), "sweep gradient");
    }

    void groups() {
        using Composite = Canvas::Composite;
        const auto composite = [](Composite mode, Color backdrop, Color source) {
            Canvas c(1, 1, identity);
            c.fill(backdrop);
            c.push_group();
            c.fill(source);
            c.pop_group(mode);
            return at(c, c.pixels(), 0, 0);
        };
        const Color red{1, 0, 0, 1};
        const Color half_blue{0, 0, 1, .5f};
        require(near(composite(Composite::SrcOver, red, half_blue), {.5f, 0, .5f, 1}), "source over");
        require(near(composite(Composite::DestOut, red, half_blue), {.5f, 0, 0, .5f}), "destination out");
        require(near(composite(Composite::SrcIn, {1, 0, 0, .5f}, half_blue), {0, 0, .25f, .25f}), "source in");
        require(near(composite(Composite::Clear, red, half_blue), {}), "clear");
        require(near(composite(Composite::Multiply, {1, .5f, 1, 1}, {.5f, 1, 1, 1}), {.5f, .5f, 1, 1}), "multiply");
        require(near(composite(Composite::Screen, {.5f, 0, 0, 1}, {.5f, 0, 0, 1}), {.75f, 0, 0, 1}), "screen");
        require(near(composite(Composite::Plus, {1, 0, 0, .5f}, {1, 0, 0, .75f}), {1, 0, 0, 1}), "plus");
        // Overlay is hard light with the layers swapped: a dark backdrop multiplies.
        require(
            near(composite(Composite::Overlay, {.25f, .25f, .25f, 1}, {.75f, .75f, .75f, 1}), {.375f, .375f, .375f, 1}),
            "overlay");
        // Luminosity keeps the backdrop's hue: a gray source over red is a red of the gray's luminosity.
        const auto lum = composite(Composite::Luminosity, red, {.5f, .5f, .5f, 1});
        require(std::abs(.3f * lum[0] + .59f * lum[1] + .11f * lum[2] - .5f) < .01f && lum[0] > lum[1], "luminosity");
        // Outside the current clip a composite leaves the backdrop.
        Canvas clipped(2, 1, identity);
        clipped.fill(red);
        clipped.push_clip(std::array<double, 4>{0, 0, 1, 1});
        clipped.push_group();
        clipped.pop_group(Composite::Clear);
        clipped.pop_clip();
        const auto p = clipped.pixels();
        require(near(at(clipped, p, 0, 0), {}) && near(at(clipped, p, 1, 0), red), "composite inside the clip");
        // Groups nested past the bound are not made; the canvas reports it and stays balanced.
        Canvas deep(1, 1, identity);
        for (int i = 0; i < 40; ++i) {
            deep.push_group();
        }
        deep.fill(red);
        for (int i = 0; i < 40; ++i) {
            deep.pop_group(Composite::SrcOver);
        }
        require(deep.overflowed() && near(at(deep, deep.pixels(), 0, 0), red), "group bound");
    }

    // A COLRv1 emoji of the installed Segoe UI Emoji: its paint has several hues inside the em, and a plain
    // outline glyph of the same face paints in the foreground color.
    void emoji() {
        const std::filesystem::path path = "C:/Windows/Fonts/seguiemj.ttf";
        if (!std::filesystem::exists(path)) {
            std::cout << "skip: no Segoe UI Emoji\n";
            return;
        }
        weft::Font font(path);
        const auto fire = font.glyph_index(0x1F525);
        const auto image = font.rasterize_color(fire, 48, .25f, .5f, {0, 0, 0, 1});
        require(fire && image && image->width >= 40 && image->width <= 60 && image->height >= 40 &&
                    image->height <= 60 && image->top > 30,
                "emoji image");
        unsigned red{};
        unsigned yellow{};
        for (std::size_t i = 0; i < image->rgba.size(); i += 4) {
            const auto* q = image->rgba.data() + i;
            red += q[3] > 200 && q[0] > 180 && q[1] < 120;
            yellow += q[3] > 200 && q[0] > 180 && q[1] > 160 && q[2] < 140;
        }
        require(red > 50 && yellow > 50, "emoji colors");
        // The subpixel origin moves the paint: the ink's center moves by the offset.
        const auto center = [](const weft::ColorGlyphBitmap& b) {
            double sum{};
            double moment{};
            for (std::uint32_t y = 0; y < b.height; ++y) {
                for (std::uint32_t x = 0; x < b.width; ++x) {
                    const double a = b.rgba[(std::size_t(y) * b.width + x) * 4 + 3];
                    sum += a;
                    moment += a * (x + .5 + b.left);
                }
            }
            return moment / sum;
        };
        const auto still = font.rasterize_color(fire, 48, 0, 0, {0, 0, 0, 1});
        const auto shifted = font.rasterize_color(fire, 48, .5f, 0, {0, 0, 0, 1});
        require(std::abs(center(*shifted) - center(*still) - .5) < .02, "subpixel origin");
        const auto digit = font.rasterize_color(font.glyph_index(U'#'), 48, 0, 0, {0, .5f, 0, 1});
        require(digit && !digit->rgba.empty(), "outline glyph of a color face");
        bool green = true;
        for (std::size_t i = 0; i < digit->rgba.size(); i += 4) {
            const auto* q = digit->rgba.data() + i;
            green &= q[0] == 0 && q[2] == 0 && std::abs(int(q[1]) * 2 - int(q[3])) <= 2;
        }
        require(green, "outline glyph paints the foreground");
    }
} // namespace

int main() {
    try {
        clips();
        gradients();
        groups();
        emoji();
        std::cout << "PASS color canvas: clips, gradients, extend modes, composites, group bound, COLRv1 glyph\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
