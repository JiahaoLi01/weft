#include "scene/scene_geometry.hpp"
#include "scene/scene_path.hpp"
#include "scene/scene_values.hpp"

#include "base/css_scalar.hpp"
#include "base/diagnostic.hpp"
#include "base/owner_thread.hpp"
#include "constants/common.hpp"
#include "constants/scene.hpp"

#include <algorithm>
#include <array>
#include <charconv>
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
        using scene_values::view;

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

        // The attributes an SVG shape is built from, sorted.
        constexpr std::array<std::string_view, 15> geometry_attributes{
            "cx", "cy", "d", "height", "points", "r", "rx", "ry", "width", "x", "x1", "x2", "y", "y1", "y2"};

        // The index of a geometry attribute in geometry_attributes; its size for any other name.
        std::size_t geometry_attribute(std::string_view name) {
            // Paint, style and class names mostly start with other letters.
            switch (name.empty() ? '\0' : name[0]) {
                case 'c':
                case 'd':
                case 'h':
                case 'p':
                case 'r':
                case 'w':
                case 'x':
                case 'y':
                    break;
                default:
                    return geometry_attributes.size();
            }
            const auto it = std::lower_bound(geometry_attributes.begin(), geometry_attributes.end(), name);
            return it != geometry_attributes.end() && *it == name ? std::size_t(it - geometry_attributes.begin())
                                                                  : geometry_attributes.size();
        }

        // A node's shape with what it was built from. While those still match, the node reuses the shape without
        // building or hashing a key.
        struct NodeShape {
            std::string tag;
            std::string fill_rule;
            // Geometry attributes by index, in name order, with their values.
            std::vector<std::pair<std::size_t, std::string>> attributes;
            std::shared_ptr<const Shape> shape;
            std::size_t bytes{};

            bool built_from(const SceneNode& n, std::string_view rule) const {
                if (tag != n.tag || fill_rule != rule) {
                    return false;
                }
                auto it = attributes.begin();
                for (const auto& [name, value] : n.attributes) {
                    if (const auto index = geometry_attribute(name); index < geometry_attributes.size()) {
                        if (it == attributes.end() || it->first != index || it->second != value) {
                            return false;
                        }
                        ++it;
                    }
                }
                return it == attributes.end();
            }
        };

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

            void arc(const SceneArc& a, Point end) {
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
                        if (auto arc = scene_center_arc(p0, c.v[0], c.v[1], c.v[2], c.v[3] != 0, c.v[4] != 0, end)) {
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
                if (auto a = scene_center_arc(from, rx, ry, 0, false, true, to)) {
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
            if (auto a = scene_center_arc({cx + rx, cy}, rx, ry, 0, false, true, {cx - rx, cy})) {
                b.arc(*a, {cx - rx, cy});
            }
            if (auto a = scene_center_arc({cx - rx, cy}, rx, ry, 0, false, true, {cx + rx, cy})) {
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
        // Reused to build shape keys without allocating.
        std::string key_buffer;
        std::map<std::array<std::uint64_t, 3>, NodeShape> node_shapes;
        std::size_t node_shape_bytes{};
        // The last shape too large to keep per node, so shape() can return a reference.
        std::shared_ptr<const Shape> unkept;
        std::map<std::string, SceneAffine, std::less<>> transforms;
        std::size_t transform_bytes{};

        explicit Impl(SceneText& t) : text(t) {
        }

        void require_owner() const {
            owner.require(Diag::SceneRasterOwner);
        }

        // An SVG element's transform attribute, else its CSS transform, in user units; most elements have neither.
        SceneAffine affine(const SceneNode& n) {
            static const auto identity = scene_affine({}, 0, 0);
            const auto it = n.attributes.find("transform");
            const std::string_view source =
                it != n.attributes.end() ? std::string_view(it->second) : view(n.style, PropertyId::Transform);
            if (source.empty() || source == "none") {
                return identity;
            }
            if (const auto known = transforms.find(source); known != transforms.end()) {
                return known->second;
            }
            const auto result = scene_affine(parse_scene_transform(source, true).value(), 0, 0);
            if (source.size() <= constants::scene::transform_cache_max_bytes) {
                if (transforms.size() >= constants::scene::transform_cache_max_entries ||
                    transform_bytes + source.size() > constants::scene::transform_cache_max_bytes) {
                    transforms.clear();
                    transform_bytes = 0;
                }
                transform_bytes += source.size();
                transforms.emplace(source, result);
            }
            return result;
        }

        // Fill and stroke geometry of an SVG element in its user space; empty for groups, text and definitions. The
        // reference holds until the next call.
        const std::shared_ptr<const Shape>& shape(const SceneNode& n) {
            static const std::shared_ptr<const Shape> none;
            if (n.tag == "text" || n.tag == "g" || n.tag == "svg" || n.tag == "defs" || n.tag == "clipPath") {
                return none;
            }
            const auto fill_rule = view(n.style, PropertyId::FillRule);
            const std::array<std::uint64_t, 3> id{n.identity.view_epoch, n.identity.slot, n.identity.generation};
            const auto known = node_shapes.find(id);
            if (known != node_shapes.end() && known->second.built_from(n, fill_rule)) {
                return known->second.shape;
            }
            auto out = keyed_shape(n, fill_rule);
            NodeShape entry{n.tag, std::string(fill_rule), {}, out};
            entry.bytes = entry.tag.size() + entry.fill_rule.size();
            for (const auto& [name, value] : n.attributes) {
                if (const auto index = geometry_attribute(name); index < geometry_attributes.size()) {
                    entry.attributes.emplace_back(index, value);
                    entry.bytes += value.size();
                }
            }
            if (known != node_shapes.end()) {
                node_shape_bytes -= known->second.bytes;
                node_shapes.erase(known);
            }
            if (entry.bytes <= constants::scene::node_shape_cache_max_bytes) {
                if (node_shapes.size() >= constants::scene::node_shape_cache_max_entries ||
                    node_shape_bytes + entry.bytes > constants::scene::node_shape_cache_max_bytes) {
                    node_shapes.clear();
                    node_shape_bytes = 0;
                }
                node_shape_bytes += entry.bytes;
                return node_shapes.emplace(id, std::move(entry)).first->second.shape;
            }
            unkept = std::move(out);
            return unkept;
        }

        // The shape of an element with this tag, fill rule and geometry attributes, shared by every such element.
        std::shared_ptr<const Shape> keyed_shape(const SceneNode& n, std::string_view fill_rule) {
            const auto& a = n.attributes;
            // The key: tag, fill rule and the geometry attributes, each value with its length.
            auto& key = key_buffer;
            key.assign(n.tag);
            const auto add = [&](std::string_view v) {
                char digits[24];
                key.append(digits, std::to_chars(digits, digits + sizeof digits, v.size()).ptr);
                key += ':';
                key.append(v);
            };
            add(fill_rule);
            for (const auto& [name, value] : a) {
                if (geometry_attribute(name) < geometry_attributes.size()) {
                    add(name);
                    add(value);
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
            out->evenodd = fill_rule == "evenodd";
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
                shapes.emplace(key, out);
            }
            return out;
        }

        HitStroke stroke(const SceneNode& n) {
            HitStroke s;
            s.width = len(view(n.style, PropertyId::StrokeWidth, "1"));
            const auto cap = view(n.style, PropertyId::StrokeLinecap, "butt");
            s.cap = cap == "round"    ? HitStroke::Cap::Round
                    : cap == "square" ? HitStroke::Cap::Square
                                      : HitStroke::Cap::Butt;
            const auto d = view(n.style, PropertyId::StrokeDasharray, "none");
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
            s.offset = len(view(n.style, PropertyId::StrokeDashoffset, "0"));
            return s;
        }

        // clip-path="url(#id)" of one user-space rect, as in the reference raster.
        std::shared_ptr<const Shape> clip(const SceneNode& n, const SceneNode& root) {
            const auto attribute = n.attributes.find("clip-path");
            const std::string_view v =
                attribute == n.attributes.end() ? std::string_view{} : std::string_view(attribute->second);
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
            if (view(n.style, PropertyId::Display) == "none" || n.tag == "defs" || n.tag == "clipPath") {
                return;
            }
            const auto m = parent * affine_plane(impl_->affine(n));
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
            const auto pointer = view(n.style, PropertyId::PointerEvents, "visiblePainted");
            if (auto shape = impl_->shape(n); shape && pointer != "none") {
                const bool painted = pointer == "auto" || pointer == "painted" || pointer == "visiblePainted";
                const bool fill = pointer == "all" || pointer == "fill" || pointer == "visibleFill" ||
                                  pointer == "visible" ||
                                  (painted && view(n.style, PropertyId::Fill, constants::default_color) != "none");
                const bool stroke = pointer == "all" || pointer == "stroke" || pointer == "visibleStroke" ||
                                    pointer == "visible" ||
                                    (painted && view(n.style, PropertyId::Stroke, "none") != "none");
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
                const auto anchor = view(n.style, PropertyId::TextAnchor, "start");
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
            if (view(n.style, PropertyId::Display) == "none") {
                return;
            }
            const auto m = parent * (n.fragments.empty() ? scene_plane(n)
                                                         : affine_plane(SceneAffine::translation(n.box[0], n.box[1])));
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
            if (n.tag != "#text" && view(n.style, PropertyId::PointerEvents) != "none") {
                if (n.fragments.empty()) {
                    area(n, inverse, {box}, corners ? out.areas.add_clip(inverse, *corners, clip) : clip);
                } else {
                    // An inline element in lines: its fragments.
                    std::vector<HitShape> fragments;
                    for (const auto& f : n.fragments) {
                        fragments.push_back(HitShape::box(f.rect[0], f.rect[1], f.rect[2], f.rect[3]));
                    }
                    area(n, inverse, std::move(fragments), clip);
                }
            }
            const auto overflow = view(n.style, PropertyId::Overflow);
            const bool clipping = overflow == "hidden" || overflow == "auto";
            if (n.tag == "svg") {
                // An svg viewport clips at the border box, as the reference raster clips it.
                clip = out.areas.add_clip(inverse, box, clip);
                if (clipping && corners) {
                    clip = out.areas.add_clip(inverse, *corners, clip);
                }
            } else if (clipping) {
                // Content clips to the padding box with its inner corners, as painted.
                const auto padding = scene_values::padding_box(n.style, n.box[2], n.box[3]);
                clip = out.areas.add_clip(
                    inverse, HitShape::box(padding.rect[0], padding.rect[1], padding.rect[2], padding.rect[3]), clip);
                if (scene_values::is_rounded(padding)) {
                    HitShape inner;
                    inner.kind = HitShape::Kind::Rounded;
                    inner.rounded = padding;
                    clip = out.areas.add_clip(inverse, inner, clip);
                }
            }
            if (n.space == constants::svg_namespace_uri) {
                const auto viewport = m * affine_plane(viewport_affine(n));
                for (const auto& c : n.children) {
                    svg_visit(svg_visit, c, viewport, n, clip);
                }
                return;
            }
            // Stacking order among siblings, as painted; auto stacks at 0. A single child is not ordered, so its
            // z-index is not read.
            std::vector<std::pair<float, const SceneNode*>> order;
            order.reserve(n.children.size());
            for (const auto& c : n.children) {
                float z{};
                if (n.children.size() > 1) {
                    const auto v = view(c.style, PropertyId::ZIndex, "0");
                    z = v == "auto" ? 0.f : len(v);
                }
                order.emplace_back(z, &c);
            }
            std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
                return a.first < b.first;
            });
            for (const auto& entry : order) {
                self(self, *entry.second, m, clip);
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

        // The local bounds and transform of each element below one child of an svg element, in tree order. A group's
        // bounds take its children's, so a subtree is measured before it is listed.
        struct Local {
            Bounds bounds;
            SceneAffine affine;
        };

        std::vector<Local> locals;
        // Measures n's subtree into locals, n's slot first, and returns that slot. As a parent's bounds take them,
        // each child is measured before its transform is read.
        auto measure_subtree = [&](auto&& self, const SceneNode& n) -> std::size_t {
            const auto slot = locals.size();
            locals.emplace_back();
            Bounds r;
            if (const auto& shape = impl_->shape(n)) {
                r = shape->bounds;
            }
            if (n.tag == "text") {
                const auto metrics = impl_->text.measure(n.text, n.style);
                float tx = n.attributes.contains("x") ? len(n.attributes.at("x")) : 0;
                float ty = n.attributes.contains("y") ? len(n.attributes.at("y")) : 0;
                if (n.attributes.contains("dy")) {
                    ty += len(n.attributes.at("dy"),
                              len(view(n.style, PropertyId::FontSize, constants::default_font_size)));
                }
                const auto anchor = view(n.style, PropertyId::TextAnchor, "start");
                if (anchor == "middle") {
                    tx -= metrics.width / 2;
                }
                if (anchor == "end") {
                    tx -= metrics.width;
                }
                ty -= view(n.style, PropertyId::DominantBaseline) == "central" ? metrics.height / 2 : metrics.baseline;
                r = {tx, ty, tx + metrics.width, ty + metrics.height};
            }
            for (const auto& c : n.children) {
                if (c.tag != "defs" && c.tag != "clipPath") {
                    const auto child = self(self, c);
                    const auto t = impl_->affine(c);
                    locals[child].affine = t;
                    const auto q = locals[child].bounds;
                    if (q.right >= q.left) {
                        for (auto p : std::array<Point, 4>{
                                 {{q.left, q.top}, {q.right, q.top}, {q.right, q.bottom}, {q.left, q.bottom}}}) {
                            auto v = t.point(p[0], p[1]);
                            r.add(v[0], v[1]);
                        }
                    }
                }
            }
            locals[slot].bounds = r;
            return slot;
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
            auto& e = out.emplace_back();
            e.identity = n.identity;
            e.rect = {b.left, b.top, b.right - b.left, b.bottom - b.top};
            e.client_rects.assign(1, e.rect);
            // An inline element in lines: a client rect per fragment, and no client box.
            if (!n.fragments.empty()) {
                e.client_rects.clear();
                for (const auto& f : n.fragments) {
                    Bounds fragment;
                    for (auto p : std::array<Point, 4>{{{f.rect[0], f.rect[1]},
                                                        {f.rect[0] + f.rect[2], f.rect[1]},
                                                        {f.rect[0] + f.rect[2], f.rect[1] + f.rect[3]},
                                                        {f.rect[0], f.rect[1] + f.rect[3]}}}) {
                        auto q = m.point(p[0], p[1]);
                        fragment.add(q[0], q[1]);
                    }
                    e.client_rects.push_back(
                        {fragment.left, fragment.top, fragment.right - fragment.left, fragment.bottom - fragment.top});
                }
            }
            e.client_width = std::max(0.f, n.box[2] - len(view(n.style, PropertyId::BorderLeftWidth, "0")) -
                                               len(view(n.style, PropertyId::BorderRightWidth, "0")));
            e.client_height = std::max(0.f, n.box[3] - len(view(n.style, PropertyId::BorderTopWidth, "0")) -
                                                len(view(n.style, PropertyId::BorderBottomWidth, "0")));
            if (!n.fragments.empty()) {
                e.client_width = e.client_height = 0;
            }
            e.scroll_width = e.client_width;
            e.scroll_height = e.client_height;
            e.offset = {n.box[0], n.box[1], n.box[2], n.box[3]};
            e.offset_parent = offset_parent;
            e.matrix = {m.m[0], m.m[3], m.m[1], m.m[4], m.m[2], m.m[5]};
        };
        // Lists a measured subtree in tree order; next is the slot of n.
        auto svg_visit = [&](auto&& self, const SceneNode& n, Plane parent, std::size_t& next) -> void {
            if (n.tag == "defs" || n.tag == "clipPath") {
                return;
            }
            const auto local = locals[next++];
            const auto m = parent * affine_plane(local.affine);
            const auto& b = local.bounds;
            if (b.right >= b.left) {
                append(n, m, {b.left, b.top, b.right - b.left, b.bottom - b.top}, {});
            }
            for (const auto& c : n.children) {
                self(self, c, m, next);
            }
        };
        auto visit = [&](auto&& self, const SceneNode& n, Plane parent, NodeIdentity offset_parent, double ox,
                         double oy) -> void {
            const auto m = parent * (n.fragments.empty() ? scene_plane(n)
                                                         : affine_plane(SceneAffine::translation(n.box[0], n.box[1])));
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
                        if (c.tag == "defs" || c.tag == "clipPath") {
                            continue;
                        }
                        // The child's own transform is read before its subtree is measured.
                        const auto t = impl_->affine(c);
                        locals.clear();
                        measure_subtree(measure_subtree, c);
                        locals.front().affine = t;
                        std::size_t next{};
                        svg_visit(svg_visit, c, viewport, next);
                    }
                }
                return;
            }
            const bool positioned = view(n.style, PropertyId::Position, "static") != "static" ||
                                    view(n.style, PropertyId::Transform, "none") != "none" ||
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
