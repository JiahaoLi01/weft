#include "scene/scene_gradient.hpp"

#include "base/css_scalar.hpp"
#include "constants/common.hpp"
#include "constants/scene.hpp"
#include "scene/scene_values.hpp"
#include "style/style.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

namespace weft {
    namespace {
        using paint::GradientKind;

        [[noreturn]] void reject(const std::string& what) {
            throw std::invalid_argument(what);
        }

        std::vector<std::string> split(std::string_view s, char c = ' ') {
            return css::split(s, c, {.quotes = false}).parts;
        }

        float number(std::string_view s) {
            const auto n =
                css::parse_number<float>(std::string(s), static_cast<float>(constants::scene::max_numeric_magnitude));
            if (!n) {
                reject("gradient number " + std::string(s));
            }
            return *n;
        }

        // A length in px (unitless numbers too), or a percentage of reference.
        float length(std::string_view s, float reference) {
            const auto [digits, unit] = css::split_unit(s, {.px = true, .percent = true});
            const float n = number(digits);
            return unit == css::Unit::Percent ? n * reference / constants::percent_scale : n;
        }

        bool is_length(const std::string& s) {
            return !s.empty() &&
                   (std::isdigit(static_cast<unsigned char>(s[0])) || s[0] == '.' || s[0] == '-' || s[0] == '+');
        }

        paint::Color color(std::string s, const StyleValues& style) {
            if (s == "currentcolor" || s == "currentColor") {
                s = std::string(style.text(PropertyId::Color, constants::default_color));
            }
            if (s == "transparent" || s == "none") {
                return {};
            }
            return parse_css_color(css::expand_color_shorthand(s));
        }

        // A one- or two-value position in a w x h tile, horizontal first: keywords (two in either order), lengths
        // and percentages.
        std::array<float, 2> position(std::vector<std::string> a, float w, float h) {
            const auto vertical = [](const std::string& k) {
                return k == "top" || k == "bottom";
            };
            const auto horizontal = [](const std::string& k) {
                return k == "left" || k == "right";
            };
            const auto keyword = [&](const std::string& k) {
                return vertical(k) || horizontal(k) || k == "center";
            };
            if (a.empty() || a.size() > 2) {
                reject("gradient position");
            }
            if (a.size() == 1) {
                a.push_back("center");
                if (vertical(a[0])) {
                    std::swap(a[0], a[1]);
                }
            } else if (vertical(a[0]) || horizontal(a[1])) {
                if (!keyword(a[0]) || !keyword(a[1]) || vertical(a[1]) || horizontal(a[0])) {
                    reject("gradient position");
                }
                std::swap(a[0], a[1]);
            }
            const auto offset = [&](const std::string& k, float size, const char* start, const char* end) {
                if (k == "center") {
                    return size / 2;
                }
                if (k == start) {
                    return 0.f;
                }
                if (k == end) {
                    return size;
                }
                if (keyword(k)) {
                    reject("gradient position " + k);
                }
                return length(k, size);
            };
            return {offset(a[0], w, "left", "right"), offset(a[1], h, "top", "bottom")};
        }

        // The ending shape of a radial gradient in a w x h tile from the tokens before its stops (CSS Images 3,
        // 3.2.1): center, then horizontal and vertical radii.
        std::array<float, 4> ending_shape(const std::vector<std::string>& tokens, float w, float h) {
            std::optional<bool> circle;
            std::string size;
            std::vector<std::string> lengths;
            std::vector<std::string> at;
            for (std::size_t i = 0; i < tokens.size(); ++i) {
                const auto& t = tokens[i];
                if (t == "at") {
                    at.assign(tokens.begin() + std::ptrdiff_t(i) + 1, tokens.end());
                    if (at.empty()) {
                        reject("radial gradient position");
                    }
                    break;
                }
                if (t == "circle" || t == "ellipse") {
                    if (circle) {
                        reject("radial gradient shape");
                    }
                    circle = t == "circle";
                } else if (t == "closest-side" || t == "farthest-side" || t == "closest-corner" ||
                           t == "farthest-corner") {
                    if (!size.empty() || !lengths.empty()) {
                        reject("radial gradient size");
                    }
                    size = t;
                } else if (is_length(t) && size.empty()) {
                    lengths.push_back(t);
                } else {
                    reject("radial gradient " + t);
                }
            }
            // One length sizes a circle; two, an ellipse.
            const bool round = circle ? *circle : lengths.size() == 1;
            if (lengths.size() > 2 || (round && lengths.size() == 2) || (!round && lengths.size() == 1)) {
                reject("radial gradient size");
            }
            const auto center = at.empty() ? std::array<float, 2>{w / 2, h / 2} : position(at, w, h);
            // The distances from the center to the nearest and farthest sides, on each axis.
            const float near_x = std::min(std::abs(center[0]), std::abs(w - center[0]));
            const float far_x = std::max(std::abs(center[0]), std::abs(w - center[0]));
            const float near_y = std::min(std::abs(center[1]), std::abs(h - center[1]));
            const float far_y = std::max(std::abs(center[1]), std::abs(h - center[1]));
            float rx{};
            float ry{};
            if (!lengths.empty()) {
                if (round && lengths[0].ends_with('%')) {
                    reject("radial gradient circle of a percentage");
                }
                rx = length(lengths[0], w);
                ry = round ? rx : length(lengths[1], h);
            } else if (size == "closest-side" || size == "farthest-side") {
                const bool closest = size == "closest-side";
                rx = closest ? near_x : far_x;
                ry = closest ? near_y : far_y;
                if (round) {
                    rx = ry = closest ? std::min(rx, ry) : std::max(rx, ry);
                }
            } else {
                // A corner: the circle through it, or the ellipse through it of the side size's aspect ratio, whose
                // radii are the side size's times sqrt 2.
                const bool closest = size == "closest-corner";
                const float dx = closest ? near_x : far_x;
                const float dy = closest ? near_y : far_y;
                if (round) {
                    rx = ry = std::hypot(dx, dy);
                } else {
                    rx = dx * std::numbers::sqrt2_v<float>;
                    ry = dy * std::numbers::sqrt2_v<float>;
                }
            }
            if (!(rx > 0) || !(ry > 0) || !std::isfinite(rx) || !std::isfinite(ry)) {
                reject("radial gradient of a zero or infinite radius");
            }
            return {center[0], center[1], rx, ry};
        }

        float gradient_t(const paint::Gradient& g, float x, float y) {
            const auto& tile = g.tile;
            float qx = x - tile[2];
            float qy = y - tile[3];
            qx -= std::floor(qx / tile[0]) * tile[0];
            qy -= std::floor(qy / tile[1]) * tile[1];
            const float angle = g.angle * std::numbers::pi_v<float> / constants::degrees_per_half_turn;
            if (g.kind == GradientKind::Linear) {
                const float dx = std::sin(angle);
                const float dy = -std::cos(angle);
                const float extent = tile[0] * std::abs(dx) + tile[1] * std::abs(dy);
                return ((qx - tile[0] / 2) * dx + (qy - tile[1] / 2) * dy) / extent + .5f;
            }
            if (g.kind == GradientKind::Radial) {
                return std::hypot((qx - g.shape[0]) / g.shape[2], (qy - g.shape[1]) / g.shape[3]);
            }
            const float turn = 2 * std::numbers::pi_v<float>;
            const float t = (std::atan2(qx - tile[0] / 2, -(qy - tile[1] / 2)) - angle) / turn;
            return t + 2 - std::floor(t + 2);
        }
    } // namespace

    std::optional<paint::Gradient> scene_gradient(const StyleValues& s, float width, float height) {
        const auto image = css::trim(s.text(PropertyId::BackgroundImage, "none"));
        if (image == "none" || image.starts_with("-weft-effect(") || image.starts_with("-weft-texture(") ||
            image.starts_with("url(")) {
            return std::nullopt;
        }
        const auto b = image.find('(');
        const auto e = image.rfind(')');
        if (b == image.npos || e == image.npos || e + 1 != image.size()) {
            reject("gradient grammar");
        }
        paint::Gradient g;
        auto name = std::string_view(image).substr(0, b);
        g.repeating = name.starts_with("repeating-");
        if (g.repeating) {
            name.remove_prefix(std::string_view("repeating-").size());
        }
        g.kind = name == "linear-gradient"                  ? GradientKind::Linear
                 : name == "radial-gradient"                ? GradientKind::Radial
                 : name == "conic-gradient" && !g.repeating ? GradientKind::Conic
                                                            : GradientKind::None;
        if (g.kind == GradientKind::None) {
            reject("background image " + std::string(image.substr(0, b)));
        }
        auto a = split(image.substr(b + 1, e - b - 1), ',');
        if (a.empty()) {
            reject("gradient args");
        }
        auto& tile = g.tile;
        tile = {width, height, 0, 0};
        if (const auto size = split(s.text(PropertyId::BackgroundSize, "100% 100%")); size.size() == 2) {
            tile[0] = length(size[0], width);
            tile[1] = length(size[1], height);
        }
        // An empty tile paints nothing.
        if (!(tile[0] > 0) || !(tile[1] > 0)) {
            return std::nullopt;
        }
        if (const auto at = split(s.text(PropertyId::BackgroundPosition, "0% 0%")); !at.empty()) {
            tile[2] = length(at[0], width - tile[0]);
            if (at.size() > 1) {
                tile[3] = length(at[1], height - tile[1]);
            }
        }
        g.angle =
            g.kind == GradientKind::Linear ? static_cast<float>(constants::scene::default_linear_gradient_angle) : 0.f;
        const auto first = split(a[0]);
        if (g.kind == GradientKind::Linear && a[0].ends_with("deg")) {
            g.angle = number(a[0].substr(0, a[0].size() - 3));
            a.erase(a.begin());
        } else if (g.kind == GradientKind::Linear && first.size() == 2 && first[0] == "to") {
            const auto& side = first[1];
            g.angle = side == "top"      ? 0.f
                      : side == "right"  ? 90.f
                      : side == "bottom" ? 180.f
                      : side == "left"   ? 270.f
                                         : (reject("linear gradient to " + side), 0.f);
            a.erase(a.begin());
        } else if (g.kind == GradientKind::Radial) {
            if (!first.empty() &&
                (first[0] == "circle" || first[0] == "ellipse" || first[0] == "at" || first[0].ends_with("-side") ||
                 first[0].ends_with("-corner") || is_length(first[0]))) {
                g.shape = ending_shape(first, tile[0], tile[1]);
                a.erase(a.begin());
            } else {
                g.shape = ending_shape({}, tile[0], tile[1]);
            }
        } else if (g.kind == GradientKind::Conic && a[0].starts_with("from ")) {
            const auto angle = css::trim(std::string_view(a[0]).substr(5));
            if (!angle.ends_with("deg")) {
                reject("conic gradient angle " + angle);
            }
            g.angle = number(std::string_view(angle).substr(0, angle.size() - 3));
            a.erase(a.begin());
        }
        // Length stops are fractions of the gradient line: a linear gradient's extent through the tile at its angle,
        // a radial gradient's horizontal radius. A conic gradient takes none.
        const float radians = g.angle * std::numbers::pi_v<float> / constants::degrees_per_half_turn;
        const float line = g.kind == GradientKind::Radial ? g.shape[2]
                           : g.kind == GradientKind::Linear
                               ? std::abs(tile[0] * std::sin(radians)) + std::abs(tile[1] * std::cos(radians))
                               : 0.f;
        std::vector<bool> specified;
        for (const auto& text : a) {
            const auto v = split(text);
            if (v.empty() || v.size() > 3) {
                reject("gradient stop " + text);
            }
            const auto c = color(v[0], s);
            if (v.size() == 1) {
                g.stops.push_back({0, c});
                specified.push_back(false);
                continue;
            }
            for (std::size_t i = 1; i < v.size(); ++i) {
                float offset = length(v[i], 1);
                if (!v[i].ends_with('%')) {
                    if (line <= 0) {
                        reject("conic gradient length stop");
                    }
                    offset /= line;
                }
                g.stops.push_back({offset, c});
                specified.push_back(true);
            }
        }
        if (g.stops.size() < 2 || g.stops.size() > constants::scene::max_gradient_stops) {
            reject("gradient stop bound");
        }
        if (!specified[0]) {
            g.stops[0].offset = 0;
        }
        if (!specified.back()) {
            g.stops.back().offset = 1;
        }
        specified[0] = specified.back() = true;
        float last = g.stops[0].offset;
        for (std::size_t i = 0; i < g.stops.size(); ++i) {
            if (specified[i]) {
                g.stops[i].offset = std::max(last, g.stops[i].offset);
                last = g.stops[i].offset;
            }
        }
        for (std::size_t i = 1; i < g.stops.size() - 1;) {
            if (specified[i]) {
                ++i;
                continue;
            }
            auto j = i;
            while (!specified[j]) {
                ++j;
            }
            for (auto k = i; k < j; ++k) {
                g.stops[k].offset = g.stops[i - 1].offset +
                                    (g.stops[j].offset - g.stops[i - 1].offset) * float(k - i + 1) / float(j - i + 1);
            }
            i = j;
        }
        // Stops that span nothing repeat into one color, as Blink draws them: the last stop's.
        if (g.repeating && !(g.stops.back().offset > g.stops.front().offset)) {
            g.stops = {{0, g.stops.back().color}, {1, g.stops.back().color}};
            g.repeating = false;
        }
        return g;
    }

    paint::Color scene_gradient_color(const paint::Gradient& g, float x, float y) {
        float t = gradient_t(g, x, y);
        const auto& stops = g.stops;
        if (g.repeating) {
            const float first = stops.front().offset;
            const float span = stops.back().offset - first;
            t = first + (t - first) - std::floor((t - first) / span) * span;
        }
        if (t <= stops.front().offset) {
            return stops.front().color;
        }
        if (t >= stops.back().offset) {
            return stops.back().color;
        }
        std::size_t i = 1;
        while (i < stops.size() && stops[i].offset <= t) {
            ++i;
        }
        const auto& from = stops[i - 1];
        const auto& to = stops[i];
        // from.offset <= t < to.offset.
        const float f = std::clamp((t - from.offset) / (to.offset - from.offset), 0.f, 1.f);
        const float alpha = from.color[3] + (to.color[3] - from.color[3]) * f;
        paint::Color out{0, 0, 0, alpha};
        if (alpha > 0) {
            for (unsigned c = 0; c < 3; ++c) {
                out[c] = ((1 - f) * from.color[c] * from.color[3] + f * to.color[c] * to.color[3]) / alpha;
            }
        }
        return out;
    }
} // namespace weft
