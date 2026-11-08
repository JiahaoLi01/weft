#include "scene/scene_path.hpp"

#include "scene/scene_geometry.hpp"
#include "scene/scene_values.hpp"
#include "constants/common.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace weft {
    namespace {
        using Point = std::array<double, 2>;
        using Verb = paint::Path::Verb;
        constexpr double pi = std::numbers::pi;
        // Control point distance of a cubic quarter circle, as a fraction of the radius.
        constexpr double quarter_circle_kappa = 0.5522847498307936;

        class Builder {
        public:
            explicit Builder(paint::Path& path) : path_(path) {
            }

            void move(Point p) {
                path_.verbs.push_back(Verb::Move);
                add(p);
                current_ = p;
            }

            void line(Point p) {
                path_.verbs.push_back(Verb::Line);
                add(p);
                current_ = p;
            }

            void cubic(Point c1, Point c2, Point p) {
                path_.verbs.push_back(Verb::Cubic);
                add(c1);
                add(c2);
                add(p);
                current_ = p;
            }

            void quadratic(Point c, Point p) {
                const auto p0 = current_;
                cubic({p0[0] + 2.0 / 3 * (c[0] - p0[0]), p0[1] + 2.0 / 3 * (c[1] - p0[1])},
                      {p[0] + 2.0 / 3 * (c[0] - p[0]), p[1] + 2.0 / 3 * (c[1] - p[1])}, p);
            }

            // An arc as cubics of at most a quarter turn each, ending exactly at end.
            void arc(const SceneArc& a, Point end) {
                const int count = std::max(1, int(std::ceil(std::abs(a.sweep) / (pi / 2) - 1e-9)));
                const double delta = a.sweep / count;
                const double k = 4.0 / 3 * std::tan(delta / 4);
                const double c = std::cos(a.phi);
                const double s = std::sin(a.phi);
                // A point of the unit circle's plane on the ellipse.
                const auto place = [&](double x, double y) {
                    return Point{a.cx + a.rx * c * x - a.ry * s * y, a.cy + a.rx * s * x + a.ry * c * y};
                };
                for (int i = 0; i < count; ++i) {
                    const double t0 = a.start + delta * i;
                    const double t1 = t0 + delta;
                    const double x0 = std::cos(t0);
                    const double y0 = std::sin(t0);
                    const double x1 = std::cos(t1);
                    const double y1 = std::sin(t1);
                    cubic(place(x0 - k * y0, y0 + k * x0), place(x1 + k * y1, y1 - k * x1),
                          i + 1 == count ? end : place(x1, y1));
                }
            }

            void close() {
                path_.verbs.push_back(Verb::Close);
            }

            Point current() const {
                return current_;
            }

        private:
            void add(Point p) {
                path_.points.push_back({float(p[0]), float(p[1])});
            }

            paint::Path& path_;
            Point current_{};
        };

        // A quarter ellipse from the end of one side to the start of the next, turning clockwise.
        void corner(Builder& b, Point from, Point to, double rx, double ry) {
            if (auto a = scene_center_arc(from, rx, ry, 0, false, true, to)) {
                b.arc(*a, to);
            } else {
                b.line(to);
            }
        }
    } // namespace

    std::array<double, 2> SceneArc::at(double t) const {
        const double c = std::cos(t);
        const double s = std::sin(t);
        return {cx + rx * std::cos(phi) * c - ry * std::sin(phi) * s,
                cy + rx * std::sin(phi) * c + ry * std::cos(phi) * s};
    }

    std::optional<SceneArc> scene_center_arc(std::array<double, 2> p0, double rx, double ry, double degrees, bool large,
                                             bool sweep, std::array<double, 2> p1) {
        rx = std::abs(rx);
        ry = std::abs(ry);
        if (rx == 0 || ry == 0 || (p0[0] == p1[0] && p0[1] == p1[1])) {
            return std::nullopt;
        }
        const double phi = degrees * pi / constants::degrees_per_half_turn;
        const double c = std::cos(phi);
        const double s = std::sin(phi);
        const double hx = (p0[0] - p1[0]) / 2;
        const double hy = (p0[1] - p1[1]) / 2;
        const double x1 = c * hx + s * hy;
        const double y1 = -s * hx + c * hy;
        const double lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
        if (lambda > 1) {
            rx *= std::sqrt(lambda);
            ry *= std::sqrt(lambda);
        }
        const double numerator = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
        const double denominator = rx * rx * y1 * y1 + ry * ry * x1 * x1;
        double factor = denominator > 0 ? std::sqrt(std::max(0.0, numerator / denominator)) : 0;
        if (large == sweep) {
            factor = -factor;
        }
        const double cx1 = factor * rx * y1 / ry;
        const double cy1 = -factor * ry * x1 / rx;
        SceneArc a;
        a.cx = c * cx1 - s * cy1 + (p0[0] + p1[0]) / 2;
        a.cy = s * cx1 + c * cy1 + (p0[1] + p1[1]) / 2;
        a.rx = rx;
        a.ry = ry;
        a.phi = phi;
        const auto angle = [](double ux, double uy, double vx, double vy) {
            return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
        };
        a.start = angle(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry);
        a.sweep = angle((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry);
        if (!sweep && a.sweep > 0) {
            a.sweep -= 2 * pi;
        } else if (sweep && a.sweep < 0) {
            a.sweep += 2 * pi;
        }
        return a;
    }

    paint::Stroke scene_svg_stroke(const SceneStyle& style) {
        using scene_values::get;
        using scene_values::len;
        paint::Stroke out;
        out.width = len(get(style, PropertyId::StrokeWidth, "1"));
        const auto cap = get(style, PropertyId::StrokeLinecap, "butt");
        const auto join = get(style, PropertyId::StrokeLinejoin, "miter");
        if (cap != "butt" && cap != "round" && cap != "square" && !cap.empty()) {
            scene_values::fail("stroke linecap");
        }
        if (join != "miter" && join != "round" && join != "bevel" && !join.empty()) {
            scene_values::fail("stroke linejoin");
        }
        out.cap = cap == "round"    ? paint::Stroke::Cap::Round
                  : cap == "square" ? paint::Stroke::Cap::Square
                                    : paint::Stroke::Cap::Butt;
        out.join = join == "round"   ? paint::Stroke::Join::Round
                   : join == "bevel" ? paint::Stroke::Join::Bevel
                                     : paint::Stroke::Join::Miter;
        out.miter_limit = constants::scene::svg_miter_limit;
        const auto dashes = get(style, PropertyId::StrokeDasharray, "none");
        if (dashes != "none" && !dashes.empty()) {
            for (const auto v : parse_scene_numbers(dashes)) {
                if (v < 0 || out.dashes.size() >= constants::scene::max_stroke_dashes) {
                    scene_values::fail("stroke dash bound");
                }
                out.dashes.push_back(v);
            }
            if (out.dashes.size() % 2) {
                const auto copy = out.dashes;
                out.dashes.insert(out.dashes.end(), copy.begin(), copy.end());
            }
            if (std::all_of(out.dashes.begin(), out.dashes.end(), [](float v) {
                    return v == 0;
                })) {
                out.dashes.clear();
            }
        }
        out.dash_offset = len(get(style, PropertyId::StrokeDashoffset, "0"));
        return out;
    }

    std::optional<SceneSvgShape> scene_svg_shape(const SceneNode& n) {
        using scene_values::len;
        const auto& a = n.attributes;
        const auto v = [&](const char* k, float def = 0) {
            const auto i = a.find(k);
            return i == a.end() ? def : len(i->second);
        };
        using Kind = SceneSvgShape::Kind;
        if (n.tag == "rect") {
            const double x = v("x");
            const double y = v("y");
            const double w = v("width");
            const double h = v("height");
            if (!(w > 0) || !(h > 0)) {
                return std::nullopt;
            }
            // Radii: either one stands for both, each at most half the side (as the reference raster clamps them).
            const float rx_value = v("rx");
            const double rx = std::min(std::max(double(rx_value), 0.0), w / 2);
            const double ry = std::min(std::max(double(v("ry", rx_value)), 0.0), h / 2);
            const bool rounded = rx > 0 && ry > 0;
            return SceneSvgShape{Kind::Rect, x, y, w, h, rounded ? rx : 0, rounded ? ry : 0};
        }
        if (n.tag == "circle" || n.tag == "ellipse") {
            const double cx = v("cx");
            const double cy = v("cy");
            const double rx = n.tag == "circle" ? v("r") : v("rx");
            const double ry = n.tag == "circle" ? v("r") : v("ry");
            if (!(rx > 0) || !(ry > 0)) {
                return std::nullopt;
            }
            return SceneSvgShape{Kind::Ellipse, cx, cy, 0, 0, rx, ry};
        }
        if (n.tag == "line") {
            return SceneSvgShape{Kind::Line, v("x1"), v("y1"), v("x2"), v("y2"), 0, 0};
        }
        return std::nullopt;
    }

    std::shared_ptr<const paint::Path> scene_svg_path(const SceneNode& n) {
        using scene_values::len;
        const auto& a = n.attributes;
        auto out = std::make_shared<paint::Path>();
        out->evenodd = scene_values::get(n.style, PropertyId::FillRule) == "evenodd";
        Builder b(*out);
        if (n.tag == "rect") {
            const auto shape = scene_svg_shape(n);
            if (!shape) {
                return {};
            }
            const double x = shape->x;
            const double y = shape->y;
            const double w = shape->w;
            const double h = shape->h;
            const double rx = shape->rx;
            const double ry = shape->ry;
            if (rx <= 0 || ry <= 0) {
                b.move({x, y});
                b.line({x + w, y});
                b.line({x + w, y + h});
                b.line({x, y + h});
                b.close();
                return out;
            }
            b.move({x + rx, y});
            b.line({x + w - rx, y});
            corner(b, {x + w - rx, y}, {x + w, y + ry}, rx, ry);
            b.line({x + w, y + h - ry});
            corner(b, {x + w, y + h - ry}, {x + w - rx, y + h}, rx, ry);
            b.line({x + rx, y + h});
            corner(b, {x + rx, y + h}, {x, y + h - ry}, rx, ry);
            b.line({x, y + ry});
            corner(b, {x, y + ry}, {x + rx, y}, rx, ry);
            b.close();
            return out;
        }
        if (n.tag == "circle" || n.tag == "ellipse") {
            const auto shape = scene_svg_shape(n);
            if (!shape) {
                return {};
            }
            const double cx = shape->x;
            const double cy = shape->y;
            const double rx = shape->rx;
            const double ry = shape->ry;
            const double kx = rx * quarter_circle_kappa;
            const double ky = ry * quarter_circle_kappa;
            b.move({cx + rx, cy});
            b.cubic({cx + rx, cy + ky}, {cx + kx, cy + ry}, {cx, cy + ry});
            b.cubic({cx - kx, cy + ry}, {cx - rx, cy + ky}, {cx - rx, cy});
            b.cubic({cx - rx, cy - ky}, {cx - kx, cy - ry}, {cx, cy - ry});
            b.cubic({cx + kx, cy - ry}, {cx + rx, cy - ky}, {cx + rx, cy});
            b.close();
            return out;
        }
        if (n.tag == "line") {
            const auto shape = scene_svg_shape(n);
            b.move({shape->x, shape->y});
            b.line({shape->w, shape->h});
            return out;
        }
        if (n.tag == "polygon" || n.tag == "polyline") {
            const auto it = a.find("points");
            const auto numbers = parse_scene_numbers(it == a.end() ? std::string_view{} : std::string_view(it->second));
            if (numbers.size() % 2) {
                scene_values::fail("points pairs");
            }
            if (numbers.empty()) {
                return {};
            }
            for (std::size_t i = 0; i + 1 < numbers.size(); i += 2) {
                if (i == 0) {
                    b.move({numbers[i], numbers[i + 1]});
                } else {
                    b.line({numbers[i], numbers[i + 1]});
                }
            }
            if (n.tag == "polygon") {
                b.close();
            }
            return out;
        }
        if (n.tag == "path") {
            const auto it = a.find("d");
            if (it == a.end()) {
                return {};
            }
            return scene_path_data(it->second, out->evenodd);
        }
        return {};
    }

    std::shared_ptr<const paint::Path> scene_path_data(std::string_view data, bool evenodd) {
        auto out = std::make_shared<paint::Path>();
        out->evenodd = evenodd;
        Builder b(*out);
        using Kind = ScenePathCommand::Kind;
        for (const auto& c : parse_scene_path(data)) {
            switch (c.kind) {
                case Kind::Move:
                    b.move({c.v[0], c.v[1]});
                    break;
                case Kind::Line:
                    b.line({c.v[0], c.v[1]});
                    break;
                case Kind::Cubic:
                    b.cubic({c.v[0], c.v[1]}, {c.v[2], c.v[3]}, {c.v[4], c.v[5]});
                    break;
                case Kind::Quadratic:
                    b.quadratic({c.v[0], c.v[1]}, {c.v[2], c.v[3]});
                    break;
                case Kind::Arc: {
                    const Point end{c.v[5], c.v[6]};
                    const auto p0 = b.current();
                    if (p0 == end) {
                        break;
                    }
                    if (auto arc = scene_center_arc(p0, c.v[0], c.v[1], c.v[2], c.v[3] != 0, c.v[4] != 0, end)) {
                        b.arc(*arc, end);
                    } else {
                        b.line(end);
                    }
                    break;
                }
                case Kind::Close:
                    b.close();
                    break;
            }
        }
        if (out->verbs.empty()) {
            return {};
        }
        return out;
    }
} // namespace weft
