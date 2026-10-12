#include "constants/render.hpp"
#include "support/check.hpp"
#include "vector/path_raster.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>

// Paths as coverage: fills cover their geometry's area, fill rules differ on overlaps, strokes reach their width with
// the caps and joins each style names, dashes alternate from their offset and restart per subpath, a map stretches a
// stroke, and the mask bounds hold every covered pixel.
namespace {
    using weft::testing::require;
    namespace paint = weft::paint;
    using Verb = paint::Path::Verb;
    using Cap = paint::Stroke::Cap;
    using Join = paint::Stroke::Join;

    paint::Path polyline(std::initializer_list<std::array<float, 2>> points, bool close) {
        paint::Path p;
        for (const auto& point : points) {
            p.verbs.push_back(p.verbs.empty() ? Verb::Move : Verb::Line);
            p.points.push_back(point);
        }
        if (close) {
            p.verbs.push_back(Verb::Close);
        }
        return p;
    }

    void append(paint::Path& into, const paint::Path& from) {
        into.verbs.insert(into.verbs.end(), from.verbs.begin(), from.verbs.end());
        into.points.insert(into.points.end(), from.points.begin(), from.points.end());
    }

    // A circle of four cubics.
    paint::Path circle(float cx, float cy, float r) {
        constexpr float kappa = .5522847498f;
        const float k = r * kappa;
        paint::Path p;
        p.verbs = {Verb::Move, Verb::Cubic, Verb::Cubic, Verb::Cubic, Verb::Cubic, Verb::Close};
        p.points = {{cx + r, cy},     {cx + r, cy + k}, {cx + k, cy + r}, {cx, cy + r},     {cx - k, cy + r},
                    {cx - r, cy + k}, {cx - r, cy},     {cx - r, cy - k}, {cx - k, cy - r}, {cx, cy - r},
                    {cx + k, cy - r}, {cx + r, cy - k}, {cx + r, cy}};
        return p;
    }

    paint::PathPaint fill(paint::Path p) {
        return {std::make_shared<const paint::Path>(std::move(p)), {1, 1, 1, 1}, std::nullopt};
    }

    paint::PathPaint stroke(paint::Path p, float width, Cap cap = Cap::Butt, Join join = Join::Miter) {
        paint::Stroke s;
        s.width = width;
        s.cap = cap;
        s.join = join;
        return {std::make_shared<const paint::Path>(std::move(p)), {1, 1, 1, 1}, s};
    }

    struct Drawn {
        weft::vector::Mask mask;

        // Coverage of the device pixel (x, y); zero outside the mask.
        int at(int x, int y) const {
            if (x < mask.x || y < mask.y || x >= mask.x + int(mask.width) || y >= mask.y + int(mask.height)) {
                return 0;
            }
            return mask.coverage[std::size_t(y - mask.y) * mask.width + std::size_t(x - mask.x)];
        }

        // Covered area in pixels.
        double area() const {
            double sum{};
            for (auto c : mask.coverage) {
                sum += c;
            }
            return sum / weft::constants::color_channel_max;
        }

        // Whether the mask's outermost rows and columns are uncovered.
        bool clear_border() const {
            for (std::uint32_t x = 0; x < mask.width; ++x) {
                if (at(mask.x + int(x), mask.y) || at(mask.x + int(x), mask.y + int(mask.height) - 1)) {
                    return false;
                }
            }
            for (std::uint32_t y = 0; y < mask.height; ++y) {
                if (at(mask.x, mask.y + int(y)) || at(mask.x + int(mask.width) - 1, mask.y + int(y))) {
                    return false;
                }
            }
            return true;
        }
    };

    constexpr std::array<std::int32_t, 4> everywhere{-4096, -4096, 4096, 4096};

    Drawn draw(weft::vector::Rasterizer& r, const paint::PathPaint& p, const paint::Affine& m = paint::identity,
               const std::array<std::int32_t, 4>& clip = everywhere) {
        return {r.rasterize(p, m, weft::vector::mask_bounds(p, m, clip)).value()};
    }

    bool close_to(double a, double b, double tolerance) {
        return std::abs(a - b) <= tolerance;
    }

    void fills(weft::vector::Rasterizer& r) {
        // Fractional edges cover their pixels by area.
        const auto rect =
            draw(r, fill(polyline({{10.25f, 20.5f}, {30.75f, 20.5f}, {30.75f, 25.5f}, {10.25f, 25.5f}}, true)));
        require(rect.at(20, 22) == 255 && close_to(rect.at(10, 22), 191, 1) && close_to(rect.at(30, 22), 191, 1) &&
                    close_to(rect.at(20, 20), 128, 1) && rect.at(9, 22) == 0 && rect.at(31, 22) == 0 &&
                    rect.at(20, 26) == 0,
                "fractional rectangle edges");
        require(close_to(rect.area(), 20.5 * 5, .15) && rect.clear_border(), "rectangle area and bounds");
        const auto disc = draw(r, fill(circle(50, 50, 10)));
        require(close_to(disc.area(), std::numbers::pi * 100, std::numbers::pi * 100 * .003) && disc.clear_border(),
                "circle area " + std::to_string(disc.area()) + " bounds clear " + std::to_string(disc.clear_border()));
        // Nested squares wound the same way: non-zero fills the hole, even-odd leaves it.
        auto nested = polyline({{0, 0}, {20, 0}, {20, 20}, {0, 20}}, true);
        append(nested, polyline({{5, 5}, {15, 5}, {15, 15}, {5, 15}}, true));
        auto even = nested;
        even.evenodd = true;
        auto reversed = polyline({{0, 0}, {20, 0}, {20, 20}, {0, 20}}, true);
        append(reversed, polyline({{5, 5}, {5, 15}, {15, 15}, {15, 5}}, true));
        require(draw(r, fill(nested)).at(10, 10) == 255 && draw(r, fill(even)).at(10, 10) == 0 &&
                    draw(r, fill(even)).at(2, 10) == 255 && draw(r, fill(reversed)).at(10, 10) == 0,
                "fill rules");
        // A line after a close starts at the closed subpath's start.
        auto reopened = polyline({{0, 0}, {10, 0}, {10, 10}}, true);
        reopened.verbs.push_back(Verb::Line);
        reopened.points.push_back({0, 10});
        reopened.verbs.push_back(Verb::Line);
        reopened.points.push_back({-10, 10});
        require(draw(r, fill(reopened)).at(-2, 8) == 255, "subpath restarts at the closed start");
    }

    void strokes(weft::vector::Rasterizer& r) {
        const auto line = polyline({{10, 10}, {30, 10}}, false);
        const auto butt = draw(r, stroke(line, 4));
        require(butt.at(20, 8) == 255 && butt.at(20, 11) == 255 && butt.at(20, 7) == 0 && butt.at(20, 12) == 0 &&
                    butt.at(10, 10) == 255 && butt.at(9, 10) == 0 && butt.at(30, 10) == 0 && butt.clear_border(),
                "butt caps end at the line");
        const auto square = draw(r, stroke(line, 4, Cap::Square));
        require(square.at(8, 10) == 255 && square.at(7, 10) == 0 && square.at(31, 10) == 255 && square.at(32, 10) == 0,
                "square caps reach half the width");
        const auto round = draw(r, stroke(line, 4, Cap::Round));
        require(close_to(round.area(), 80 + std::numbers::pi * 4, .5) && round.at(8, 10) > 0 && round.at(7, 10) == 0,
                "round caps add a disc");
        // An outer corner: a miter fills it, a bevel cuts it, a round join rounds it.
        const auto corner = polyline({{10, 10}, {30, 10}, {30, 30}}, false);
        const auto miter = draw(r, stroke(corner, 4, Cap::Butt, Join::Miter));
        const auto bevel = draw(r, stroke(corner, 4, Cap::Butt, Join::Bevel));
        const auto rounded = draw(r, stroke(corner, 4, Cap::Butt, Join::Round));
        require(miter.at(31, 8) == 255 && bevel.at(31, 8) < 160 && rounded.at(31, 8) < 255 &&
                    rounded.at(31, 8) > bevel.at(31, 8),
                "joins");
        // A sharp turn: its miter (about ten times the width) passes a limit of 4 and becomes a bevel.
        const auto sharp = polyline({{0, 20}, {40, 20}, {0, 24}}, false);
        auto limited = stroke(sharp, 2);
        auto unlimited = limited;
        auto loose = *unlimited.stroke;
        loose.miter_limit = 100;
        unlimited.stroke = loose;
        const auto beyond = [](const Drawn& d) {
            int covered{};
            for (int y = 10; y < 30; ++y) {
                for (int x = 42; x < 70; ++x) {
                    covered += d.at(x, y) > 0;
                }
            }
            return covered;
        };
        require(beyond(draw(r, limited)) == 0 && beyond(draw(r, unlimited)) > 10, "miter limit");
        // A map that triples y triples the stroke's thickness.
        const auto tall = draw(r, stroke(line, 2), {1, 0, 0, 3, 0, 0});
        require(tall.at(20, 27) == 255 && tall.at(20, 32) == 255 && tall.at(20, 26) == 0 && tall.at(20, 33) == 0,
                "a map stretches the stroke");
        // A subpath of no length shows only through round or square caps.
        auto dot = polyline({{10, 10}}, true);
        require(draw(r, stroke(dot, 4)).mask.coverage.empty() || draw(r, stroke(dot, 4)).area() == 0,
                "no-length subpath with butt caps");
        require(close_to(draw(r, stroke(dot, 4, Cap::Round)).area(), std::numbers::pi * 4, .3), "round dot");
    }

    void dashes(weft::vector::Rasterizer& r) {
        auto dashed = [&](float offset, std::vector<float> pattern, Cap cap = Cap::Butt, float width = 2) {
            auto twin = polyline({{0, 10}, {40, 10}}, false);
            append(twin, polyline({{0, 20}, {40, 20}}, false));
            auto p = stroke(twin, width, cap);
            auto s = *p.stroke;
            s.dashes = std::move(pattern);
            s.dash_offset = offset;
            p.stroke = s;
            return draw(r, p);
        };
        const auto plain = dashed(0, {10, 5});
        for (int y : {10, 20}) {
            require(plain.at(5, y) == 255 && plain.at(12, y) == 0 && plain.at(17, y) == 255 && plain.at(27, y) == 0 &&
                        plain.at(35, y) == 255,
                    "dashes alternate and restart on each subpath");
        }
        const auto shifted = dashed(5, {10, 5});
        require(shifted.at(2, 10) == 255 && shifted.at(7, 10) == 0 && shifted.at(12, 10) == 255 &&
                    shifted.at(22, 10) == 0,
                "dash offset");
        const auto odd = dashed(0, {10, 5, 5});
        // An odd pattern repeats: 10 on, 5 off, 5 on, 10 off, 5 on, 5 off.
        require(odd.at(5, 10) == 255 && odd.at(12, 10) == 0 && odd.at(17, 10) == 255 && odd.at(22, 10) == 0 &&
                    odd.at(32, 10) == 255 && odd.at(37, 10) == 0,
                "odd dash pattern");
        // An offset into the repeated half: 2 into the 10 off, then 5 on, 5 off, 10 on, 5 off, 5 on.
        const auto late = dashed(22, {10, 5, 5});
        require(late.at(3, 10) == 0 && late.at(10, 10) == 255 && late.at(15, 10) == 0 && late.at(22, 10) == 255 &&
                    late.at(30, 10) == 0 && late.at(35, 10) == 255,
                "odd dash pattern offset into its repeat");
        const auto dots = dashed(0, {0, 10}, Cap::Round, 4);
        require(dots.at(0, 10) > 0 && dots.at(9, 10) > 0 && dots.at(10, 10) > 0 && dots.at(5, 10) == 0 &&
                    close_to(dots.area(), 10 * std::numbers::pi * 4, 2),
                "dots from dashes of no length");
    }

    void bounds(weft::vector::Rasterizer& r) {
        const auto shape = stroke(polyline({{10, 10}, {60, 15}, {20, 50}}, true), 6, Cap::Round, Join::Miter);
        const auto whole = draw(r, shape);
        require(whole.area() > 0 && whole.clear_border(), "bounds hold every covered pixel");
        const auto part = draw(r, shape, paint::identity, {20, 20, 40, 40});
        require(part.mask.x == 20 && part.mask.y == 20 && part.mask.width == 20 && part.mask.height == 20,
                "bounds cut to the clip");
        bool same = true;
        for (int y = 20; y < 40; ++y) {
            for (int x = 20; x < 40; ++x) {
                same = same && part.at(x, y) == whole.at(x, y);
            }
        }
        require(same, "a clipped mask keeps the unclipped coverage");
        require(weft::vector::mask_bounds(shape, paint::identity, {100, 100, 200, 200}) ==
                    std::array<std::int32_t, 4>{},
                "nothing past the clip");
        const auto huge = fill(polyline({{0, 0}, {5000, 0}, {5000, 5000}, {0, 5000}}, true));
        const auto refused = r.rasterize(
            huge, paint::identity, weft::vector::mask_bounds(huge, paint::identity, {-10000, -10000, 10000, 10000}));
        require(!refused && refused.error().code == weft::Diag::PaintList, "mask bound");
        // The translation's fraction moves coverage, not just the mask.
        const auto moved = draw(r, fill(polyline({{0, 0}, {4, 0}, {4, 4}, {0, 4}}, true)), {1, 0, 0, 1, 10.5f, 0});
        require(close_to(moved.at(10, 1), 128, 1) && moved.at(11, 1) == 255 && close_to(moved.at(14, 1), 128, 1),
                "fractional translation");
    }

    // Random paths, filled and stroked in every style under random maps: nothing fails, coverage stays inside the
    // mask bounds, and a convex polygon's covered area matches its exact area.
    void random_paths(weft::vector::Rasterizer& r) {
        std::uint32_t seed = 20261012;
        const auto next = [&] {
            seed = seed * 1664525u + 1013904223u;
            return seed;
        };
        const auto unit = [&] {
            return (next() >> 8) / double(1 << 24);
        };
        for (int i = 0; i < 1500; ++i) {
            paint::Path path;
            const int verbs = 1 + int(next() % 9);
            const auto pick = [&] {
                // Now and then the last point again: segments of no length.
                if (!path.points.empty() && next() % 5 == 0) {
                    return path.points.back();
                }
                return std::array<float, 2>{float(unit() * 60 - 10), float(unit() * 60 - 10)};
            };
            for (int v = 0; v < verbs; ++v) {
                switch (v == 0 ? 0u : next() % 4) {
                    case 0:
                        path.verbs.push_back(Verb::Move);
                        path.points.push_back(pick());
                        break;
                    case 1:
                        path.verbs.push_back(Verb::Line);
                        path.points.push_back(pick());
                        break;
                    case 2:
                        path.verbs.push_back(Verb::Cubic);
                        for (int k = 0; k < 3; ++k) {
                            path.points.push_back(pick());
                        }
                        break;
                    default:
                        path.verbs.push_back(Verb::Close);
                        break;
                }
            }
            path.evenodd = next() % 2 != 0;
            const paint::Affine m{float(.5 + unit() * 2), float(unit() - .5), float(unit() - .5),
                                  float(.5 + unit() * 2), float(unit() * 20), float(unit() * 20)};
            require(draw(r, fill(path), m).clear_border(), "random fill inside its bounds");
            paint::Stroke s;
            s.width = float(unit() * 6);
            s.cap = Cap(next() % 3);
            s.join = Join(next() % 3);
            s.miter_limit = float(1 + unit() * 6);
            if (next() % 3 == 0) {
                s.dashes = {float(unit() * 8), float(unit() * 8)};
                if (next() % 2) {
                    s.dashes.push_back(float(unit() * 4));
                }
                s.dash_offset = float(unit() * 20 - 10);
            }
            const paint::PathPaint stroked{std::make_shared<const paint::Path>(path), {1, 1, 1, 1}, s};
            require(draw(r, stroked, m).clear_border(), "random stroke inside its bounds");
        }
        for (int i = 0; i < 200; ++i) {
            const int n = 3 + int(next() % 8);
            const double radius = 5 + unit() * 20;
            std::vector<double> angles;
            for (int k = 0; k < n; ++k) {
                angles.push_back(unit() * 2 * std::numbers::pi);
            }
            std::sort(angles.begin(), angles.end());
            paint::Path polygon;
            for (auto a : angles) {
                polygon.verbs.push_back(polygon.verbs.empty() ? Verb::Move : Verb::Line);
                polygon.points.push_back({float(30 + radius * std::cos(a)), float(30 + radius * std::sin(a))});
            }
            polygon.verbs.push_back(Verb::Close);
            double area{};
            for (std::size_t k = 0; k < polygon.points.size(); ++k) {
                const auto& a = polygon.points[k];
                const auto& b = polygon.points[(k + 1) % polygon.points.size()];
                area += double(a[0]) * b[1] - double(b[0]) * a[1];
            }
            area = std::abs(area) / 2;
            require(close_to(draw(r, fill(polygon)).area(), area, area * .005 + .5),
                    "convex polygon area " + std::to_string(i));
        }
    }
} // namespace

int main() {
    try {
        weft::vector::Rasterizer rasterizer;
        fills(rasterizer);
        strokes(rasterizer);
        dashes(rasterizer);
        bounds(rasterizer);
        random_paths(rasterizer);
        std::cout << "PASS vector raster: fill areas and rules, caps, joins and miter limit, stretched strokes, "
                     "dashes and dots, bounds and clips, 1500 random paths, 200 polygon areas\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
