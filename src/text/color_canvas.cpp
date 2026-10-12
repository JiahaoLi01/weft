#include "text/color_canvas.hpp"

#include "constants/text.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace weft {
    namespace {
        namespace limits = constants::text;
        using Point = ColorCanvas::Point;
        using Color = ColorCanvas::Color;
        using Matrix = ColorCanvas::Matrix;
        using Composite = ColorCanvas::Composite;

        Point apply(const Matrix& m, Point p) {
            return {m[0] * p[0] + m[2] * p[1] + m[4], m[1] * p[0] + m[3] * p[1] + m[5]};
        }

        // The map that applies b, then a.
        Matrix compose(const Matrix& a, const Matrix& b) {
            return {a[0] * b[0] + a[2] * b[1], a[1] * b[0] + a[3] * b[1],        a[0] * b[2] + a[2] * b[3],
                    a[1] * b[2] + a[3] * b[3], a[0] * b[4] + a[2] * b[5] + a[4], a[1] * b[4] + a[3] * b[5] + a[5]};
        }

        // False for a degenerate map.
        bool invert(const Matrix& m, Matrix& out) {
            const double det = m[0] * m[3] - m[1] * m[2];
            if (!std::isfinite(det) || std::abs(det) < limits::color_singular_epsilon) {
                return false;
            }
            out = {m[3] / det,
                   -m[1] / det,
                   -m[2] / det,
                   m[0] / det,
                   (m[2] * m[5] - m[3] * m[4]) / det,
                   (m[1] * m[4] - m[0] * m[5]) / det};
            return true;
        }

        Color premultiply(const Color& c) {
            const float a = std::clamp(c[3], 0.f, 1.f);
            return {std::clamp(c[0], 0.f, 1.f) * a, std::clamp(c[1], 0.f, 1.f) * a, std::clamp(c[2], 0.f, 1.f) * a, a};
        }

        // Appends a cubic's points, flattened by subdivision, to a polyline (its start excluded).
        void flatten(Point p0, Point c1, Point c2, Point p, int depth, std::vector<Point>& out) {
            const double dx = p[0] - p0[0];
            const double dy = p[1] - p0[1];
            const double length = std::hypot(dx, dy);
            const auto distance = [&](Point q) {
                return length > 0 ? std::abs((q[0] - p0[0]) * dy - (q[1] - p0[1]) * dx) / length
                                  : std::hypot(q[0] - p0[0], q[1] - p0[1]);
            };
            if (depth >= limits::color_max_flattening_depth ||
                std::max(distance(c1), distance(c2)) <= limits::color_flattening_tolerance) {
                out.push_back(p);
                return;
            }
            const Point a{(p0[0] + c1[0]) / 2, (p0[1] + c1[1]) / 2};
            const Point b{(c1[0] + c2[0]) / 2, (c1[1] + c2[1]) / 2};
            const Point c{(c2[0] + p[0]) / 2, (c2[1] + p[1]) / 2};
            const Point ab{(a[0] + b[0]) / 2, (a[1] + b[1]) / 2};
            const Point bc{(b[0] + c[0]) / 2, (b[1] + c[1]) / 2};
            const Point mid{(ab[0] + bc[0]) / 2, (ab[1] + bc[1]) / 2};
            flatten(p0, a, ab, mid, depth + 1, out);
            flatten(mid, bc, c, p, depth + 1, out);
        }

        // The separable blend functions of the W3C compositing specification, of unpremultiplied backdrop b and
        // source s.
        float blend(Composite mode, float b, float s) {
            const auto multiply = [](float x, float y) {
                return x * y;
            };
            const auto screen = [](float x, float y) {
                return x + y - x * y;
            };
            const auto hard_light = [&](float x, float y) {
                return y <= .5f ? multiply(x, 2 * y) : screen(x, 2 * y - 1);
            };
            switch (mode) {
                case Composite::Multiply:
                    return multiply(b, s);
                case Composite::Screen:
                    return screen(b, s);
                case Composite::Overlay:
                    return hard_light(s, b);
                case Composite::Darken:
                    return std::min(b, s);
                case Composite::Lighten:
                    return std::max(b, s);
                case Composite::ColorDodge:
                    return b == 0 ? 0 : s >= 1 ? 1 : std::min(1.f, b / (1 - s));
                case Composite::ColorBurn:
                    return b >= 1 ? 1 : s <= 0 ? 0 : 1 - std::min(1.f, (1 - b) / s);
                case Composite::HardLight:
                    return hard_light(b, s);
                case Composite::SoftLight: {
                    if (s <= .5f) {
                        return b - (1 - 2 * s) * b * (1 - b);
                    }
                    const float d = b <= .25f ? ((16 * b - 12) * b + 4) * b : std::sqrt(b);
                    return b + (2 * s - 1) * (d - b);
                }
                case Composite::Difference:
                    return std::abs(b - s);
                case Composite::Exclusion:
                    return b + s - 2 * b * s;
                default:
                    return s;
            }
        }

        using Rgb = std::array<float, 3>;

        float luminosity(const Rgb& c) {
            return .3f * c[0] + .59f * c[1] + .11f * c[2];
        }

        Rgb with_luminosity(Rgb c, float l) {
            const float d = l - luminosity(c);
            for (auto& v : c) {
                v += d;
            }
            const float lum = luminosity(c);
            const float n = std::min({c[0], c[1], c[2]});
            const float x = std::max({c[0], c[1], c[2]});
            for (auto& v : c) {
                if (n < 0 && lum - n > 0) {
                    v = lum + (v - lum) * lum / (lum - n);
                }
                if (x > 1 && x - lum > 0) {
                    v = lum + (v - lum) * (1 - lum) / (x - lum);
                }
            }
            return c;
        }

        float saturation(const Rgb& c) {
            return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]});
        }

        Rgb with_saturation(Rgb c, float s) {
            std::array<int, 3> order{0, 1, 2};
            std::sort(order.begin(), order.end(), [&](int a, int b) {
                return c[a] < c[b];
            });
            auto& lo = c[order[0]];
            auto& mid = c[order[1]];
            auto& hi = c[order[2]];
            if (hi > lo) {
                mid = (mid - lo) * s / (hi - lo);
                hi = s;
            } else {
                mid = hi = 0;
            }
            lo = 0;
            return c;
        }

        // A source composited onto a backdrop, both premultiplied.
        Color composite(Composite mode, const Color& s, const Color& d) {
            const float sa = s[3];
            const float da = d[3];
            Color out{};
            const auto combine = [&](float fs, float fd) {
                for (int c = 0; c < 4; ++c) {
                    out[c] = s[c] * fs + d[c] * fd;
                }
                return out;
            };
            switch (mode) {
                case Composite::Clear:
                    return {};
                case Composite::Src:
                    return s;
                case Composite::Dest:
                    return d;
                case Composite::SrcOver:
                    return combine(1, 1 - sa);
                case Composite::DestOver:
                    return combine(1 - da, 1);
                case Composite::SrcIn:
                    return combine(da, 0);
                case Composite::DestIn:
                    return combine(0, sa);
                case Composite::SrcOut:
                    return combine(1 - da, 0);
                case Composite::DestOut:
                    return combine(0, 1 - sa);
                case Composite::SrcAtop:
                    return combine(da, 1 - sa);
                case Composite::DestAtop:
                    return combine(1 - da, sa);
                case Composite::Xor:
                    return combine(1 - da, 1 - sa);
                case Composite::Plus:
                    for (int c = 0; c < 4; ++c) {
                        out[c] = std::min(1.f, s[c] + d[c]);
                    }
                    return out;
                default:
                    break;
            }
            // Blend modes: the source's color blended with the backdrop's where both are, each alone elsewhere.
            const Rgb cs{sa > 0 ? s[0] / sa : 0, sa > 0 ? s[1] / sa : 0, sa > 0 ? s[2] / sa : 0};
            const Rgb cb{da > 0 ? d[0] / da : 0, da > 0 ? d[1] / da : 0, da > 0 ? d[2] / da : 0};
            Rgb mixed{};
            switch (mode) {
                case Composite::Hue:
                    mixed = with_luminosity(with_saturation(cs, saturation(cb)), luminosity(cb));
                    break;
                case Composite::Saturation:
                    mixed = with_luminosity(with_saturation(cb, saturation(cs)), luminosity(cb));
                    break;
                case Composite::Color:
                    mixed = with_luminosity(cs, luminosity(cb));
                    break;
                case Composite::Luminosity:
                    mixed = with_luminosity(cb, luminosity(cs));
                    break;
                default:
                    for (int c = 0; c < 3; ++c) {
                        mixed[c] = blend(mode, cb[c], cs[c]);
                    }
                    break;
            }
            for (int c = 0; c < 3; ++c) {
                out[c] = s[c] * (1 - da) + d[c] * (1 - sa) + sa * da * std::clamp(mixed[c], 0.f, 1.f);
            }
            out[3] = sa + da - sa * da;
            return out;
        }
    } // namespace

    Color color_line(const std::vector<ColorCanvas::Stop>& stops, ColorCanvas::Extend extend, double t) {
        if (stops.empty()) {
            return {};
        }
        const double first = stops.front().offset;
        const double last = stops.back().offset;
        const double span = last - first;
        if (span > 0 && std::isfinite(t)) {
            if (extend == ColorCanvas::Extend::Repeat) {
                t = first + (t - first - span * std::floor((t - first) / span));
            } else if (extend == ColorCanvas::Extend::Reflect) {
                const double u = t - first - 2 * span * std::floor((t - first) / (2 * span));
                t = first + (u <= span ? u : 2 * span - u);
            }
        }
        if (!(t > first)) {
            return premultiply(stops.front().color);
        }
        if (!(t < last)) {
            return premultiply(stops.back().color);
        }
        std::size_t i = 1;
        while (i + 1 < stops.size() && stops[i].offset <= t) {
            ++i;
        }
        const auto& a = stops[i - 1];
        const auto& b = stops[i];
        const double gap = double(b.offset) - a.offset;
        const float f = gap > 0 ? float((t - a.offset) / gap) : 1.f;
        const auto pa = premultiply(a.color);
        const auto pb = premultiply(b.color);
        Color out{};
        for (int c = 0; c < 4; ++c) {
            out[c] = pa[c] + (pb[c] - pa[c]) * f;
        }
        return out;
    }

    ColorCanvas::ColorCanvas(std::uint32_t width, std::uint32_t height, const Matrix& base)
        : width_(width), height_(height) {
        if (!width || !height || std::uint64_t(width) * height > std::uint64_t(limits::max_color_glyph_pixels)) {
            throw std::invalid_argument("WEFT_COLOR_GLYPH: canvas bound");
        }
        transforms_.push_back(base);
        clips_.emplace_back(std::size_t(width) * height, 1.f);
        groups_.emplace_back(std::size_t(width) * height, Color{});
    }

    void ColorCanvas::push_transform(const Matrix& m) {
        transforms_.push_back(compose(transforms_.back(), m));
    }

    void ColorCanvas::pop_transform() {
        if (transforms_.size() > 1) {
            transforms_.pop_back();
        }
    }

    std::vector<float> ColorCanvas::coverage(const std::vector<std::vector<Point>>& polygons, bool evenodd) const {
        // Signed area accumulation (as font-rs): each edge adds the area it covers right of itself to the cells of
        // its rows, and a running sum along a row gives the winding coverage of each pixel. Edges lie within the
        // canvas's columns: the parts of an edge left or right of it run vertically along that side, so area left of
        // the canvas lands in its first cell.
        const auto w = std::size_t(width_);
        const auto h = std::size_t(height_);
        const auto stride = w + 2;
        std::vector<float> area(stride * h);
        const auto inside = [&](Point a, Point b) {
            if (a[1] == b[1]) {
                return;
            }
            const double direction = a[1] < b[1] ? 1 : -1;
            if (a[1] > b[1]) {
                std::swap(a, b);
            }
            const double top = std::max(a[1], 0.0);
            const double bottom = std::min(b[1], double(h));
            if (!(top < bottom)) {
                return;
            }
            const double slope = (b[0] - a[0]) / (b[1] - a[1]);
            double x = a[0] + (top - a[1]) * slope;
            for (auto y = std::size_t(std::floor(top)); y < h && double(y) < bottom; ++y) {
                const double dy = std::min(double(y + 1), bottom) - std::max(double(y), top);
                const double next = x + slope * dy;
                const double d = dy * direction;
                const double x0 = std::min(x, next);
                const double x1 = std::max(x, next);
                float* row = area.data() + y * stride;
                const double x0floor = std::floor(x0);
                const auto x0i = std::size_t(x0floor);
                const double x1ceil = std::ceil(x1);
                const auto x1i = std::size_t(x1ceil);
                if (x1i <= x0i + 1) {
                    const double middle = .5 * (x0 + x1) - x0floor;
                    row[x0i] += float(d - d * middle);
                    row[x0i + 1] += float(d * middle);
                } else {
                    const double s = 1 / (x1 - x0);
                    const double x0f = x0 - x0floor;
                    const double a0 = .5 * s * (1 - x0f) * (1 - x0f);
                    const double x1f = x1 - x1ceil + 1;
                    const double am = .5 * s * x1f * x1f;
                    row[x0i] += float(d * a0);
                    if (x1i == x0i + 2) {
                        row[x0i + 1] += float(d * (1 - a0 - am));
                    } else {
                        const double a1 = s * (1.5 - x0f);
                        row[x0i + 1] += float(d * (a1 - a0));
                        for (auto xi = x0i + 2; xi + 1 < x1i; ++xi) {
                            row[xi] += float(d * s);
                        }
                        const double a2 = a1 + double(x1i - x0i - 3) * s;
                        row[x1i - 1] += float(d * (1 - a2 - am));
                    }
                    row[x1i] += float(d * am);
                }
                x = next;
            }
        };
        const auto edge = [&](Point a, Point b) {
            std::array<double, 4> cuts{0, 1};
            std::size_t count = 2;
            for (const double side : {0.0, double(w)}) {
                if ((a[0] - side) * (b[0] - side) < 0) {
                    cuts[count++] = (side - a[0]) / (b[0] - a[0]);
                }
            }
            std::sort(cuts.begin(), cuts.begin() + std::ptrdiff_t(count));
            for (std::size_t i = 0; i + 1 < count; ++i) {
                Point p{a[0] + (b[0] - a[0]) * cuts[i], a[1] + (b[1] - a[1]) * cuts[i]};
                Point q{a[0] + (b[0] - a[0]) * cuts[i + 1], a[1] + (b[1] - a[1]) * cuts[i + 1]};
                const double middle = (p[0] + q[0]) / 2;
                const double side = middle <= 0 ? 0 : middle >= double(w) ? double(w) : -1;
                if (side >= 0) {
                    p[0] = q[0] = side;
                } else {
                    p[0] = std::clamp(p[0], 0.0, double(w));
                    q[0] = std::clamp(q[0], 0.0, double(w));
                }
                inside(p, q);
            }
        };
        for (const auto& polygon : polygons) {
            for (std::size_t i = 0; i < polygon.size(); ++i) {
                edge(polygon[i], polygon[(i + 1) % polygon.size()]);
            }
        }
        std::vector<float> out(w * h);
        for (std::size_t y = 0; y < h; ++y) {
            double sum{};
            for (std::size_t x = 0; x < w; ++x) {
                sum += area[y * stride + x];
                double v = std::abs(sum);
                if (evenodd) {
                    v -= 2 * std::floor(v / 2);
                    v = v > 1 ? 2 - v : v;
                }
                out[y * w + x] = float(std::min(v, 1.0));
            }
        }
        return out;
    }

    void ColorCanvas::intersect(const std::vector<float>& coverage) {
        auto clip = clips_.back();
        for (std::size_t i = 0; i < clip.size(); ++i) {
            clip[i] *= coverage[i];
        }
        clips_.push_back(std::move(clip));
    }

    void ColorCanvas::push_clip(const GlyphOutline& outline) {
        const auto& m = transforms_.back();
        std::vector<std::vector<Point>> polygons;
        std::size_t p{};
        const auto at = [&](std::size_t i) {
            return apply(m, {outline.points[i][0], outline.points[i][1]});
        };
        for (const auto verb : outline.verbs) {
            switch (verb) {
                case GlyphOutline::Verb::Move:
                    polygons.push_back({at(p++)});
                    break;
                case GlyphOutline::Verb::Line:
                    if (!polygons.empty()) {
                        polygons.back().push_back(at(p));
                    }
                    ++p;
                    break;
                case GlyphOutline::Verb::Quadratic: {
                    if (!polygons.empty()) {
                        const auto p0 = polygons.back().back();
                        const auto c = at(p);
                        const auto e = at(p + 1);
                        flatten(p0, {p0[0] + 2.0 / 3 * (c[0] - p0[0]), p0[1] + 2.0 / 3 * (c[1] - p0[1])},
                                {e[0] + 2.0 / 3 * (c[0] - e[0]), e[1] + 2.0 / 3 * (c[1] - e[1])}, e, 0,
                                polygons.back());
                    }
                    p += 2;
                    break;
                }
                case GlyphOutline::Verb::Cubic:
                    if (!polygons.empty()) {
                        flatten(polygons.back().back(), at(p), at(p + 1), at(p + 2), 0, polygons.back());
                    }
                    p += 3;
                    break;
                case GlyphOutline::Verb::Close:
                    break;
            }
        }
        intersect(coverage(polygons, outline.evenodd));
    }

    void ColorCanvas::push_clip(const std::array<double, 4>& rect) {
        const auto& m = transforms_.back();
        intersect(coverage({{apply(m, {rect[0], rect[1]}), apply(m, {rect[2], rect[1]}), apply(m, {rect[2], rect[3]}),
                             apply(m, {rect[0], rect[3]})}},
                           false));
    }

    void ColorCanvas::pop_clip() {
        if (clips_.size() > 1) {
            clips_.pop_back();
        }
    }

    template <typename Shade>
    void ColorCanvas::paint(Shade shade) {
        Matrix inverse;
        if (!invert(transforms_.back(), inverse)) {
            return;
        }
        const auto& clip = clips_.back();
        auto& group = groups_.back();
        for (std::uint32_t y = 0; y < height_; ++y) {
            for (std::uint32_t x = 0; x < width_; ++x) {
                const auto i = std::size_t(y) * width_ + x;
                const float c = clip[i];
                if (c <= 0) {
                    continue;
                }
                auto s = shade(apply(inverse, {x + .5, y + .5}));
                if (s[3] <= 0) {
                    continue;
                }
                auto& d = group[i];
                const float keep = 1 - s[3] * c;
                for (int k = 0; k < 4; ++k) {
                    d[k] = s[k] * c + d[k] * keep;
                }
            }
        }
    }

    void ColorCanvas::fill(const Color& color) {
        const auto c = premultiply(color);
        paint([&](Point) {
            return c;
        });
    }

    void ColorCanvas::linear_gradient(std::vector<Stop> stops, Extend extend, Point p0, Point p1, Point p2) {
        std::stable_sort(stops.begin(), stops.end(), [](const Stop& a, const Stop& b) {
            return a.offset < b.offset;
        });
        // p1 projected onto the line through p0 perpendicular to p0p2: color lines run parallel to p0p2.
        const Point normal{p2[1] - p0[1], -(p2[0] - p0[0])};
        const double nn = normal[0] * normal[0] + normal[1] * normal[1];
        Point axis{p1[0] - p0[0], p1[1] - p0[1]};
        if (nn > 0) {
            const double k = (axis[0] * normal[0] + axis[1] * normal[1]) / nn;
            axis = {normal[0] * k, normal[1] * k};
        }
        const double length = axis[0] * axis[0] + axis[1] * axis[1];
        if (!(length > 0) || stops.empty()) {
            return;
        }
        paint([&](Point p) {
            return color_line(stops, extend, ((p[0] - p0[0]) * axis[0] + (p[1] - p0[1]) * axis[1]) / length);
        });
    }

    void ColorCanvas::radial_gradient(std::vector<Stop> stops, Extend extend, Point c0, double r0, Point c1,
                                      double r1) {
        std::stable_sort(stops.begin(), stops.end(), [](const Stop& a, const Stop& b) {
            return a.offset < b.offset;
        });
        if (stops.empty()) {
            return;
        }
        // The largest t whose circle (c0 + t (c1 - c0), r0 + t (r1 - r0)) passes through p with a radius >= 0.
        const Point cd{c1[0] - c0[0], c1[1] - c0[1]};
        const double dr = r1 - r0;
        const double a = cd[0] * cd[0] + cd[1] * cd[1] - dr * dr;
        paint([&](Point p) -> Color {
            const Point pd{p[0] - c0[0], p[1] - c0[1]};
            const double b = pd[0] * cd[0] + pd[1] * cd[1] + r0 * dr;
            const double c = pd[0] * pd[0] + pd[1] * pd[1] - r0 * r0;
            const auto valid = [&](double t) {
                return std::isfinite(t) && r0 + t * dr >= 0;
            };
            if (std::abs(a) < limits::color_singular_epsilon) {
                if (std::abs(b) < limits::color_singular_epsilon) {
                    return {};
                }
                const double t = c / (2 * b);
                return valid(t) ? color_line(stops, extend, t) : Color{};
            }
            const double discriminant = b * b - a * c;
            if (discriminant < 0) {
                return {};
            }
            const double root = std::sqrt(discriminant);
            const double high = std::max((b + root) / a, (b - root) / a);
            const double low = std::min((b + root) / a, (b - root) / a);
            if (valid(high)) {
                return color_line(stops, extend, high);
            }
            return valid(low) ? color_line(stops, extend, low) : Color{};
        });
    }

    void ColorCanvas::sweep_gradient(std::vector<Stop> stops, Extend extend, Point center, double start, double end) {
        std::stable_sort(stops.begin(), stops.end(), [](const Stop& a, const Stop& b) {
            return a.offset < b.offset;
        });
        if (stops.empty() || !(std::abs(end - start) > 0)) {
            return;
        }
        paint([&](Point p) {
            double angle = std::atan2(p[1] - center[1], p[0] - center[0]);
            if (angle < 0) {
                angle += 2 * std::numbers::pi;
            }
            return color_line(stops, extend, (angle - start) / (end - start));
        });
    }

    void ColorCanvas::push_group() {
        if (skipped_groups_ || groups_.size() > std::size_t(limits::max_color_groups)) {
            ++skipped_groups_;
            overflowed_ = true;
            return;
        }
        groups_.emplace_back(std::size_t(width_) * height_, Color{});
    }

    void ColorCanvas::pop_group(Composite mode) {
        if (skipped_groups_) {
            --skipped_groups_;
            return;
        }
        if (groups_.size() < 2) {
            return;
        }
        const auto source = std::move(groups_.back());
        groups_.pop_back();
        auto& backdrop = groups_.back();
        const auto& clip = clips_.back();
        // Inside the current clip, the composite replaces the backdrop; outside it the backdrop stays.
        for (std::size_t i = 0; i < backdrop.size(); ++i) {
            const float c = clip[i];
            if (c <= 0) {
                continue;
            }
            const auto mixed = composite(mode, source[i], backdrop[i]);
            for (int k = 0; k < 4; ++k) {
                backdrop[i][k] += (mixed[k] - backdrop[i][k]) * c;
            }
        }
    }

    std::vector<std::uint8_t> ColorCanvas::pixels() const {
        std::vector<std::uint8_t> out(groups_.front().size() * 4);
        for (std::size_t i = 0; i < groups_.front().size(); ++i) {
            for (int k = 0; k < 4; ++k) {
                out[i * 4 + k] = std::uint8_t(
                    std::floor(std::clamp(groups_.front()[i][k], 0.f, 1.f) * constants::color_channel_max + .5f));
            }
        }
        return out;
    }
} // namespace weft
