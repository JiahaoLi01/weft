#include "layout/hit_test.hpp"

#include <algorithm>
#include <cmath>

namespace weft {
    namespace {
        using Point = std::array<double, 2>;

        bool fill_contains(const HitOutline& outline, Point p) {
            int winding{};
            int crossings{};
            for (const auto& f : outline.figures) {
                const auto& v = f.points;
                for (std::size_t i = 0; i < v.size(); ++i) {
                    const auto& a = v[i];
                    const auto& b = v[(i + 1) % v.size()];
                    if ((a[1] <= p[1]) != (b[1] <= p[1])) {
                        const double x = a[0] + (p[1] - a[1]) * (b[0] - a[0]) / (b[1] - a[1]);
                        if (x > p[0]) {
                            ++crossings;
                            winding += b[1] > a[1] ? 1 : -1;
                        }
                    }
                }
            }
            return outline.evenodd ? (crossings % 2) != 0 : winding != 0;
        }

        double segment_distance(Point p, Point a, Point b) {
            const double dx = b[0] - a[0];
            const double dy = b[1] - a[1];
            const double length2 = dx * dx + dy * dy;
            const double t =
                length2 > 0 ? std::clamp(((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / length2, 0.0, 1.0) : 0;
            return std::hypot(p[0] - (a[0] + t * dx), p[1] - (a[1] + t * dy));
        }

        // Stroke containment on the flattened figures: segments within half the width, round joins, butt/square/
        // round caps at open ends and dash ends, dashes walked along each figure.
        bool stroke_contains(const HitOutline& outline, const HitStroke& stroke, Point p) {
            const double half = stroke.width / 2;
            auto cap_hit = [&](Point end, Point direction) {
                if (stroke.cap == HitStroke::Cap::Round) {
                    return std::hypot(p[0] - end[0], p[1] - end[1]) <= half;
                }
                if (stroke.cap == HitStroke::Cap::Square) {
                    const double length = std::hypot(direction[0], direction[1]);
                    if (length <= 0) {
                        return false;
                    }
                    const double ux = direction[0] / length;
                    const double uy = direction[1] / length;
                    const double along = (p[0] - end[0]) * ux + (p[1] - end[1]) * uy;
                    const double across = -(p[0] - end[0]) * uy + (p[1] - end[1]) * ux;
                    return along >= 0 && along <= half && std::abs(across) <= half;
                }
                return false;
            };
            auto segment_hit = [&](Point a, Point b) {
                const double dx = b[0] - a[0];
                const double dy = b[1] - a[1];
                const double length2 = dx * dx + dy * dy;
                if (length2 <= 0) {
                    return false;
                }
                const double t = ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / length2;
                return t >= 0 && t <= 1 && segment_distance(p, a, b) <= half;
            };
            for (const auto& f : outline.figures) {
                std::vector<Point> v = f.points;
                if (f.closed && !v.empty()) {
                    v.push_back(v.front());
                }
                if (v.size() < 2) {
                    continue;
                }
                if (stroke.dashes.empty()) {
                    for (std::size_t i = 0; i + 1 < v.size(); ++i) {
                        if (segment_hit(v[i], v[i + 1])) {
                            return true;
                        }
                        // Joins between segments (round).
                        if (i > 0 && std::hypot(p[0] - v[i][0], p[1] - v[i][1]) <= half) {
                            return true;
                        }
                    }
                    if (f.closed && std::hypot(p[0] - v.front()[0], p[1] - v.front()[1]) <= half) {
                        return true;
                    }
                    if (!f.closed &&
                        (cap_hit(v.front(), {v.front()[0] - v[1][0], v.front()[1] - v[1][1]}) ||
                         cap_hit(v.back(), {v.back()[0] - v[v.size() - 2][0], v.back()[1] - v[v.size() - 2][1]}))) {
                        return true;
                    }
                    continue;
                }
                double total{};
                for (auto d : stroke.dashes) {
                    total += d;
                }
                double phase = std::fmod(stroke.offset, total);
                if (phase < 0) {
                    phase += total;
                }
                std::size_t dash{};
                while (phase >= stroke.dashes[dash]) {
                    phase -= stroke.dashes[dash];
                    dash = (dash + 1) % stroke.dashes.size();
                }
                double left = stroke.dashes[dash] - phase;
                for (std::size_t i = 0; i + 1 < v.size(); ++i) {
                    Point a = v[i];
                    const Point b = v[i + 1];
                    double remaining = std::hypot(b[0] - a[0], b[1] - a[1]);
                    const Point direction{b[0] - a[0], b[1] - a[1]};
                    while (remaining > 0) {
                        const double take = std::min(left, remaining);
                        const double t = take / remaining;
                        const Point c{a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t};
                        if (dash % 2 == 0 && (segment_hit(a, c) || cap_hit(a, {-direction[0], -direction[1]}) ||
                                              cap_hit(c, direction))) {
                            return true;
                        }
                        remaining -= take;
                        left -= take;
                        a = c;
                        if (left <= 0) {
                            dash = (dash + 1) % stroke.dashes.size();
                            left = stroke.dashes[dash];
                        }
                    }
                }
            }
            return false;
        }
    } // namespace

    bool hit_contains(const HitShape& shape, double x, double y) {
        const auto& r = shape.rect;
        switch (shape.kind) {
            case HitShape::Kind::Box:
                return r[2] > 0 && r[3] > 0 && x >= r[0] && y >= r[1] && x < r[0] + r[2] && y < r[1] + r[3];
            case HitShape::Kind::Bounds:
                return x >= r[0] && x <= r[0] + r[2] && y >= r[1] && y <= r[1] + r[3];
            case HitShape::Kind::Rounded:
                return contains_rounded(shape.rounded, float(x), float(y));
            case HitShape::Kind::Fill:
                return shape.outline && fill_contains(*shape.outline, {x, y});
            case HitShape::Kind::Stroke:
                return shape.outline && stroke_contains(*shape.outline, shape.stroke, {x, y});
        }
        return false;
    }

    std::optional<std::size_t> hit_areas(const HitAreas& areas, double x, double y,
                                         FunctionRef<bool(std::size_t target)> accept) {
        if (!std::isfinite(x) || !std::isfinite(y)) {
            return std::nullopt;
        }
        for (auto i = areas.areas.size(); i-- > 0;) {
            const auto& area = areas.areas[i];
            if (!accept(area.target)) {
                continue;
            }
            const auto p = area.inverse.point(x, y);
            if (std::none_of(area.shapes.begin(), area.shapes.end(), [&](const auto& shape) {
                    return hit_contains(shape, p[0], p[1]);
                })) {
                continue;
            }
            bool inside = true;
            for (auto c = area.clip; inside && c != HitAreas::none; c = areas.clips[c].parent) {
                const auto& clip = areas.clips[c];
                const auto q = clip.inverse.point(x, y);
                inside = hit_contains(clip.shape, q[0], q[1]);
            }
            if (inside) {
                return area.target;
            }
        }
        return std::nullopt;
    }

    std::optional<std::size_t> hit_areas(const HitAreas& areas, double x, double y) {
        const auto all = [](std::size_t) {
            return true;
        };
        return hit_areas(areas, x, y, all);
    }
} // namespace weft
