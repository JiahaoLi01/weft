#include "scene/scene_clip_path.hpp"

#include "base/css_scalar.hpp"
#include "paint/display_list.hpp"
#include "scene/scene_background.hpp"
#include "scene/scene_path.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace weft {
    namespace {
        using Point = std::array<float, 2>;

        [[noreturn]] void reject(const std::string& what) {
            throw std::invalid_argument("clip-path " + what + " outside scene profile");
        }

        // A reference box in the element's px, with its corner radii, top-left first and clockwise.
        struct Box {
            float x{};
            float y{};
            float w{};
            float h{};
            std::array<float, 4> rx{};
            std::array<float, 4> ry{};
        };

        // A length on an axis of `reference` px: px, a percentage of it, or calc() of the two.
        float length(std::string_view value, float reference) {
            const auto l = scene_length(value);
            if (!l) {
                reject("length " + std::string(value));
            }
            return l->px + l->share * reference;
        }

        // A margin as the margin box takes it: auto is 0; a percentage (of the containing block) is not known here.
        float margin(const StyleValues& style, PropertyId id) {
            const auto v = css::trim(style.text(id, "0"));
            if (v == "auto") {
                return 0;
            }
            const auto l = scene_length(v);
            if (!l || l->share != 0) {
                reject("margin-box with a margin of " + v);
            }
            return l->px;
        }

        // A corner radius grown by a margin as a box shadow's spread grows it (CSS Backgrounds 3 §7.1.1): by the whole
        // margin from a radius as large, by less from a smaller one; a square corner stays square.
        float spread(float radius, float by) {
            if (radius <= 0) {
                return 0;
            }
            if (by <= 0 || radius >= by) {
                return radius + by;
            }
            const float ratio = radius / by - 1;
            return radius + by * (1 + ratio * ratio * ratio);
        }

        Box reference(const StyleValues& style, std::string_view name, float w, float h) {
            const auto from = [&](BackgroundBox which) {
                const auto r = scene_background_box(style, which, w, h);
                return Box{r.rect[0], r.rect[1], r.rect[2], r.rect[3], r.radius_x, r.radius_y};
            };
            if (name.empty() || name == "border-box" || name == "stroke-box" || name == "view-box") {
                return from(BackgroundBox::Border);
            }
            if (name == "padding-box") {
                return from(BackgroundBox::Padding);
            }
            if (name == "content-box" || name == "fill-box") {
                return from(BackgroundBox::Content);
            }
            if (name == "margin-box") {
                auto box = from(BackgroundBox::Border);
                const float top = margin(style, PropertyId::MarginTop);
                const float right = margin(style, PropertyId::MarginRight);
                const float bottom = margin(style, PropertyId::MarginBottom);
                const float left = margin(style, PropertyId::MarginLeft);
                box.x -= left;
                box.y -= top;
                box.w += left + right;
                box.h += top + bottom;
                const std::array<float, 4> horizontal{left, right, right, left};
                const std::array<float, 4> vertical{top, top, bottom, bottom};
                for (unsigned i = 0; i < 4; ++i) {
                    box.rx[i] = spread(box.rx[i], horizontal[i]);
                    box.ry[i] = spread(box.ry[i], vertical[i]);
                }
                return box;
            }
            reject("box " + std::string(name));
        }

        bool box_keyword(std::string_view v) {
            for (const auto* k :
                 {"border-box", "padding-box", "content-box", "margin-box", "fill-box", "stroke-box", "view-box"}) {
                if (v == k) {
                    return true;
                }
            }
            return false;
        }

        std::vector<std::string> words(std::string_view s) {
            return css::split(s, ' ').parts;
        }

        // A rounded rectangle x, y, w, h with corner radii whose sums over a side shrink together to fit it (CSS
        // Backgrounds 3 §5.5); empty when it has no area.
        SceneClipPath rounded(float x, float y, float w, float h, std::array<float, 4> rx, std::array<float, 4> ry) {
            SceneClipPath out;
            if (!(w > 0) || !(h > 0)) {
                out.empty = true;
                return out;
            }
            float factor = 1;
            for (const auto& [side, a, b] : std::array<std::array<float, 3>, 4>{
                     {{w, rx[0], rx[1]}, {w, rx[3], rx[2]}, {h, ry[0], ry[3]}, {h, ry[1], ry[2]}}}) {
                if (a + b > side) {
                    factor = std::min(factor, side / (a + b));
                }
            }
            RoundedClip c;
            c.rect = {x, y, w, h};
            for (unsigned i = 0; i < 4; ++i) {
                c.radius_x[i] = std::max(0.f, rx[i] * factor);
                c.radius_y[i] = std::max(0.f, ry[i] * factor);
                if (c.radius_x[i] <= 0 || c.radius_y[i] <= 0) {
                    c.radius_x[i] = c.radius_y[i] = 0;
                }
            }
            out.rounded = c;
            out.bounds = c.rect;
            return out;
        }

        // Up to four values of a box's sides or corners, in CSS's order, as four.
        std::array<std::string, 4> four(const std::vector<std::string>& v, std::string_view what) {
            if (v.empty() || v.size() > 4) {
                reject(std::string(what));
            }
            switch (v.size()) {
                case 1:
                    return {v[0], v[0], v[0], v[0]};
                case 2:
                    return {v[0], v[1], v[0], v[1]};
                case 3:
                    return {v[0], v[1], v[2], v[1]};
                default:
                    return {v[0], v[1], v[2], v[3]};
            }
        }

        // A rectangle of the reference box inset by top, right, bottom and left px (a pair past the box shrinks to
        // meet), its corners border-radius values after `round` against the reference box's size.
        SceneClipPath inset_shape(const Box& box, float top, float right, float bottom, float left,
                                  const std::vector<std::string>& corners) {
            if (top + bottom > box.h && top + bottom > 0) {
                const float k = box.h / (top + bottom);
                top *= k;
                bottom *= k;
            }
            if (left + right > box.w && left + right > 0) {
                const float k = box.w / (left + right);
                left *= k;
                right *= k;
            }
            std::array<float, 4> rx{};
            std::array<float, 4> ry{};
            if (!corners.empty()) {
                std::vector<std::string> horizontal;
                std::vector<std::string> vertical;
                bool slash{};
                for (const auto& c : corners) {
                    if (c == "/") {
                        if (slash) {
                            reject("round " + c);
                        }
                        slash = true;
                    } else {
                        (slash ? vertical : horizontal).push_back(c);
                    }
                }
                const auto hx = four(horizontal, "round");
                const auto vy = four(slash ? vertical : horizontal, "round");
                for (unsigned i = 0; i < 4; ++i) {
                    rx[i] = length(hx[i], box.w);
                    ry[i] = length(vy[i], box.h);
                }
            }
            return rounded(box.x + left, box.y + top, box.w - left - right, box.h - top - bottom, rx, ry);
        }

        // The values of a shape before `round`, and the corner values after it.
        std::pair<std::vector<std::string>, std::vector<std::string>> split_round(std::string_view args) {
            std::string spaced;
            for (const char c : args) {
                spaced += c == '/' ? std::string(" / ") : std::string(1, c);
            }
            std::pair<std::vector<std::string>, std::vector<std::string>> out;
            bool round{};
            for (auto& w : words(spaced)) {
                if (w == "round") {
                    if (round) {
                        reject("round " + std::string(args));
                    }
                    round = true;
                } else {
                    (round ? out.second : out.first).push_back(std::move(w));
                }
            }
            if (round && out.second.empty()) {
                reject("round without corners");
            }
            return out;
        }

        // A circle's or an ellipse's radius on an axis: a length (a percentage of reference), or the distance from the
        // center to the box's nearer or farther side; the center at (c, its axis's start, length).
        float shape_radius(std::string_view v, float reference, std::array<float, 2> to_sides) {
            if (v == "closest-side") {
                return std::min(to_sides[0], to_sides[1]);
            }
            if (v == "farthest-side") {
                return std::max(to_sides[0], to_sides[1]);
            }
            const float r = length(v, reference);
            if (r < 0) {
                reject("radius " + std::string(v));
            }
            return r;
        }

        // A circle's or an ellipse's arguments: its radii (none, one or two words) and its center.
        struct Round {
            std::vector<std::string> radii;
            Point center{};
        };

        Round round_shape(std::string_view args, const Box& box) {
            Round out;
            const auto w = words(args);
            const auto at = std::find(w.begin(), w.end(), "at");
            out.radii.assign(w.begin(), at);
            out.center = {box.x + box.w / 2, box.y + box.h / 2};
            if (at != w.end()) {
                std::string position;
                for (auto it = at + 1; it != w.end(); ++it) {
                    position += (position.empty() ? "" : " ") + *it;
                }
                if (position.empty()) {
                    reject("at without a position");
                }
                const auto p = scene_position(position);
                out.center = {box.x + p[0].px + p[0].share * box.w, box.y + p[1].px + p[1].share * box.h};
            }
            return out;
        }

        // A path's subpaths as closed contours of lines, curves cut into lines no further than tolerance px off them
        // (Wang's bound), shifted by (dx, dy).
        std::vector<std::vector<Point>> flatten(const paint::Path& path, float tolerance, float dx, float dy) {
            std::vector<std::vector<Point>> contours;
            std::size_t at{};
            Point current{};
            for (const auto verb : path.verbs) {
                using Verb = paint::Path::Verb;
                if (verb == Verb::Move) {
                    current = path.points[at++];
                    contours.push_back({{current[0] + dx, current[1] + dy}});
                } else if (verb == Verb::Line) {
                    current = path.points[at++];
                    if (contours.empty()) {
                        contours.emplace_back();
                    }
                    contours.back().push_back({current[0] + dx, current[1] + dy});
                } else if (verb == Verb::Cubic) {
                    const auto p0 = current;
                    const auto c1 = path.points[at];
                    const auto c2 = path.points[at + 1];
                    const auto p3 = path.points[at + 2];
                    at += 3;
                    const auto second = [](Point a, Point b, Point c) {
                        return std::hypot(a[0] - 2 * b[0] + c[0], a[1] - 2 * b[1] + c[1]);
                    };
                    const float bend = std::max(second(p0, c1, c2), second(c1, c2, p3));
                    const int pieces =
                        std::clamp(int(std::ceil(std::sqrt(.75f * bend / std::max(tolerance, 1e-3f)))), 1, 64);
                    if (contours.empty()) {
                        contours.emplace_back();
                    }
                    for (int i = 1; i <= pieces; ++i) {
                        const float t = float(i) / float(pieces);
                        const float u = 1 - t;
                        const float a = u * u * u;
                        const float b = 3 * u * u * t;
                        const float c = 3 * u * t * t;
                        const float d = t * t * t;
                        contours.back().push_back({a * p0[0] + b * c1[0] + c * c2[0] + d * p3[0] + dx,
                                                   a * p0[1] + b * c1[1] + c * c2[1] + d * p3[1] + dy});
                    }
                    current = p3;
                } else if (!contours.empty()) {
                    // Close: the next command starts from the subpath's start.
                    current = {contours.back().front()[0] - dx, contours.back().front()[1] - dy};
                    contours.push_back({});
                    contours.back().push_back(contours[contours.size() - 2].front());
                }
            }
            return contours;
        }

        // Contours as a polygon clip's points: each of three points or more, closed, apart by a break.
        SceneClipPath polygon_shape(const std::vector<std::vector<Point>>& contours, bool evenodd) {
            auto points = std::make_shared<std::vector<Point>>();
            float left = std::numeric_limits<float>::infinity();
            float top = left;
            float right = -left;
            float bottom = -left;
            for (const auto& c : contours) {
                if (c.size() < 3) {
                    continue;
                }
                if (!points->empty()) {
                    points->push_back({clip_polygon_break, 0});
                }
                for (const auto& p : c) {
                    if (!std::isfinite(p[0]) || !std::isfinite(p[1])) {
                        reject("point off any scale");
                    }
                    points->push_back(p);
                    left = std::min(left, p[0]);
                    top = std::min(top, p[1]);
                    right = std::max(right, p[0]);
                    bottom = std::max(bottom, p[1]);
                }
                if (c.back() != c.front()) {
                    points->push_back(c.front());
                }
            }
            SceneClipPath out;
            if (points->empty() || !(right > left) || !(bottom > top)) {
                out.empty = true;
                return out;
            }
            out.polygon = std::move(points);
            out.evenodd = evenodd;
            out.bounds = {left, top, right - left, bottom - top};
            return out;
        }
    } // namespace

    std::string scene_clip_path_value(const StyleValues& style) {
        for (const auto id : {PropertyId::ClipPath, PropertyId::WebkitClipPath}) {
            auto v = css::trim(style.text(id, "none"));
            if (!v.empty() && v != "none") {
                return v;
            }
        }
        return "none";
    }

    std::optional<SceneClipPath> scene_clip_path(const StyleValues& style, float width, float height,
                                                 float tolerance) {
        const auto value = scene_clip_path_value(style);
        if (value == "none") {
            return std::nullopt;
        }
        if (value.starts_with("url(")) {
            reject(value + " (an SVG clipPath)");
        }
        std::string function;
        std::string box_name;
        for (const auto& w : words(value)) {
            if (box_keyword(w) && box_name.empty()) {
                box_name = w;
            } else if (w.find('(') != std::string::npos && w.ends_with(')') && function.empty()) {
                function = w;
            } else {
                reject(value);
            }
        }
        const auto box = reference(style, box_name, width, height);
        if (function.empty()) {
            // The reference box alone, with its corners.
            return rounded(box.x, box.y, box.w, box.h, box.rx, box.ry);
        }
        const auto open = function.find('(');
        const auto name = function.substr(0, open);
        const auto args = std::string_view(function).substr(open + 1, function.size() - open - 2);
        if (name == "inset") {
            const auto [values, corners] = split_round(args);
            const auto sides = four(values, value);
            return inset_shape(box, length(sides[0], box.h), length(sides[1], box.w), length(sides[2], box.h),
                               length(sides[3], box.w), corners);
        }
        if (name == "rect") {
            // Edges from the box's top and left; auto is the box's own edge.
            const auto [values, corners] = split_round(args);
            if (values.size() != 4) {
                reject(value);
            }
            const auto edge = [&](const std::string& v, float reference, float automatic) {
                return v == "auto" ? automatic : length(v, reference);
            };
            const float top = edge(values[0], box.h, 0);
            const float right = edge(values[1], box.w, box.w);
            const float bottom = edge(values[2], box.h, box.h);
            const float left = edge(values[3], box.w, 0);
            return inset_shape(box, top, box.w - std::max(right, left), box.h - std::max(bottom, top), left, corners);
        }
        if (name == "xywh") {
            const auto [values, corners] = split_round(args);
            if (values.size() != 4) {
                reject(value);
            }
            const float x = length(values[0], box.w);
            const float y = length(values[1], box.h);
            const float w = length(values[2], box.w);
            const float h = length(values[3], box.h);
            if (w < 0 || h < 0) {
                reject(value);
            }
            return inset_shape(box, y, box.w - x - w, box.h - y - h, x, corners);
        }
        if (name == "circle" || name == "ellipse") {
            const auto shape = round_shape(args, box);
            const auto [cx, cy] = shape.center;
            const std::array<float, 2> to_x{std::abs(cx - box.x), std::abs(box.x + box.w - cx)};
            const std::array<float, 2> to_y{std::abs(cy - box.y), std::abs(box.y + box.h - cy)};
            float rx{};
            float ry{};
            if (name == "circle") {
                if (shape.radii.size() > 1) {
                    reject(value);
                }
                const auto r = shape.radii.empty() ? std::string("closest-side") : shape.radii[0];
                if (r == "closest-side" || r == "farthest-side") {
                    const auto near = std::min(std::min(to_x[0], to_x[1]), std::min(to_y[0], to_y[1]));
                    const auto far = std::max(std::max(to_x[0], to_x[1]), std::max(to_y[0], to_y[1]));
                    rx = r == "closest-side" ? near : far;
                } else {
                    rx = shape_radius(r, std::hypot(box.w, box.h) / std::sqrt(2.f), {});
                }
                ry = rx;
            } else {
                if (shape.radii.size() == 1 || shape.radii.size() > 2) {
                    reject(value);
                }
                rx = shape_radius(shape.radii.empty() ? "closest-side" : shape.radii[0], box.w, to_x);
                ry = shape_radius(shape.radii.empty() ? "closest-side" : shape.radii[1], box.h, to_y);
            }
            return rounded(cx - rx, cy - ry, 2 * rx, 2 * ry, {rx, rx, rx, rx}, {ry, ry, ry, ry});
        }
        if (name == "polygon" || name == "path") {
            auto parts = css::split(args, ',').parts;
            bool evenodd{};
            if (!parts.empty() && (parts[0] == "evenodd" || parts[0] == "nonzero")) {
                evenodd = parts[0] == "evenodd";
                parts.erase(parts.begin());
            }
            std::vector<std::vector<Point>> contours;
            if (name == "polygon") {
                contours.emplace_back();
                for (const auto& p : parts) {
                    const auto xy = words(p);
                    if (xy.size() != 2) {
                        reject(value);
                    }
                    contours.back().push_back({box.x + length(xy[0], box.w), box.y + length(xy[1], box.h)});
                }
            } else {
                if (parts.size() != 1 || parts[0].size() < 2 || (parts[0].front() != '\'' && parts[0].front() != '"') ||
                    parts[0].back() != parts[0].front()) {
                    reject(value);
                }
                const auto data = std::string_view(parts[0]).substr(1, parts[0].size() - 2);
                // Coarser lines for a path past the points a clip may hold.
                for (float within = tolerance;; within *= 2) {
                    const auto path = scene_path_data(data, evenodd);
                    contours = path ? flatten(*path, within, box.x, box.y) : std::vector<std::vector<Point>>{};
                    std::size_t count{};
                    for (const auto& c : contours) {
                        count += c.size() + 2;
                    }
                    if (count <= max_clip_polygon_points) {
                        break;
                    }
                    if (within > tolerance * 64) {
                        reject("path of too many points");
                    }
                }
            }
            auto out = polygon_shape(contours, evenodd);
            if (out.polygon && out.polygon->size() > max_clip_polygon_points) {
                reject("polygon of more than " + std::to_string(max_clip_polygon_points) + " points");
            }
            return out;
        }
        reject(value);
    }
} // namespace weft
