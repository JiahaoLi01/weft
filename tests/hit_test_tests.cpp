#include "layout/hit_test.hpp"
#include "support/check.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>

// The one hit path of documents and scenes (layout/hit_test.hpp): shapes, clip chains, planes and paint order.
namespace {
    using weft::HitAreas;
    using weft::HitShape;
    using weft::geometry::Plane;
    using weft::testing::require;

    // The inverse of a plane that scales by s about the origin and then translates by (tx, ty).
    Plane scaled_inverse(double s, double tx, double ty) {
        Plane forward{{s, 0, tx, 0, s, ty, 0, 0, 1}};
        Plane inverse;
        require(forward.inverse(inverse), "invertible plane");
        return inverse;
    }

    std::shared_ptr<weft::HitOutline> square(double x, double y, double size, bool closed = true) {
        auto outline = std::make_shared<weft::HitOutline>();
        outline->figures.push_back({{{x, y}, {x + size, y}, {x + size, y + size}, {x, y + size}}, closed});
        return outline;
    }

    HitShape shape(HitShape::Kind kind, std::shared_ptr<const weft::HitOutline> outline) {
        HitShape s;
        s.kind = kind;
        s.outline = std::move(outline);
        return s;
    }

    void shapes() {
        // Boxes are half-open; an empty box contains nothing; text bounds are closed.
        const auto box = HitShape::box(10, 20, 30, 40);
        require(weft::hit_contains(box, 10, 20) && !weft::hit_contains(box, 40, 30) &&
                    !weft::hit_contains(box, 20, 60) && weft::hit_contains(box, 39.999, 59.999),
                "half-open box");
        require(!weft::hit_contains(HitShape::box(0, 0, 0, 10), 0, 5), "empty box");
        require(weft::hit_contains(HitShape::bounds(10, 20, 30, 40), 40, 60), "closed bounds");
        // Rounded corners.
        HitShape rounded;
        rounded.kind = HitShape::Kind::Rounded;
        rounded.rounded = weft::normalize_rounded({{0, 0, 100, 100}, {20, 20, 20, 20}, {20, 20, 20, 20}});
        require(!weft::hit_contains(rounded, 2, 2) && weft::hit_contains(rounded, 50, 1) &&
                    weft::hit_contains(rounded, 20, 20),
                "rounded corner");
        // Fill rules: two overlapping squares wound the same way overlap twice.
        auto twice = square(0, 0, 10);
        twice->figures.push_back(square(5, 5, 10)->figures.front());
        require(weft::hit_contains(shape(HitShape::Kind::Fill, twice), 7, 7), "nonzero overlap");
        twice->evenodd = true;
        require(!weft::hit_contains(shape(HitShape::Kind::Fill, twice), 7, 7) &&
                    weft::hit_contains(shape(HitShape::Kind::Fill, twice), 2, 2),
                "even-odd overlap");
        // Strokes: within half the width of a segment; caps only where the stroke is open.
        auto line = std::make_shared<weft::HitOutline>();
        line->figures.push_back({{{0, 0}, {10, 0}}, false});
        auto stroke = shape(HitShape::Kind::Stroke, line);
        stroke.stroke.width = 4;
        require(weft::hit_contains(stroke, 5, 1.9) && !weft::hit_contains(stroke, 5, 2.1) &&
                    !weft::hit_contains(stroke, -1, 0),
                "butt stroke");
        stroke.stroke.cap = weft::HitStroke::Cap::Square;
        require(weft::hit_contains(stroke, -1.9, 1.9), "square cap");
        stroke.stroke.cap = weft::HitStroke::Cap::Round;
        require(weft::hit_contains(stroke, -1.4, 1.4) && !weft::hit_contains(stroke, -1.9, 1.9), "round cap");
        // Dashes: 2 on, 2 off from the start.
        stroke.stroke.cap = weft::HitStroke::Cap::Butt;
        stroke.stroke.dashes = {2, 2};
        require(weft::hit_contains(stroke, 1, 0) && !weft::hit_contains(stroke, 3, 0) &&
                    weft::hit_contains(stroke, 5, 0),
                "dashes");
        // The figure's own stroke when it is closed.
        auto closed = shape(HitShape::Kind::Stroke, square(0, 0, 10));
        require(weft::hit_contains(closed, 0, 5) && !weft::hit_contains(closed, 5, 5), "closed outline stroke");
    }

    void areas() {
        HitAreas map;
        // Paint order: the later area is on top.
        map.areas.push_back({0, {}, {HitShape::box(0, 0, 100, 100)}, HitAreas::none});
        map.areas.push_back({1, {}, {HitShape::box(50, 50, 100, 100)}, HitAreas::none});
        require(weft::hit_areas(map, 60, 60) == 1 && weft::hit_areas(map, 10, 10) == 0 &&
                    !weft::hit_areas(map, 200, 10),
                "paint order");
        // A target the caller does not accept passes the point to what is below it.
        const auto skip_top = [](std::size_t target) {
            return target != 1;
        };
        require(weft::hit_areas(map, 60, 60, skip_top) == 0, "pass-through target");
        // Clips chain through their parents: an area clipped by its ancestors' boxes.
        const auto outer = map.add_clip({}, HitShape::box(0, 0, 80, 80), HitAreas::none);
        const auto inner = map.add_clip({}, HitShape::box(40, 0, 80, 80), outer);
        map.areas.push_back({2, {}, {HitShape::box(0, 0, 200, 200)}, inner});
        require(weft::hit_areas(map, 60, 60) == 2, "inside the clip chain");
        require(weft::hit_areas(map, 30, 30) == 0, "outside the inner clip");
        require(weft::hit_areas(map, 90, 60) == 1, "outside the outer clip");
        // Planes: points are mapped into an area's and each clip's own coordinates.
        HitAreas planes;
        const auto clip = planes.add_clip(scaled_inverse(2, 100, 0), HitShape::box(0, 0, 10, 10), HitAreas::none);
        planes.areas.push_back({7, scaled_inverse(2, 100, 0), {HitShape::box(0, 0, 50, 50)}, clip});
        require(weft::hit_areas(planes, 119, 19) == 7 && !weft::hit_areas(planes, 121, 19) &&
                    !weft::hit_areas(planes, 99, 5),
                "planes");
        // A clip keeps its own plane: an unmapped area inside a clip drawn at twice the size.
        HitAreas mixed;
        const auto scaled = mixed.add_clip(scaled_inverse(2, 0, 0), HitShape::box(0, 0, 10, 10), HitAreas::none);
        mixed.areas.push_back({8, {}, {HitShape::box(0, 0, 100, 100)}, scaled});
        require(weft::hit_areas(mixed, 15, 15) == 8 && !weft::hit_areas(mixed, 25, 5), "clip in its own plane");
        // A quarter turn about the origin.
        Plane turn{{0, -1, 0, 1, 0, 0, 0, 0, 1}};
        Plane inverse;
        require(turn.inverse(inverse), "invertible turn");
        HitAreas rotated;
        rotated.areas.push_back({3, inverse, {HitShape::box(0, 0, 10, 20)}, HitAreas::none});
        require(weft::hit_areas(rotated, -15, 5) == 3 && !weft::hit_areas(rotated, 5, 5), "rotated area");
        // Points outside the finite plane hit nothing.
        require(!weft::hit_areas(map, std::numeric_limits<double>::quiet_NaN(), 0) &&
                    !weft::hit_areas(map, std::numeric_limits<double>::infinity(), 0),
                "non-finite point");
    }
} // namespace

int main() {
    try {
        shapes();
        areas();
        std::cout
            << "PASS one hit path: half-open boxes, closed bounds, rounded corners, fill rules, strokes with caps "
               "and dashes, paint order, pass-through targets, clip chains and planes\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
