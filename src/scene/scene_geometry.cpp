#include "scene/scene_geometry.hpp"
#include "scene/scene_values.hpp"

#include "base/css_scalar.hpp"
#include "base/diagnostic.hpp"
#include "base/owner_thread.hpp"
#include "constants/common.hpp"
#include "constants/scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <optional>
#include <unordered_map>

namespace weft {
    namespace {
        using Style = SceneStyle;
        constexpr double pi = std::numbers::pi;

        using scene_values::fail;
        using scene_values::get;
        using scene_values::len;
        using scene_values::num;

        std::vector<std::string> split(std::string_view s, char delimiter = ' ') {
            return css::split(s, delimiter, {.quotes = false}).parts;
        }

        struct Scanner {
            std::string value;
            std::size_t p{};

            void spaces() {
                while (p < value.size() && (std::isspace(static_cast<unsigned char>(value[p])) || value[p] == ',')) {
                    ++p;
                }
            }

            bool end() {
                spaces();
                return p == value.size();
            }

            bool has_number() {
                spaces();
                return p < value.size() && (std::isdigit(static_cast<unsigned char>(value[p])) || value[p] == '+' ||
                                            value[p] == '-' || value[p] == '.');
            }

            float number() {
                spaces();
                char* end{};
                const auto v = std::strtof(value.c_str() + p, &end);
                if (end == value.c_str() + p || !std::isfinite(v)) {
                    fail("path number near " + value.substr(p, constants::scene::path_diagnostic_excerpt_chars));
                }
                p = std::size_t(end - value.c_str());
                return v;
            }
        };

        using Plane = ScenePlane;

        // An SVG element's transform attribute, or its transform property, in user units.
        SceneAffine svg_affine(const SceneNode& n) {
            const auto text =
                n.attributes.contains("transform") ? n.attributes.at("transform") : get(n.style, PropertyId::Transform);
            return scene_affine(parse_scene_transform(text, true).value(), 0, 0);
        }

        SceneAffine viewport_affine(const SceneNode& n) {
            const auto v = scene_svg_viewport(n);
            return {v.sx, 0, 0, v.sy, v.tx, v.ty};
        }

        using Point = std::array<double, 2>;

        struct Bounds {
            double left = constants::scene::geometry_bounds_seed;
            double top = constants::scene::geometry_bounds_seed;
            double right = -constants::scene::geometry_bounds_seed;
            double bottom = -constants::scene::geometry_bounds_seed;

            void add(double x, double y) {
                left = std::min(left, x);
                top = std::min(top, y);
                right = std::max(right, x);
                bottom = std::max(bottom, y);
            }
        };

        // A flattened SVG shape: closed polygons for fill, polylines for stroke, exact (unflattened) bounds.
        struct Shape : HitOutline {
            Bounds bounds;
        };

        // Points of an elliptical arc in center form, endpoints excluded; SVG implementation notes F.6.5.
        struct Arc {
            double cx{};
            double cy{};
            double rx{};
            double ry{};
            double phi{};
            double start{};
            double sweep{};

            Point at(double t) const {
                const double c = std::cos(t);
                const double s = std::sin(t);
                return {cx + rx * std::cos(phi) * c - ry * std::sin(phi) * s,
                        cy + rx * std::sin(phi) * c + ry * std::cos(phi) * s};
            }
        };

        std::optional<Arc> center_arc(Point p0, double rx, double ry, double degrees, bool large, bool sweep,
                                      Point p1) {
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
            Arc a;
            a.cx = c * cx1 - s * cy1 + (p0[0] + p1[0]) / 2;
            a.cy = s * cx1 + c * cy1 + (p0[1] + p1[1]) / 2;
            a.rx = rx;
            a.ry = ry;
            a.phi = phi;
            auto angle = [](double ux, double uy, double vx, double vy) {
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

        // Builds flattened figures and exact bounds from path commands.
        class ShapeBuilder {
        public:
            explicit ShapeBuilder(Shape& shape) : shape_(shape) {
            }

            void move(Point p) {
                finish(false);
                current_.push_back(p);
                shape_.bounds.add(p[0], p[1]);
            }

            void line(Point p) {
                current_.push_back(p);
                shape_.bounds.add(p[0], p[1]);
            }

            void cubic(Point c1, Point c2, Point p) {
                const Point p0 = current_.back();
                // Exact bounds: endpoints and the extrema where a coordinate's derivative vanishes.
                for (int axis = 0; axis < 2; ++axis) {
                    const double a = -p0[axis] + 3 * c1[axis] - 3 * c2[axis] + p[axis];
                    const double b = 2 * (p0[axis] - 2 * c1[axis] + c2[axis]);
                    const double c = c1[axis] - p0[axis];
                    for (double t : roots(a, b, c)) {
                        const auto q = cubic_at(p0, c1, c2, p, t);
                        shape_.bounds.add(q[0], q[1]);
                    }
                }
                flatten_cubic(p0, c1, c2, p, 0);
                shape_.bounds.add(p[0], p[1]);
            }

            void quadratic(Point c, Point p) {
                const Point p0 = current_.back();
                cubic({p0[0] + 2.0 / 3 * (c[0] - p0[0]), p0[1] + 2.0 / 3 * (c[1] - p0[1])},
                      {p[0] + 2.0 / 3 * (c[0] - p[0]), p[1] + 2.0 / 3 * (c[1] - p[1])}, p);
            }

            void arc(const Arc& a, Point end) {
                // Bounds: endpoints and each axis extremum inside the swept range.
                for (int axis = 0; axis < 2; ++axis) {
                    const double t0 = axis == 0 ? std::atan2(-a.ry * std::sin(a.phi), a.rx * std::cos(a.phi))
                                                : std::atan2(a.ry * std::cos(a.phi), a.rx * std::sin(a.phi));
                    for (int k = -4; k <= 4; ++k) {
                        const double t = t0 + k * pi;
                        const double from = std::min(a.start, a.start + a.sweep);
                        const double to = std::max(a.start, a.start + a.sweep);
                        if (t > from && t < to) {
                            const auto q = a.at(t);
                            shape_.bounds.add(q[0], q[1]);
                        }
                    }
                }
                const double radius = std::max(a.rx, a.ry);
                const double step =
                    2 * std::acos(std::clamp(1 - constants::scene::geometry_flattening_tolerance / radius, -1.0, 1.0));
                const int count = std::max(1, int(std::ceil(std::abs(a.sweep) / std::max(step, 1e-3))));
                for (int i = 1; i < count; ++i) {
                    current_.push_back(a.at(a.start + a.sweep * i / count));
                }
                line(end);
            }

            void close() {
                finish(true);
            }

            void finish(bool closed) {
                if (!current_.empty()) {
                    shape_.figures.push_back({std::move(current_), closed});
                    current_.clear();
                }
            }

            Point current() const {
                return current_.empty() ? Point{} : current_.back();
            }

        private:
            static std::vector<double> roots(double a, double b, double c) {
                std::vector<double> out;
                auto keep = [&](double t) {
                    if (t > 0 && t < 1) {
                        out.push_back(t);
                    }
                };
                if (std::abs(a) < 1e-12) {
                    if (std::abs(b) > 1e-12) {
                        keep(-c / b);
                    }
                    return out;
                }
                const double d = b * b - 4 * a * c;
                if (d >= 0) {
                    keep((-b + std::sqrt(d)) / (2 * a));
                    keep((-b - std::sqrt(d)) / (2 * a));
                }
                return out;
            }

            static Point cubic_at(Point p0, Point c1, Point c2, Point p, double t) {
                const double u = 1 - t;
                return {u * u * u * p0[0] + 3 * u * u * t * c1[0] + 3 * u * t * t * c2[0] + t * t * t * p[0],
                        u * u * u * p0[1] + 3 * u * u * t * c1[1] + 3 * u * t * t * c2[1] + t * t * t * p[1]};
            }

            void flatten_cubic(Point p0, Point c1, Point c2, Point p, int depth) {
                // Control points within tolerance of the chord, or the depth bound, end the subdivision.
                const double dx = p[0] - p0[0];
                const double dy = p[1] - p0[1];
                const double length = std::hypot(dx, dy);
                auto distance = [&](Point q) {
                    return length > 0 ? std::abs((q[0] - p0[0]) * dy - (q[1] - p0[1]) * dx) / length
                                      : std::hypot(q[0] - p0[0], q[1] - p0[1]);
                };
                if (depth >= constants::scene::geometry_max_subdivision ||
                    std::max(distance(c1), distance(c2)) <= constants::scene::geometry_flattening_tolerance) {
                    current_.push_back(p);
                    return;
                }
                const Point a{(p0[0] + c1[0]) / 2, (p0[1] + c1[1]) / 2};
                const Point b{(c1[0] + c2[0]) / 2, (c1[1] + c2[1]) / 2};
                const Point c{(c2[0] + p[0]) / 2, (c2[1] + p[1]) / 2};
                const Point ab{(a[0] + b[0]) / 2, (a[1] + b[1]) / 2};
                const Point bc{(b[0] + c[0]) / 2, (b[1] + c[1]) / 2};
                const Point mid{(ab[0] + bc[0]) / 2, (ab[1] + bc[1]) / 2};
                flatten_cubic(p0, a, ab, mid, depth + 1);
                flatten_cubic(mid, bc, c, p, depth + 1);
            }

            Shape& shape_;
            std::vector<Point> current_;
        };

        void build_path(const std::vector<ScenePathCommand>& commands, Shape& shape) {
            ShapeBuilder b(shape);
            for (const auto& c : commands) {
                switch (c.kind) {
                    case ScenePathCommand::Kind::Move:
                        b.move({c.v[0], c.v[1]});
                        break;
                    case ScenePathCommand::Kind::Line:
                        b.line({c.v[0], c.v[1]});
                        break;
                    case ScenePathCommand::Kind::Cubic:
                        b.cubic({c.v[0], c.v[1]}, {c.v[2], c.v[3]}, {c.v[4], c.v[5]});
                        break;
                    case ScenePathCommand::Kind::Quadratic:
                        b.quadratic({c.v[0], c.v[1]}, {c.v[2], c.v[3]});
                        break;
                    case ScenePathCommand::Kind::Arc: {
                        const Point end{c.v[5], c.v[6]};
                        const auto p0 = b.current();
                        if (p0[0] == end[0] && p0[1] == end[1]) {
                            break;
                        }
                        if (auto arc = center_arc(p0, c.v[0], c.v[1], c.v[2], c.v[3] != 0, c.v[4] != 0, end)) {
                            b.arc(*arc, end);
                        } else {
                            b.line(end);
                        }
                        break;
                    }
                    case ScenePathCommand::Kind::Close:
                        b.close();
                        break;
                }
            }
            b.finish(false);
        }

        // Rounded rectangle as path commands, radii clamped like D2D (to half the width and height).
        void build_rect(Shape& shape, double x, double y, double w, double h, double rx, double ry) {
            rx = std::min(std::max(rx, 0.0), w / 2);
            ry = std::min(std::max(ry, 0.0), h / 2);
            ShapeBuilder b(shape);
            if (rx <= 0 || ry <= 0) {
                b.move({x, y});
                b.line({x + w, y});
                b.line({x + w, y + h});
                b.line({x, y + h});
                b.close();
                shape.bounds = {};
                shape.bounds.add(x, y);
                shape.bounds.add(x + w, y + h);
                return;
            }
            auto corner = [&](Point from, Point to) {
                if (auto a = center_arc(from, rx, ry, 0, false, true, to)) {
                    b.arc(*a, to);
                }
            };
            b.move({x + rx, y});
            b.line({x + w - rx, y});
            corner({x + w - rx, y}, {x + w, y + ry});
            b.line({x + w, y + h - ry});
            corner({x + w, y + h - ry}, {x + w - rx, y + h});
            b.line({x + rx, y + h});
            corner({x + rx, y + h}, {x, y + h - ry});
            b.line({x, y + ry});
            corner({x, y + ry}, {x + rx, y});
            b.close();
            shape.bounds = {};
            shape.bounds.add(x, y);
            shape.bounds.add(x + w, y + h);
        }

        void build_ellipse(Shape& shape, double cx, double cy, double rx, double ry) {
            ShapeBuilder b(shape);
            b.move({cx + rx, cy});
            if (auto a = center_arc({cx + rx, cy}, rx, ry, 0, false, true, {cx - rx, cy})) {
                b.arc(*a, {cx - rx, cy});
            }
            if (auto a = center_arc({cx - rx, cy}, rx, ry, 0, false, true, {cx + rx, cy})) {
                b.arc(*a, {cx + rx, cy});
            }
            b.close();
            shape.bounds = {};
            shape.bounds.add(cx - rx, cy - ry);
            shape.bounds.add(cx + rx, cy + ry);
        }
    } // namespace

    SceneViewport scene_svg_viewport(const SceneNode& n) {
        auto i = n.attributes.find("viewBox");
        if (i == n.attributes.end()) {
            return {};
        }
        Scanner s{i->second};
        const auto x = s.number();
        const auto y = s.number();
        const auto w = s.number();
        const auto h = s.number();
        if (!s.end() || w <= 0 || h <= 0) {
            fail("viewBox dimensions");
        }
        float sx = n.box[2] / w;
        float sy = n.box[3] / h;
        float dx{};
        float dy{};
        auto aspect =
            n.attributes.contains("preserveAspectRatio") ? n.attributes.at("preserveAspectRatio") : "xMidYMid meet";
        auto a = split(aspect);
        if (a.empty()) {
            fail("preserveAspectRatio");
        }
        if (a[0] != "none") {
            if (a[0].size() != 8 || a[0][0] != 'x' || a[0][4] != 'Y' || a.size() > 2) {
                fail("preserveAspectRatio alignment");
            }
            auto mode = a.size() > 1 ? a[1] : "meet";
            if (mode != "meet" && mode != "slice") {
                fail("preserveAspectRatio mode");
            }
            sx = sy = mode == "slice" ? std::max(sx, sy) : std::min(sx, sy);
            auto align = [](std::string v) {
                if (v == "Min") {
                    return 0.f;
                }
                if (v == "Mid") {
                    return .5f;
                }
                if (v == "Max") {
                    return 1.f;
                }
                fail("preserveAspectRatio alignment");
            };
            dx = (n.box[2] - w * sx) * align(a[0].substr(1, 3));
            dy = (n.box[3] - h * sy) * align(a[0].substr(5, 3));
        }
        return {sx, sy, dx - x * sx, dy - y * sy};
    }

    std::vector<ScenePathCommand> parse_scene_path(std::string_view data) {
        if (data.size() > constants::scene::max_svg_path_bytes) {
            fail("SVG path byte bound");
        }
        using Kind = ScenePathCommand::Kind;
        std::vector<ScenePathCommand> out;
        Scanner s{std::string(data)};
        char command{};
        std::array<float, 2> point{};
        std::array<float, 2> start{};
        bool open{};
        unsigned segments{};
        char previous{};
        std::array<float, 2> control{};
        while (!s.end()) {
            if (++segments > constants::scene::max_svg_path_segments) {
                fail("path segment bound");
            }
            if (!s.has_number()) {
                command = s.value[s.p++];
            }
            const bool relative = std::islower(static_cast<unsigned char>(command));
            const char c = char(std::toupper(static_cast<unsigned char>(command)));
            auto endpoint = [&] {
                float x = s.number();
                float y = s.number();
                return std::array<float, 2>{x + (relative ? point[0] : 0), y + (relative ? point[1] : 0)};
            };
            if (c == 'M') {
                auto p = endpoint();
                out.push_back({Kind::Move, {p[0], p[1]}});
                point = start = p;
                open = true;
                command = relative ? 'l' : 'L';
            } else if (c == 'Z') {
                if (open) {
                    out.push_back({Kind::Close, {}});
                }
                open = false;
                point = start;
                command = 0;
            } else {
                if (!open) {
                    out.push_back({Kind::Move, {point[0], point[1]}});
                    start = point;
                    open = true;
                }
                if (c == 'L') {
                    point = endpoint();
                    out.push_back({Kind::Line, {point[0], point[1]}});
                } else if (c == 'H') {
                    point[0] = s.number() + (relative ? point[0] : 0);
                    out.push_back({Kind::Line, {point[0], point[1]}});
                } else if (c == 'V') {
                    point[1] = s.number() + (relative ? point[1] : 0);
                    out.push_back({Kind::Line, {point[0], point[1]}});
                } else if (c == 'C') {
                    auto p1 = endpoint();
                    auto p2 = endpoint();
                    auto p3 = endpoint();
                    out.push_back({Kind::Cubic, {p1[0], p1[1], p2[0], p2[1], p3[0], p3[1]}});
                    control = p2;
                    point = p3;
                } else if (c == 'S') {
                    auto p1 = previous == 'C' || previous == 'S'
                                  ? std::array<float, 2>{2 * point[0] - control[0], 2 * point[1] - control[1]}
                                  : point;
                    auto p2 = endpoint();
                    auto p3 = endpoint();
                    out.push_back({Kind::Cubic, {p1[0], p1[1], p2[0], p2[1], p3[0], p3[1]}});
                    control = p2;
                    point = p3;
                } else if (c == 'Q') {
                    auto p1 = endpoint();
                    auto p2 = endpoint();
                    out.push_back({Kind::Quadratic, {p1[0], p1[1], p2[0], p2[1]}});
                    control = p1;
                    point = p2;
                } else if (c == 'T') {
                    auto p1 = previous == 'Q' || previous == 'T'
                                  ? std::array<float, 2>{2 * point[0] - control[0], 2 * point[1] - control[1]}
                                  : point;
                    auto p2 = endpoint();
                    out.push_back({Kind::Quadratic, {p1[0], p1[1], p2[0], p2[1]}});
                    control = p1;
                    point = p2;
                } else if (c == 'A') {
                    auto rx = std::abs(s.number());
                    auto ry = std::abs(s.number());
                    auto angle = s.number();
                    auto large = s.number();
                    auto sweep = s.number();
                    auto p = endpoint();
                    if ((large != 0 && large != 1) || (sweep != 0 && sweep != 1)) {
                        fail("arc flags");
                    }
                    out.push_back({Kind::Arc, {rx, ry, angle, large, sweep, p[0], p[1]}});
                    point = p;
                } else {
                    fail("unsupported SVG path command " + std::string(1, c));
                }
            }
            previous = c;
        }
        return out;
    }

    std::vector<float> parse_scene_numbers(std::string_view list) {
        std::vector<float> out;
        Scanner s{std::string(list)};
        while (!s.end()) {
            out.push_back(s.number());
        }
        return out;
    }

    struct SceneGeometry::Impl {
        OwnerThread owner;
        SceneText& text;
        std::unordered_map<std::string, std::shared_ptr<const Shape>> shapes;
        std::size_t shape_bytes{};

        explicit Impl(SceneText& t) : text(t) {
        }

        void require_owner() const {
            owner.require(Diag::SceneRasterOwner);
        }

        // Fill and stroke geometry of an SVG element in its user space; empty for groups, text and definitions.
        std::shared_ptr<const Shape> shape(const SceneNode& n) {
            const auto& a = n.attributes;
            if (n.tag == "text" || n.tag == "g" || n.tag == "svg" || n.tag == "defs" || n.tag == "clipPath") {
                return {};
            }
            std::string key = n.tag;
            auto add = [&](std::string_view v) {
                key += std::to_string(v.size()) + ":";
                key += v;
            };
            add(get(n.style, PropertyId::FillRule));
            for (const auto* k :
                 {"x", "y", "width", "height", "rx", "ry", "cx", "cy", "r", "d", "points", "x1", "y1", "x2", "y2"}) {
                if (a.contains(k)) {
                    add(k);
                    add(a.at(k));
                }
            }
            if (auto it = shapes.find(key); it != shapes.end()) {
                return it->second;
            }
            auto v = [&](const char* k, float def = 0) {
                auto i = a.find(k);
                return i == a.end() ? def : len(i->second);
            };
            auto out = std::make_shared<Shape>();
            out->evenodd = get(n.style, PropertyId::FillRule) == "evenodd";
            if (n.tag == "rect") {
                const float rx = v("rx");
                build_rect(*out, v("x"), v("y"), v("width"), v("height"), rx, v("ry", rx));
            } else if (n.tag == "circle") {
                build_ellipse(*out, v("cx"), v("cy"), v("r"), v("r"));
            } else if (n.tag == "ellipse") {
                build_ellipse(*out, v("cx"), v("cy"), v("rx"), v("ry"));
            } else if (n.tag == "path") {
                if (!a.contains("d")) {
                    return {};
                }
                build_path(parse_scene_path(a.at("d")), *out);
            } else if (n.tag == "polygon" || n.tag == "polyline") {
                const auto numbers = parse_scene_numbers(a.contains("points") ? a.at("points") : "");
                std::vector<ScenePathCommand> commands;
                for (std::size_t i = 0; i + 1 < numbers.size(); i += 2) {
                    commands.push_back({i ? ScenePathCommand::Kind::Line : ScenePathCommand::Kind::Move,
                                        {numbers[i], numbers[i + 1]}});
                }
                if (numbers.size() % 2) {
                    fail("path number near " + std::string(a.at("points")));
                }
                if (n.tag == "polygon" && !commands.empty()) {
                    commands.push_back({ScenePathCommand::Kind::Close, {}});
                }
                build_path(commands, *out);
            } else if (n.tag == "line") {
                build_path({{ScenePathCommand::Kind::Move, {v("x1"), v("y1")}},
                            {ScenePathCommand::Kind::Line, {v("x2"), v("y2")}}},
                           *out);
            } else {
                fail("SVG element " + n.tag);
            }
            if (key.size() <= constants::scene::shape_cache_max_bytes) {
                if (shapes.size() >= constants::scene::shape_cache_max_entries ||
                    shape_bytes + key.size() > constants::scene::shape_cache_max_bytes) {
                    shapes.clear();
                    shape_bytes = 0;
                }
                shape_bytes += key.size();
                shapes.emplace(std::move(key), out);
            }
            return out;
        }

        HitStroke stroke(const SceneNode& n) {
            HitStroke s;
            s.width = len(get(n.style, PropertyId::StrokeWidth, "1"));
            const auto cap = get(n.style, PropertyId::StrokeLinecap, "butt");
            s.cap = cap == "round"    ? HitStroke::Cap::Round
                    : cap == "square" ? HitStroke::Cap::Square
                                      : HitStroke::Cap::Butt;
            auto d = get(n.style, PropertyId::StrokeDasharray, "none");
            if (d != "none" && !d.empty()) {
                for (auto value : parse_scene_numbers(d)) {
                    if (value < 0 || s.dashes.size() >= constants::scene::max_stroke_dashes) {
                        fail("stroke dash bound");
                    }
                    s.dashes.push_back(value);
                }
                if (s.dashes.size() % 2) {
                    auto copy = s.dashes;
                    s.dashes.insert(s.dashes.end(), copy.begin(), copy.end());
                }
                if (std::all_of(s.dashes.begin(), s.dashes.end(), [](double v) {
                        return v == 0;
                    })) {
                    s.dashes.clear();
                }
            }
            s.offset = len(get(n.style, PropertyId::StrokeDashoffset, "0"));
            return s;
        }

        // clip-path="url(#id)" of one user-space rect, as in the reference raster.
        std::shared_ptr<const Shape> clip(const SceneNode& n, const SceneNode& root) {
            auto v = n.attributes.contains("clip-path") ? n.attributes.at("clip-path") : "";
            if (v.empty() || v == "none") {
                return {};
            }
            if (!v.starts_with("url(#") || !v.ends_with(')')) {
                fail("local clip-path required");
            }
            const auto id = v.substr(5, v.size() - 6);
            auto find = [&](auto&& self, const auto& c) -> const SceneNode* {
                if (c.attributes.contains("id") && c.attributes.at("id") == id) {
                    return &c;
                }
                for (const auto& child : c.children) {
                    if (auto p = self(self, child)) {
                        return p;
                    }
                }
                return nullptr;
            };
            auto* definition = find(find, root);
            if (!definition || definition->tag != "clipPath" || definition->children.size() != 1 ||
                definition->children[0].tag != "rect" ||
                get(definition->attributes, "clipPathUnits", "userSpaceOnUse") != "userSpaceOnUse") {
                fail("clipPath requires one user-space rect");
            }
            return shape(definition->children[0]);
        }
    };

    SceneGeometry::SceneGeometry(SceneText& text) : impl_(std::make_unique<Impl>(text)) {
    }

    SceneGeometry::~SceneGeometry() = default;

    std::array<double, 6> SceneGeometry::transform_matrix(const SceneNode& n) const {
        impl_->require_owner();
        const auto m = scene_node_affine(n);
        return {m.m11, m.m12, m.m21, m.m22, m.dx, m.dy};
    }

    NodeIdentity SceneGeometry::HitMap::hit(float x, float y) const {
        const auto target = hit_areas(areas, x, y);
        return target ? targets[*target] : NodeIdentity{};
    }

    // Areas in the order the scene paints: children by z-index, SVG content after its svg box. A box clips its
    // descendants when its overflow is hidden or auto, as an svg element does; a clip-path clips its element too.
    SceneGeometry::HitMap SceneGeometry::hit_map(const SceneFrame& f) {
        impl_->require_owner();
        HitMap out;
        const auto area = [&](const SceneNode& n, const Plane& inverse, std::vector<HitShape> shapes,
                              std::size_t clip) {
            out.targets.push_back(n.identity);
            out.areas.areas.push_back({out.targets.size() - 1, inverse, std::move(shapes), clip});
        };
        auto svg_visit = [&](auto&& self, const SceneNode& n, Plane parent, const SceneNode& root,
                             std::size_t clip) -> void {
            if (get(n.style, PropertyId::Display) == "none" || n.tag == "defs" || n.tag == "clipPath") {
                return;
            }
            const auto m = parent * affine_plane(svg_affine(n));
            Plane inverse;
            if (!m.inverse(inverse)) {
                return;
            }
            if (auto path = impl_->clip(n, root)) {
                HitShape shape;
                shape.kind = HitShape::Kind::Fill;
                shape.outline = std::shared_ptr<const HitOutline>(path, path.get());
                clip = out.areas.add_clip(inverse, std::move(shape), clip);
            }
            const auto pointer = get(n.style, PropertyId::PointerEvents, "visiblePainted");
            if (auto shape = impl_->shape(n); shape && pointer != "none") {
                const bool painted = pointer == "auto" || pointer == "painted" || pointer == "visiblePainted";
                const bool fill = pointer == "all" || pointer == "fill" || pointer == "visibleFill" ||
                                  pointer == "visible" ||
                                  (painted && get(n.style, PropertyId::Fill, constants::default_color) != "none");
                const bool stroke = pointer == "all" || pointer == "stroke" || pointer == "visibleStroke" ||
                                    pointer == "visible" ||
                                    (painted && get(n.style, PropertyId::Stroke, "none") != "none");
                std::vector<HitShape> shapes;
                const std::shared_ptr<const HitOutline> outline(shape, shape.get());
                if (fill && n.tag != "line") {
                    HitShape s;
                    s.kind = HitShape::Kind::Fill;
                    s.outline = outline;
                    shapes.push_back(std::move(s));
                }
                if (stroke) {
                    HitShape s;
                    s.kind = HitShape::Kind::Stroke;
                    s.outline = outline;
                    s.stroke = impl_->stroke(n);
                    shapes.push_back(std::move(s));
                }
                if (!shapes.empty()) {
                    area(n, inverse, std::move(shapes), clip);
                }
            }
            if (n.tag == "text" && pointer != "none") {
                auto metrics = impl_->text.measure(n.text, n.style);
                float tx = n.attributes.contains("x") ? len(n.attributes.at("x")) : 0;
                float ty = n.attributes.contains("y") ? len(n.attributes.at("y")) : 0;
                auto anchor = get(n.style, PropertyId::TextAnchor, "start");
                if (anchor == "middle") {
                    tx -= metrics.width / 2;
                }
                if (anchor == "end") {
                    tx -= metrics.width;
                }
                ty -= metrics.baseline;
                area(n, inverse, {HitShape::bounds(tx, ty, metrics.width, metrics.height)}, clip);
            }
            for (const auto& c : n.children) {
                self(self, c, m, root, clip);
            }
        };
        auto visit = [&](auto&& self, const SceneNode& n, Plane parent, std::size_t clip) -> void {
            if (get(n.style, PropertyId::Display) == "none") {
                return;
            }
            const auto m = parent * scene_plane(n);
            Plane inverse;
            if (!m.inverse(inverse)) {
                return;
            }
            // The border box, with its rounded corners as painted.
            const auto box = HitShape::box(0, 0, n.box[2], n.box[3]);
            std::optional<HitShape> corners;
            if (const auto rounded = scene_values::rounded_box(n.style, n.box[2], n.box[3])) {
                corners.emplace();
                corners->kind = HitShape::Kind::Rounded;
                corners->rounded = *rounded;
            }
            if (n.tag != "#text" && get(n.style, PropertyId::PointerEvents) != "none") {
                area(n, inverse, {box}, corners ? out.areas.add_clip(inverse, *corners, clip) : clip);
            }
            const auto overflow = get(n.style, PropertyId::Overflow);
            const bool clipping = overflow == "hidden" || overflow == "auto";
            if (clipping || n.tag == "svg") {
                clip = out.areas.add_clip(inverse, box, clip);
            }
            if (clipping && corners) {
                clip = out.areas.add_clip(inverse, *corners, clip);
            }
            if (n.space == constants::svg_namespace_uri) {
                const auto viewport = m * affine_plane(viewport_affine(n));
                for (const auto& c : n.children) {
                    svg_visit(svg_visit, c, viewport, n, clip);
                }
                return;
            }
            // Stacking order among siblings, as painted; auto stacks at 0.
            const auto z = [](const SceneNode& c) {
                const auto v = get(c.style, PropertyId::ZIndex, "0");
                return v == "auto" ? 0.f : len(v);
            };
            std::vector<const SceneNode*> order;
            for (const auto& c : n.children) {
                order.push_back(&c);
            }
            std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
                return z(*a) < z(*b);
            });
            for (auto c : order) {
                self(self, *c, m, clip);
            }
        };
        visit(visit, f.root, Plane{}, HitAreas::none);
        return out;
    }

    NodeIdentity SceneGeometry::hit_test(const SceneFrame& f, float x, float y) {
        return hit_map(f).hit(x, y);
    }

    std::vector<ElementGeometry> SceneGeometry::geometry(const SceneFrame& f, bool svg_descendants) {
        impl_->require_owner();
        std::vector<ElementGeometry> out;
        // A group may be queried by several ancestors and then by the flat walk.
        // Memoize local bounds only inside this solve; no scene pointer survives.
        std::unordered_map<const SceneNode*, Bounds> local_bounds;
        auto bounds = [&](auto&& self, const SceneNode& n) -> Bounds {
            if (auto it = local_bounds.find(&n); it != local_bounds.end()) {
                return it->second;
            }
            Bounds r;
            if (auto shape = impl_->shape(n)) {
                r = shape->bounds;
            }
            if (n.tag == "text") {
                const auto metrics = impl_->text.measure(n.text, n.style);
                float tx = n.attributes.contains("x") ? len(n.attributes.at("x")) : 0;
                float ty = n.attributes.contains("y") ? len(n.attributes.at("y")) : 0;
                if (n.attributes.contains("dy")) {
                    ty += len(n.attributes.at("dy"),
                              len(get(n.style, PropertyId::FontSize, constants::default_font_size)));
                }
                auto anchor = get(n.style, PropertyId::TextAnchor, "start");
                if (anchor == "middle") {
                    tx -= metrics.width / 2;
                }
                if (anchor == "end") {
                    tx -= metrics.width;
                }
                ty -= get(n.style, PropertyId::DominantBaseline) == "central" ? metrics.height / 2 : metrics.baseline;
                r = {tx, ty, tx + metrics.width, ty + metrics.height};
            }
            for (const auto& c : n.children) {
                if (c.tag != "defs" && c.tag != "clipPath") {
                    auto q = self(self, c);
                    auto t = svg_affine(c);
                    if (q.right >= q.left) {
                        for (auto p : std::array<Point, 4>{
                                 {{q.left, q.top}, {q.right, q.top}, {q.right, q.bottom}, {q.left, q.bottom}}}) {
                            auto v = t.point(p[0], p[1]);
                            r.add(v[0], v[1]);
                        }
                    }
                }
            }
            local_bounds.emplace(&n, r);
            return r;
        };
        auto append = [&](const SceneNode& n, Plane m, std::array<double, 4> local, NodeIdentity offset_parent) {
            Bounds b;
            for (auto p : std::array<Point, 4>{{{local[0], local[1]},
                                                {local[0] + local[2], local[1]},
                                                {local[0] + local[2], local[1] + local[3]},
                                                {local[0], local[1] + local[3]}}}) {
                auto q = m.point(p[0], p[1]);
                b.add(q[0], q[1]);
            }
            ElementGeometry e;
            e.identity = n.identity;
            e.rect = {b.left, b.top, b.right - b.left, b.bottom - b.top};
            e.client_rects = {e.rect};
            e.client_width = std::max(0.f, n.box[2] - len(get(n.style, PropertyId::BorderLeftWidth, "0")) -
                                               len(get(n.style, PropertyId::BorderRightWidth, "0")));
            e.client_height = std::max(0.f, n.box[3] - len(get(n.style, PropertyId::BorderTopWidth, "0")) -
                                                len(get(n.style, PropertyId::BorderBottomWidth, "0")));
            e.scroll_width = e.client_width;
            e.scroll_height = e.client_height;
            e.offset = {n.box[0], n.box[1], n.box[2], n.box[3]};
            e.offset_parent = offset_parent;
            e.matrix = {m.m[0], m.m[3], m.m[1], m.m[4], m.m[2], m.m[5]};
            out.push_back(e);
        };
        auto svg_visit = [&](auto&& self, const SceneNode& n, Plane parent) -> void {
            if (n.tag == "defs" || n.tag == "clipPath") {
                return;
            }
            auto m = parent * affine_plane(svg_affine(n));
            auto b = bounds(bounds, n);
            if (b.right >= b.left) {
                append(n, m, {b.left, b.top, b.right - b.left, b.bottom - b.top}, {});
            }
            for (const auto& c : n.children) {
                self(self, c, m);
            }
        };
        auto visit = [&](auto&& self, const SceneNode& n, Plane parent, NodeIdentity offset_parent, double ox,
                         double oy) -> void {
            const auto m = parent * scene_plane(n);
            if (n.tag != "#text") {
                append(n, m, {0, 0, n.box[2], n.box[3]}, offset_parent);
                out.back().offset[0] += ox;
                out.back().offset[1] += oy;
            }
            if (n.space == constants::svg_namespace_uri) {
                auto viewport = m * affine_plane(viewport_affine(n));
                if (svg_descendants) {
                    out.back().matrix = {viewport.m[0], viewport.m[3], viewport.m[1],
                                         viewport.m[4], viewport.m[2], viewport.m[5]};
                    for (const auto& c : n.children) {
                        svg_visit(svg_visit, c, viewport);
                    }
                }
                return;
            }
            const bool positioned = get(n.style, PropertyId::Position, "static") != "static" ||
                                    get(n.style, PropertyId::Transform, "none") != "none" ||
                                    n.identity == f.root.identity;
            for (const auto& c : n.children) {
                self(self, c, m, positioned ? n.identity : offset_parent, positioned ? 0 : ox + n.box[0],
                     positioned ? 0 : oy + n.box[1]);
            }
        };
        visit(visit, f.root, Plane{}, {}, 0, 0);
        return out;
    }
} // namespace weft
