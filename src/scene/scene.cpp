#include "scene/scene.hpp"
#include "base/css_scalar.hpp"
#include "base/owner_thread.hpp"
#include "constants/scene.hpp"
#include "layout/flex.hpp"
#include "scene/scene_inline.hpp"
#include "text/case_mapping.hpp"
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <functional>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <charconv>
#include <deque>
#include <set>
#include <thread>
#include <type_traits>

namespace weft {
    namespace {
        using Style = SceneStyle;

        using css::trim;

        [[noreturn]] void fail(const std::string& s) {
            throw_diagnostic(Diag::SceneProfile, s);
        }

        std::string get(const Style& s, const std::string& k, const char* fallback = "") {
            return css::style_value(s, k, fallback);
        }

        std::string get(const Style& s, PropertyId k, const char* fallback = "") {
            return std::string(s.text(k, fallback));
        }

        // Edge and corner longhands in top, right, bottom, left (corners: top-left first, clockwise) order.
        using Edges = std::array<PropertyId, 4>;
        constexpr Edges border_width_edges{PropertyId::BorderTopWidth, PropertyId::BorderRightWidth,
                                           PropertyId::BorderBottomWidth, PropertyId::BorderLeftWidth};
        constexpr Edges border_color_edges{PropertyId::BorderTopColor, PropertyId::BorderRightColor,
                                           PropertyId::BorderBottomColor, PropertyId::BorderLeftColor};
        constexpr Edges border_style_edges{PropertyId::BorderTopStyle, PropertyId::BorderRightStyle,
                                           PropertyId::BorderBottomStyle, PropertyId::BorderLeftStyle};
        constexpr Edges radius_corners{PropertyId::BorderTopLeftRadius, PropertyId::BorderTopRightRadius,
                                       PropertyId::BorderBottomRightRadius, PropertyId::BorderBottomLeftRadius};

        // Properties a scene element takes from its parent's expanded style unless it specifies them.
        constexpr PropertyId inherited_properties[]{PropertyId::FontSize,
                                                    PropertyId::FontWeight,
                                                    PropertyId::FontFamily,
                                                    PropertyId::LineHeight,
                                                    PropertyId::LetterSpacing,
                                                    PropertyId::Color,
                                                    PropertyId::Fill,
                                                    PropertyId::FillOpacity,
                                                    PropertyId::FillRule,
                                                    PropertyId::Stroke,
                                                    PropertyId::StrokeWidth,
                                                    PropertyId::StrokeOpacity,
                                                    PropertyId::StrokeLinecap,
                                                    PropertyId::StrokeLinejoin,
                                                    PropertyId::StrokeDasharray,
                                                    PropertyId::StrokeDashoffset,
                                                    PropertyId::TextAnchor,
                                                    PropertyId::DominantBaseline,
                                                    PropertyId::PointerEvents,
                                                    PropertyId::TextAlign,
                                                    PropertyId::WhiteSpace,
                                                    PropertyId::FontStyle,
                                                    PropertyId::FontVariantNumeric,
                                                    PropertyId::TextTransform};

        bool specified(const ComputedStyle& c, PropertyId k) {
            return std::find(c.specified_properties.begin(), c.specified_properties.end(), property_name(k)) !=
                   c.specified_properties.end();
        }

        // Element attributes stay a string map.
        std::string get(const AttributeMap& s, const std::string& k, const char* fallback = "") {
            return css::style_value(s, k, fallback);
        }

        double number(std::string_view s) {
            const std::string text(s);
            const auto n = css::parse_number<double>(text, constants::scene::max_numeric_magnitude);
            if (!n) {
                fail("finite numeric value " + text);
            }
            return *n;
        }

        // Unitless numbers are accepted as px.
        double length(std::string_view s, double reference = 1) {
            const auto value = trim(s);
            const auto [digits, unit] = css::split_unit(value);
            const double n = number(digits);
            return unit == css::Unit::Percent ? n * reference / constants::percent_scale : n;
        }

        std::string numeric(double n) {
            char buffer[64];
            auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), n, std::chars_format::general,
                                              constants::scene::numeric_output_precision);
            if (error != std::errc{}) {
                fail("animation numeric output");
            }
            return {buffer, end};
        }

        std::vector<std::string> split(std::string_view s, char delimiter = ' ') {
            return css::split(s, delimiter).parts;
        }

        Style declarations(std::string_view s) {
            Style out;
            for (auto part : split(s, ';')) {
                auto p = part.find(':');
                if (p == part.npos) {
                    fail("declaration colon");
                }
                const auto name = trim(part.substr(0, p));
                const auto id = find_property(name);
                if (!id) {
                    fail("unsupported keyframe property " + name);
                }
                out.set(*id, trim(part.substr(p + 1)));
            }
            return out;
        }

        bool ident_char(char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '#' || c == '.';
        }

        // Viewport-percentage lengths in a value (vw, vh, vmin, vmax and their small, large and dynamic forms: a scene
        // has one viewport) as px of `viewport`; none when the value has none.
        std::optional<std::string> viewport_lengths(std::string_view value, std::array<float, 2> viewport) {
            if (value.find('v') == value.npos) {
                return std::nullopt;
            }
            std::string out;
            bool found{};
            std::size_t copied{};
            const auto digit = [&](std::size_t k) {
                return k < value.size() && std::isdigit(static_cast<unsigned char>(value[k]));
            };
            // A number starts at a digit, or a point before one, optionally signed, and not inside an identifier.
            const auto number_at = [&](std::size_t k) {
                return digit(k) || (k < value.size() && value[k] == '.' && digit(k + 1));
            };
            for (std::size_t i = 0; i < value.size();) {
                const std::size_t start = i;
                std::size_t at = start;
                if ((value[at] == '+' || value[at] == '-') && number_at(at + 1)) {
                    ++at;
                }
                if (!number_at(at) || (start > 0 && ident_char(value[start - 1]))) {
                    i = start + 1;
                    continue;
                }
                i = at;
                while (i < value.size() && (digit(i) || value[i] == '.')) {
                    ++i;
                }
                std::size_t unit = i;
                while (i < value.size() && std::isalpha(static_cast<unsigned char>(value[i]))) {
                    ++i;
                }
                if (i < value.size() && ident_char(value[i]) && value[i] != '.') {
                    continue;
                }
                auto name = std::string(value.substr(unit, i - unit));
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
                    return char(std::tolower(c));
                });
                if (name.size() > 2 && (name[0] == 's' || name[0] == 'l' || name[0] == 'd') && name[1] == 'v') {
                    name.erase(0, 1);
                }
                double basis{};
                if (name == "vw") {
                    basis = viewport[0];
                } else if (name == "vh") {
                    basis = viewport[1];
                } else if (name == "vmin") {
                    basis = std::min(viewport[0], viewport[1]);
                } else if (name == "vmax") {
                    basis = std::max(viewport[0], viewport[1]);
                } else {
                    continue;
                }
                found = true;
                out.append(value.substr(copied, start - copied));
                out += numeric(number(value.substr(start, unit - start)) * basis / constants::percent_scale) + "px";
                copied = i;
            }
            if (!found) {
                return std::nullopt;
            }
            out.append(value.substr(copied));
            return out;
        }

        // Text as text-transform shows it; the transforms beyond upper and lower case are outside the profile.
        std::string transformed(std::string text, const SceneStyle& style) {
            const auto transform = style.text(PropertyId::TextTransform, "none");
            if (transform == "none" || text.empty()) {
                return text;
            }
            if (transform == "uppercase" || transform == "lowercase") {
                return case_mapped(text, transform == "uppercase" ? TextCase::Upper : TextCase::Lower);
            }
            fail("text-transform " + std::string(transform) + " outside scene profile");
        }

        // A scene element's computed values from its cascaded ones: shorthands and aliases expanded, viewport
        // lengths in px of `viewport`. `viewport_units` is set when some value used one.
        Style expand(Style s, std::array<float, 2> viewport, bool* viewport_units = nullptr) {
            if (s.contains(PropertyId::Rotate)) {
                s[PropertyId::Transform] =
                    "rotate(" + s.at(PropertyId::Rotate) + ") " +
                    (get(s, PropertyId::Transform) == "none" ? std::string{} : get(s, PropertyId::Transform));
                s.erase(PropertyId::Rotate);
            }
            if (s.contains(PropertyId::Background)) {
                const auto v = s.at(PropertyId::Background);
                s[PropertyId::BackgroundImage] =
                    v.find("gradient(") != v.npos || v.find("-weft-effect(") != v.npos ? v : "none";
                s[PropertyId::BackgroundColor] = s[PropertyId::BackgroundImage] == "none" ? v : "transparent";
                s.erase(PropertyId::Background);
            }
            if (s.contains(PropertyId::BorderColor)) {
                auto v = split(s.at(PropertyId::BorderColor));
                if (v.empty() || v.size() > 4) {
                    fail("border color shorthand");
                }
                const unsigned indices[4][4] = {{0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
                unsigned i{};
                for (const auto k : border_color_edges) {
                    if (!s.contains(k)) {
                        s[k] = v[indices[v.size() - 1][i]];
                    }
                    ++i;
                }
            }
            if (s.contains(PropertyId::BorderRadius)) {
                auto v = split(s.at(PropertyId::BorderRadius));
                if (v.size() != 1) {
                    fail("scene radius requires one circular value");
                }
                for (const auto k : radius_corners) {
                    if (!s.contains(k)) {
                        s[k] = v[0];
                    }
                }
            }
            // A side whose style is none or hidden computes a zero width.
            for (std::size_t side = 0; side < border_style_edges.size(); ++side) {
                const auto style = s.text(border_style_edges[side], "solid");
                if (style == "none" || style == "hidden") {
                    s[border_width_edges[side]] = "0";
                }
            }
            // The scene clips both axes or neither: one axis that clips makes the box clip. As in CSS, visible beside
            // an axis that clips is auto, so auto wins (its content must fit); scroll and clip stay for the
            // diagnostic.
            if (s.contains(PropertyId::OverflowX) || s.contains(PropertyId::OverflowY)) {
                const auto both = std::string(s.text(PropertyId::Overflow, "visible"));
                const auto x = std::string(s.text(PropertyId::OverflowX, both));
                const auto y = std::string(s.text(PropertyId::OverflowY, both));
                s[PropertyId::Overflow] = x == y                                           ? x
                                          : x != "visible" && x != "auto" && x != "hidden" ? x
                                          : y != "visible" && y != "auto" && y != "hidden" ? y
                                          : x == "hidden" && y == "hidden"                 ? "hidden"
                                                                                           : "auto";
                s.erase(PropertyId::OverflowX);
                s.erase(PropertyId::OverflowY);
            }
            std::vector<std::pair<PropertyId, std::string>> resolved;
            for (const auto& [id, value] : s) {
                if (auto px = viewport_lengths(value.text(), viewport)) {
                    resolved.emplace_back(id, std::move(*px));
                }
            }
            for (auto& [id, px] : resolved) {
                s.set(id, std::move(px));
            }
            if (viewport_units && !resolved.empty()) {
                *viewport_units = true;
            }
            return s;
        }

        std::array<double, 4> color(std::string_view s) {
            auto c = parse_css_color(css::expand_color_shorthand(trim(s)));
            return {c[0], c[1], c[2], c[3]};
        }

        struct ValueTokens {
            std::vector<double> values;
            std::vector<std::string> pieces;
        };

        ValueTokens value_tokens(const std::string& s) {
            ValueTokens out;
            std::size_t p{};
            for (const auto& span : css::number_spans(s)) {
                out.pieces.push_back(s.substr(p, span.offset - p));
                out.values.push_back(number(std::string_view(s).substr(span.offset, span.length)));
                p = span.offset + span.length;
            }
            out.pieces.push_back(s.substr(p));
            return out;
        }

        struct InterpolationCache {
            std::map<std::string, ValueTokens> tokens;
            std::map<std::string, std::array<double, 4>> colors;
            std::size_t bytes{};

            void reserve(std::size_t size) {
                if (tokens.size() + colors.size() >= constants::scene::interpolation_cache_max_entries ||
                    bytes + size > constants::scene::interpolation_cache_max_bytes) {
                    tokens.clear();
                    colors.clear();
                    bytes = 0;
                }
                bytes += size;
            }

            ValueTokens scan(const std::string& s) {
                if (auto i = tokens.find(s); i != tokens.end()) {
                    return i->second;
                }
                auto v = value_tokens(s);
                std::size_t size = s.size() + v.values.size() * sizeof(double);
                for (const auto& piece : v.pieces) {
                    size += piece.size();
                }
                if (size <= constants::scene::interpolation_cache_max_bytes) {
                    reserve(size);
                    tokens.emplace(s, v);
                }
                return v;
            }

            std::array<double, 4> parse_color(const std::string& s) {
                if (auto i = colors.find(s); i != colors.end()) {
                    return i->second;
                }
                auto value = color(s);
                if (s.size() + sizeof(value) <= constants::scene::interpolation_cache_max_bytes) {
                    reserve(s.size() + sizeof(value));
                    colors.emplace(s, value);
                }
                return value;
            }
        };

        std::string lerp_value(const std::string& a, const std::string& b, double t,
                               InterpolationCache* cache = nullptr) {
            if (a == b) {
                return a;
            }
            auto is_color = [](const std::string& v) {
                return v.starts_with('#') || v.starts_with("rgb(") || v.starts_with("rgba(") || v == "transparent" ||
                       v == "black" || v == "white";
            };
            if (is_color(a) && is_color(b)) {
                try {
                    auto x = cache ? cache->parse_color(a) : color(a);
                    auto y = cache ? cache->parse_color(b) : color(b);
                    const double alpha = x[3] + (y[3] - x[3]) * t;
                    std::string v = "rgba(";
                    for (unsigned i = 0; i < 3; ++i) {
                        const double n = alpha ? ((1 - t) * x[i] * x[3] + t * y[i] * y[3]) / alpha : 0;
                        v += numeric(n * constants::color_channel_max) + ", ";
                    }
                    return v + numeric(alpha) + ")";
                } catch (const std::exception&) {
                }
            }
            // Interpolate equivalent numeric-token sequences (lengths, percentages,
            // transform lists). Otherwise CSS discrete interpolation is explicit.
            auto left = cache ? cache->scan(a) : value_tokens(a);
            auto right = cache ? cache->scan(b) : value_tokens(b);
            auto& av = left.values;
            auto& bv = right.values;
            auto& ap = left.pieces;
            auto& bp = right.pieces;
            if (av.size() == bv.size()) {
                for (std::size_t i = 0; i < av.size(); ++i) {
                    for (const auto* unit : {"px", "%", "deg"}) {
                        const auto u = std::string(unit);
                        if (av[i] == 0 && bp[i + 1].starts_with(u) && bp[i + 1].substr(u.size()) == ap[i + 1]) {
                            ap[i + 1] = bp[i + 1];
                        }
                        if (bv[i] == 0 && ap[i + 1].starts_with(u) && ap[i + 1].substr(u.size()) == bp[i + 1]) {
                            bp[i + 1] = ap[i + 1];
                        }
                    }
                }
            }
            if (av.size() == 1 && bv.size() == 1 &&
                ((ap.back() == "px" && bp.back() == "%") || (ap.back() == "%" && bp.back() == "px"))) {
                return "calc(" + numeric(av[0] * (1 - t)) + ap.back() + " + " + numeric(bv[0] * t) + bp.back() + ")";
            }
            if (av.size() == bv.size() && ap == bp && !av.empty()) {
                std::string v;
                for (std::size_t i = 0; i < av.size(); ++i) {
                    v += ap[i] + numeric(av[i] + (bv[i] - av[i]) * t);
                }
                return v + ap.back();
            }
            if (a == "none" && b.starts_with("scale(")) {
                return lerp_value("scale(1)", b, t, cache);
            }
            if (b == "none" && a.starts_with("scale(")) {
                return lerp_value(a, "scale(1)", t, cache);
            }
            if (a == "none" && b.starts_with("rotate(")) {
                return lerp_value("rotate(0deg)", b, t, cache);
            }
            if (b == "none" && a.starts_with("rotate(")) {
                return lerp_value(a, "rotate(0deg)", t, cache);
            }
            return t < constants::scene::discrete_interpolation_midpoint ? a : b;
        }

        // Endpoint decoding is bounded and follows the reference interpolation grammar.
        // Incompatible lists/units remain discrete; typed paint cannot broaden the profile.
        struct PaintPair {
            std::optional<PaintValue> a;
            std::optional<PaintValue> b;
            bool linear{};
        };

        struct PaintCache {
            std::map<std::pair<std::string, std::string>, PaintPair> pairs;
            std::size_t bytes{};

            std::optional<PaintValue> sample(std::string a, std::string b, double t) {
                if (a == "none" && b.starts_with("scale(")) {
                    a = "scale(1)";
                }
                if (b == "none" && a.starts_with("scale(")) {
                    b = "scale(1)";
                }
                if (a == "none" && b.starts_with("rotate(")) {
                    a = "rotate(0deg)";
                }
                if (b == "none" && a.starts_with("rotate(")) {
                    b = "rotate(0deg)";
                }
                auto key = std::pair{a, b};
                auto it = pairs.find(key);
                if (it == pairs.end()) {
                    PaintPair pair{parse_scene_paint_value(a), parse_scene_paint_value(b), a == b};
                    auto is_color = [](const std::string& v) {
                        return v.starts_with('#') || v.starts_with("rgb(") || v.starts_with("rgba(") ||
                               v == "transparent" || v == "black" || v == "white";
                    };
                    if (pair.a && pair.b && is_color(a) && is_color(b)) {
                        pair.linear = true;
                    } else if (pair.a && pair.b) {
                        auto x = value_tokens(a);
                        auto y = value_tokens(b);
                        if (x.values.size() == y.values.size()) {
                            for (std::size_t i = 0; i < x.values.size(); ++i) {
                                for (const auto* u : {"px", "%", "deg"}) {
                                    const auto unit = std::string(u);
                                    if (x.values[i] == 0 && y.pieces[i + 1].starts_with(unit) &&
                                        y.pieces[i + 1].substr(unit.size()) == x.pieces[i + 1]) {
                                        x.pieces[i + 1] = y.pieces[i + 1];
                                    }
                                    if (y.values[i] == 0 && x.pieces[i + 1].starts_with(unit) &&
                                        x.pieces[i + 1].substr(unit.size()) == y.pieces[i + 1]) {
                                        y.pieces[i + 1] = x.pieces[i + 1];
                                    }
                                }
                            }
                        }
                        pair.linear = !x.values.empty() && x.values.size() == y.values.size() && x.pieces == y.pieces;
                        // The string reference treats HSL channels as numeric tokens,
                        // not premultiplied RGBA. Keep those values on its legacy path.
                        if (a.starts_with("hsl") || b.starts_with("hsl")) {
                            return {};
                        }
                    }
                    const auto size = a.size() + b.size() + sizeof(PaintPair) +
                                      (pair.a ? pair.a->operations.size() * sizeof(PaintOperation) : 0) +
                                      (pair.b ? pair.b->operations.size() * sizeof(PaintOperation) : 0);
                    if (size > constants::scene::paint_pair_cache_max_bytes) {
                        return {};
                    }
                    if (pairs.size() >= constants::scene::paint_pair_cache_max_entries ||
                        bytes + size > constants::scene::paint_pair_cache_max_bytes) {
                        pairs.clear();
                        bytes = 0;
                    }
                    bytes += size;
                    it = pairs.emplace(std::move(key), std::move(pair)).first;
                }
                const auto& pair = it->second;
                if (pair.linear && pair.a && pair.b) {
                    return interpolate_scene_paint_value(*pair.a, *pair.b, t);
                }
                return t < constants::scene::discrete_interpolation_midpoint ? pair.a : pair.b;
            }
        };

        double duration(std::string v) {
            if (v.ends_with("ms")) {
                return number(v.substr(0, v.size() - 2));
            }
            if (v.ends_with('s')) {
                return number(v.substr(0, v.size() - 1)) * constants::milliseconds_per_second;
            }
            fail("time unit " + v);
        }

        double easing(std::string f, double x) {
            x = std::clamp(x, 0., 1.);
            if (f == "linear") {
                return x;
            }
            if (f == "step-end") {
                return x >= 1 ? 1 : 0;
            }
            if (f.starts_with("steps(")) {
                auto a = split(f.substr(6, f.size() - 7), ',');
                auto n = number(a[0]);
                if (n < 1 || n != std::floor(n) || a.size() > 2 ||
                    (a.size() == 2 && a[1] != "end" && a[1] != "jump-end")) {
                    fail("scene steps requires integer count and end position");
                }
                return std::floor(x * n) / n;
            }
            double x1 = constants::scene::ease_x1;
            double y1 = constants::scene::ease_y1;
            double x2 = constants::scene::ease_x2;
            double y2 = 1;
            if (f == "ease-in-out") {
                x1 = constants::scene::ease_in_x1;
                y1 = 0;
                x2 = constants::scene::ease_out_x2;
                y2 = 1;
            } else if (f == "ease-out") {
                x1 = 0;
                y1 = 0;
                x2 = constants::scene::ease_out_x2;
                y2 = 1;
            } else if (f == "ease-in") {
                x1 = constants::scene::ease_in_x1;
                y1 = 0;
                x2 = 1;
                y2 = 1;
            } else if (f.starts_with("cubic-bezier(")) {
                auto v = split(f.substr(13, f.size() - 14), ',');
                if (v.size() != 4) {
                    fail("bezier grammar");
                }
                x1 = number(v[0]);
                y1 = number(v[1]);
                x2 = number(v[2]);
                y2 = number(v[3]);
            } else if (f != "ease") {
                fail("timing function " + f);
            }
            if (x1 < 0 || x1 > 1 || x2 < 0 || x2 > 1) {
                fail("bezier x control point range");
            }
            auto curve = [](double t, double a, double b) {
                return 3 * (1 - t) * (1 - t) * t * a + 3 * (1 - t) * t * t * b + t * t * t;
            };
            double lo = 0;
            double hi = 1;
            for (unsigned i = 0; i < constants::scene::bezier_bisection_steps; ++i) {
                auto mid = (lo + hi) / 2;
                if (curve(mid, x1, x2) < x) {
                    lo = mid;
                } else {
                    hi = mid;
                }
            }
            return curve((lo + hi) / 2, y1, y2);
        }

        struct Animation {
            std::string name;
            std::string timing = "ease";
            std::string fill = "none";
            std::string direction = "normal";
            double duration{};
            double delay{};
            double count = 1;
        };

        Animation animation(std::string value) {
            Animation a;
            bool time{};
            for (auto word : split(value)) {
                if (word == "infinite") {
                    a.count = INFINITY;
                } else if (word == "forwards" || word == "backwards" || word == "both") {
                    a.fill = word;
                } else if (word == "alternate" || word == "reverse" || word == "alternate-reverse") {
                    a.direction = word;
                } else if (word == "linear" || word == "ease" || word.starts_with("ease-") ||
                           word.starts_with("steps(") || word.starts_with("cubic-bezier(") || word == "step-end") {
                    a.timing = word;
                } else if (word.ends_with('s')) {
                    if (!time) {
                        a.duration = duration(word);
                        time = true;
                    } else {
                        a.delay = duration(word);
                    }
                } else if (std::isdigit(static_cast<unsigned char>(word[0]))) {
                    a.count = number(word);
                } else {
                    a.name = word;
                }
            }
            return a;
        }

        using Keyframes = std::map<std::string, std::map<double, Style>>;

        Keyframes parse_keyframes(std::string text) {
            Keyframes out;
            text = css::strip_block_comments(text);
            std::size_t p{};
            while ((p = text.find("@keyframes", p)) != text.npos) {
                auto b = text.find('{', p);
                auto e = b + 1;
                unsigned depth = 1;
                while (e < text.size() && depth) {
                    if (text[e] == '{') {
                        ++depth;
                    }
                    if (text[e] == '}') {
                        --depth;
                    }
                    ++e;
                }
                if (depth) {
                    fail("unclosed keyframes");
                }
                auto name = trim(text.substr(p + 10, b - p - 10));
                auto body = text.substr(b + 1, e - b - 2);
                std::size_t q{};
                while (q < body.size()) {
                    auto start = body.find('{', q);
                    if (start == body.npos) {
                        break;
                    }
                    auto end = body.find('}', start);
                    if (end == body.npos) {
                        fail("keyframe block");
                    }
                    bool viewport_units{};
                    auto style = expand(declarations(body.substr(start + 1, end - start - 1)), {}, &viewport_units);
                    if (viewport_units) {
                        fail("viewport length in keyframes outside scene profile");
                    }
                    for (auto key : split(body.substr(q, start - q), ',')) {
                        double fraction = key == "from" ? 0 : key == "to" ? 1 : length(key);
                        if (fraction < 0 || fraction > 1) {
                            fail("keyframe offset");
                        }
                        // Like std::map::insert over a range: an earlier block keeps its value.
                        auto& frame = out[name][fraction];
                        for (const auto& [id, value] : style) {
                            if (!frame.contains(id)) {
                                frame.set(id, value);
                            }
                        }
                    }
                    q = end + 1;
                }
                p = e;
            }
            return out;
        }

        struct AnimationTrack {
            std::string property;
            std::map<double, std::string> values;
        };

        struct AnimationPlan {
            Animation clock;
            std::vector<AnimationTrack> tracks;
        };

        std::vector<AnimationPlan> compile_animations(const Style& target, const Keyframes& keyframes) {
            std::vector<AnimationPlan> plans;
            const auto value = get(target, PropertyId::Animation, "none");
            if (value == "none") {
                return plans;
            }
            const auto list = split(value, ',');
            for (std::size_t i = 0; i < list.size(); ++i) {
                auto a = animation(list[i]);
                if (!keyframes.contains(a.name)) {
                    fail("unknown animation " + a.name);
                }
                auto select = [&](const std::string& k) {
                    auto v = split(get(target, k), ',');
                    return v.empty() ? std::string{} : v[i % v.size()];
                };
                if (auto v = select("animation-delay"); !v.empty()) {
                    a.delay = duration(v);
                }
                if (auto v = select("animation-duration"); !v.empty()) {
                    a.duration = duration(v);
                }
                if (auto v = select("animation-timing-function"); !v.empty()) {
                    a.timing = v;
                }
                if (a.duration < 0 || a.count < 0 || (std::isfinite(a.count) && a.count != std::floor(a.count))) {
                    fail("nonnegative duration / integer iteration profile");
                }
                AnimationPlan plan;
                plan.clock = std::move(a);
                const auto& frames = keyframes.at(plan.clock.name);
                std::map<std::string, std::map<double, std::string>> tracks;
                for (const auto& [offset, s] : frames) {
                    for (const auto& [k, v] : s) {
                        tracks[std::string(property_name(k))][offset] = v.text();
                    }
                }
                for (auto& [k, values] : tracks) {
                    const auto fallback = k.starts_with("border-") && k.ends_with("-color")
                                              ? get(target, PropertyId::Color, "#000000")
                                          : k == "background-color" ? std::string("transparent")
                                          : k == "transform"        ? std::string("none")
                                          : k == "opacity"          ? std::string("1")
                                                                    : std::string("0");
                    const auto underlying = get(target, k, fallback.c_str());
                    if (!values.contains(0)) {
                        values[0] = underlying;
                    }
                    if (!values.contains(1)) {
                        values[1] = underlying;
                    }
                    plan.tracks.push_back({k, std::move(values)});
                }
                plans.push_back(std::move(plan));
            }
            return plans;
        }

        struct State {
            Style target;
            Style current;
            bool expand_dirty = true;
            // The expanded values used viewport lengths: a new viewport expands them again.
            bool viewport_units{};
            ScenePaintValues paint;
            std::vector<AnimationPlan> plans;
            bool plans_valid{};
            double animation_start{};
            std::string animation_value;

            struct Transition {
                std::string from;
                std::string to;
                std::string timing;
                double start{};
                double duration{};
            };

            std::map<std::string, Transition> transitions;
        };

        using SceneId = std::array<std::uint64_t, 3>;

        SceneId identity(NodeIdentity n) {
            return {n.view_epoch, n.slot, n.generation};
        }

        // Pairs of a node of the current build and the previous-frame node it was taken from; taken_from needs them
        // sorted by the first (sort_reused).
        template <class T>
        using Reused = std::vector<std::pair<const T*, const T*>>;
    } // namespace

    StyleProfile scene_style_profile() {
        StyleProfile p;
        p.scene_syntax = true;
        p.single_value_inset = true;
        p.four_value_edges = true;
        p.ua_stylesheet =
            "div,p,h1,h2,h3,h4,h5,h6,ul,ol,li,dl,dt,dd,section,article,header,footer,nav,main,aside,form,blockquote,"
            "figure,figcaption,address,hr{display:block}button{display:inline-block;text-align:center}"
            "span,b,strong{display:inline}"
            "svg{display:inline}*{box-sizing:border-box;border-style:solid;border-width:0}";
        for (const auto* k : {"display",
                              "position",
                              "width",
                              "height",
                              "min-width",
                              "min-height",
                              "max-width",
                              "max-height",
                              "left",
                              "right",
                              "top",
                              "bottom",
                              "margin",
                              "padding",
                              "border-width",
                              "border-radius",
                              "border-color",
                              "border-style",
                              "box-sizing",
                              "gap",
                              "row-gap",
                              "column-gap",
                              "flex",
                              "rotate",
                              "flex-direction",
                              "flex-wrap",
                              "flex-grow",
                              "flex-shrink",
                              "flex-basis",
                              "align-items",
                              "align-self",
                              "align-content",
                              "justify-content",
                              "justify-items",
                              "grid-template-columns",
                              "place-items",
                              "overflow",
                              "opacity",
                              "transform",
                              "transform-origin",
                              "transform-style",
                              "background",
                              "background-color",
                              "background-image",
                              "background-size",
                              "background-position",
                              "box-shadow",
                              "animation",
                              "animation-delay",
                              "animation-duration",
                              "animation-timing-function",
                              "transition-property",
                              "transition-duration",
                              "transition-delay",
                              "transition-timing-function",
                              "pointer-events",
                              "cursor",
                              "z-index",
                              "touch-action",
                              "user-select",
                              "-webkit-user-select",
                              "overflow-x",
                              "overflow-y",
                              "vertical-align",
                              "will-change"}) {
            p.properties[k] = {"", false, CssGrammar::Tokens, {}};
        }
        for (const auto* k : {"font-size",
                              "font-weight",
                              "font-family",
                              "line-height",
                              "letter-spacing",
                              "color",
                              "fill",
                              "fill-opacity",
                              "fill-rule",
                              "stroke",
                              "stroke-width",
                              "stroke-opacity",
                              "stroke-linecap",
                              "stroke-linejoin",
                              "stroke-dasharray",
                              "stroke-dashoffset",
                              "text-anchor",
                              "dominant-baseline",
                              "text-align",
                              "white-space",
                              "font-style",
                              "font-variant-numeric",
                              "text-transform"}) {
            p.properties[k] = {"", true, CssGrammar::Tokens, {}};
        }
        p.properties["pointer-events"] = {"auto", true, CssGrammar::Tokens, {}};
        p.properties["font-size"].initial = constants::default_font_size;
        p.properties["color"].initial = constants::default_color;
        p.properties["fill"].initial = constants::default_color;
        p.properties["stroke"].initial = "none";
        for (const auto* s : {"top", "right", "bottom", "left"}) {
            for (const auto* name : {"margin-", "padding-", "border-"}) {
                const auto key = std::string(name) + s;
                p.properties[key + (std::string(name) == "border-" ? "-width" : "")] = {
                    "0", false, CssGrammar::Tokens, {}};
                if (std::string(name) == "border-") {
                    p.properties[key + "-color"] = {"currentcolor", false, CssGrammar::Tokens, {}};
                    p.properties[key + "-style"] = {"solid", false, CssGrammar::Tokens, {}};
                }
            }
        }
        for (const auto* corner : {"top-left", "top-right", "bottom-right", "bottom-left"}) {
            p.properties[std::string("border-") + corner + "-radius"] = {"0", false, CssGrammar::Tokens, {}};
        }
        return p;
    }

    struct SceneSolver::Impl {
        const OwnerThread owner;
        std::string sheet;
        std::unique_ptr<CompiledStyle> compiled;
        StyleMemo style_memo;
        bool incremental = true;
        bool profiling = true;
        SceneSolver::Work work;
        StyleInput style_input;
        std::optional<ComputedStyle> cascade;
        std::optional<ComputedStyle> computed;
        std::optional<SceneFrame> previous;
        std::optional<DomSnapshot> previous_dom;
        std::optional<ComputedStyle> previous_computed;
        // What the last build took whole from the previous frame (build_into's static path): each output node and
        // computed style with the previous one it equals. Layout may since have moved the output's boxes.
        Reused<SceneNode> reused_nodes;
        Reused<ComputedStyle> reused_styles;
        float previous_width{};
        float previous_height{};
        // The frame's viewport, which viewport lengths resolve against, and the one the computed styles last used.
        std::array<float, 2> viewport{};
        std::array<float, 2> expanded_viewport{};
        Keyframes keyframes;
        InterpolationCache interpolation;
        PaintCache paint_cache;
        SceneText* text{};
        const SceneEffects* effects{};
        // While a frame is laid out: the extents of the atomic inlines measured, by node and available width, and
        // the last line's baseline of each inline formatting context root placed, from its border box top.
        std::map<std::pair<const SceneNode*, float>, SceneAtomicExtent> atomic_memo;
        std::map<const SceneNode*, float> context_baselines;
        std::map<SceneId, State> states;
        std::set<SceneId> live;
        double now = -1;
        bool animated{};

        // Cascade output remains canonical. Shorthand expansion is a separate
        // owner derivation, so it cannot turn memo hits into full property copies.
        void expand_into(const ComputedStyle& raw, ComputedStyle& out) {
            auto& state = states[identity(raw.identity)];
            if (out.identity != raw.identity || state.expand_dirty) {
                state.viewport_units = false;
                out.values = expand(raw.values, viewport, &state.viewport_units);
                state.expand_dirty = false;
            }
            out.identity = raw.identity;
            if (out.local_name != raw.local_name) {
                out.local_name = raw.local_name;
            }
            if (out.custom_properties != raw.custom_properties) {
                out.custom_properties = raw.custom_properties;
            }
            if (out.invalid_at_computed_value != raw.invalid_at_computed_value) {
                out.invalid_at_computed_value = raw.invalid_at_computed_value;
            }
            if (out.specified_properties != raw.specified_properties) {
                out.specified_properties = raw.specified_properties;
            }
            auto prior = std::move(out.children);
            out.children.clear();
            out.children.reserve(prior.capacity());
            for (std::size_t i = 0; i < raw.children.size(); ++i) {
                const auto& child = raw.children[i];
                auto index = i;
                if (index >= prior.size() || prior[index].identity != child.identity) {
                    index = 0;
                    while (index < prior.size() && prior[index].identity != child.identity) {
                        ++index;
                    }
                }
                out.children.push_back(index < prior.size() ? std::move(prior[index]) : ComputedStyle{});
                expand_into(child, out.children.back());
            }
        }

        bool static_subtree(const SceneNode& n) const {
            // An effect that reads the time changes every frame.
            if (n.effect && n.effect->program->animated) {
                return false;
            }
            if (n.tag != "#text") {
                auto it = states.find(identity(n.identity));
                if (it == states.end() || !it->second.plans_valid || !it->second.transitions.empty() ||
                    !it->second.plans.empty() || it->second.current != n.style) {
                    return false;
                }
            }
            for (const auto& child : n.children) {
                if (!static_subtree(child)) {
                    return false;
                }
            }
            return true;
        }

        bool same_inheritance(const Style* a, const Style* b) const {
            if (!a || !b) {
                return a == b;
            }
            for (const auto k : inherited_properties) {
                if (a->text(k) != b->text(k)) {
                    return false;
                }
            }
            return true;
        }

        void keep_live(const SceneNode& n) {
            if (n.tag != "#text") {
                live.insert(identity(n.identity));
            }
            for (const auto& c : n.children) {
                keep_live(c);
            }
        }

        void build_into(SceneNode& output, const DomSnapshot& d, const ComputedStyle& c, const Style* parent = nullptr,
                        const DomSnapshot* old_dom = nullptr, const ComputedStyle* old_computed = nullptr,
                        const SceneNode* old_node = nullptr, const Style* old_parent = nullptr) {
            if (incremental && old_dom && old_computed && old_node && dom::same_content(d, *old_dom) &&
                same_inheritance(parent, old_parent) && c == *old_computed && static_subtree(*old_node)) {
                keep_live(*old_node);
                ++work.static_subtree_reuses;
                if (!scene_paint_source_equal(output, *old_node)) {
                    work.geometry_changed |= !scene_geometry_equal(output, *old_node);
                    work.paint_changed = true;
                    output = *old_node;
                }
                reused_nodes.emplace_back(&output, old_node);
                reused_styles.emplace_back(&c, old_computed);
                return;
            }
            // A stable node target may own thousands of repeated property strings.
            // Move those values within the owner instead of reconstructing them;
            // animated targets and changed inheritance take the complete path.
            const auto previous_state = states.find(identity(d.identity));
            bool reuse_style = incremental && output.identity == d.identity && output.style == c.values &&
                               previous_state != states.end() && previous_state->second.plans_valid &&
                               previous_state->second.plans.empty() && previous_state->second.transitions.empty() &&
                               previous_state->second.target == output.style &&
                               previous_state->second.current == output.style;
            if (reuse_style && parent) {
                for (const auto k : inherited_properties) {
                    const auto* inherited = parent->find(k);
                    if (inherited && !specified(c, k) && output.style.text(k) != inherited->text()) {
                        reuse_style = false;
                        break;
                    }
                }
            }
            SceneNode out;
            // Layout outputs stay until layout runs again.
            out.box = output.box;
            out.lines = output.lines;
            out.fragments = output.fragments;
            out.identity = d.identity;
            out.tag = d.local_name;
            out.space = d.namespace_uri;
            out.attributes = d.attributes;
            if (reuse_style) {
                out.style = std::move(output.style);
            } else {
                out.style = incremental ? c.values : expand(c.values, viewport);
            }
            if ((d.scroll_left != 0 || d.scroll_top != 0) && get(out.style, PropertyId::Overflow) == "auto") {
                fail("scene scrolling outside supported non-overflowing auto profile");
            }
            if (d.namespace_uri == constants::svg_namespace_uri) {
                for (const auto* attribute : {"mask", "filter"}) {
                    if (d.attributes.contains(attribute)) {
                        fail(std::string("unsupported SVG attribute ") + attribute);
                    }
                }
                if (d.local_name == "text" && get(out.style, PropertyId::Stroke, "none") != "none") {
                    fail("SVG text stroke outside scene profile");
                }
            }
            for (const auto& t : d.children) {
                if (t.type == constants::text_node_type) {
                    out.text += t.data;
                }
            }
            if (std::any_of(d.children.begin(), d.children.end(),
                            [](const auto& n) {
                                return n.type == constants::element_node_type;
                            }) &&
                trim(out.text).empty() && get(out.style, PropertyId::WhiteSpace, "normal") == "normal") {
                out.text.clear();
            }
            if (parent) {
                for (const auto k : inherited_properties) {
                    const auto* inherited = parent->find(k);
                    if (inherited && !specified(c, k)) {
                        out.style.set(k, *inherited);
                    }
                }
            }
            out.text = transformed(std::move(out.text), out.style);
            if ((out.tag == "b" || out.tag == "strong") &&
                std::find(c.specified_properties.begin(), c.specified_properties.end(), "font-weight") ==
                    c.specified_properties.end()) {
                out.style[PropertyId::FontWeight] = constants::scene::bold_font_weight;
            }
            const auto id = identity(d.identity);
            live.insert(id);
            auto& st = states[id];
            const bool target_changed = out.style != st.target;
            if (!st.target.empty()) {
                const auto& target = out.style;
                const auto prop = get(target, PropertyId::TransitionProperty, "none");
                const auto timing = get(target, PropertyId::TransitionTimingFunction, "ease");
                const auto dur = get(target, PropertyId::TransitionDuration, "0s");
                if (prop != "none" && duration(dur) > 0) {
                    for (const auto& [property, v] : target) {
                        const auto k = property_name(property);
                        const auto* before = st.target.find(property);
                        if (!before || *before == v || k.starts_with("transition-") || k.starts_with("animation") ||
                            k.starts_with("--")) {
                            continue;
                        }
                        const auto props = split(prop, ',');
                        if (prop != "all" && std::find(props.begin(), props.end(), k) == props.end()) {
                            continue;
                        }
                        const auto* current = st.current.find(property);
                        st.transitions[std::string(k)] = {
                            current ? current->text() : before->text(), v.text(), timing,
                            now + duration(get(target, PropertyId::TransitionDelay, "0s")), duration(dur)};
                    }
                }
            }
            if (target_changed) {
                st.target = out.style;
                if (incremental) {
                    st.paint = compile_scene_paint_values(st.target);
                    ++work.paint_compilations;
                }
            }
            if (incremental) {
                out.paint = st.paint;
            }
            auto typed = [&](const std::string& property, const std::string& a, const std::string& b, double amount) {
                if (!out.paint) {
                    return;
                }
                const auto& keys = scene_paint_properties();
                auto i = std::find(keys.begin(), keys.end(), property);
                if (i == keys.end()) {
                    return;
                }
                out.paint->values[std::size_t(i - keys.begin())] = paint_cache.sample(a, b, amount);
                ++work.typed_samples;
            };
            const auto& target = st.target;
            for (auto i = st.transitions.begin(); i != st.transitions.end();) {
                const auto& tr = i->second;
                double t = (now - tr.start) / tr.duration;
                if (t >= 1) {
                    i = st.transitions.erase(i);
                    continue;
                }
                animated = true;
                const auto amount = easing(tr.timing, std::max(0., t));
                out.style[i->first] = lerp_value(tr.from, tr.to, amount, incremental ? &interpolation : nullptr);
                typed(i->first, tr.from, tr.to, amount);
                ++i;
            }
            const auto av = get(target, PropertyId::Animation, "none");
            if (st.animation_value != av) {
                st.animation_value = av;
                st.animation_start = now;
            }
            if (!incremental || target_changed || !st.plans_valid) {
                st.plans_valid = false;
                auto plans = compile_animations(target, keyframes);
                st.plans = std::move(plans);
                st.plans_valid = true;
                ++work.animation_compilations;
            } else {
                ++work.animation_reuses;
            }
            for (const auto& plan : st.plans) {
                const auto& a = plan.clock;
                double elapsed = now - st.animation_start - a.delay;
                if (elapsed < 0 && a.fill != "backwards" && a.fill != "both") {
                    animated = true;
                    continue;
                }
                const auto total = a.duration * a.count;
                bool end = elapsed >= total;
                if (end && a.fill != "forwards" && a.fill != "both") {
                    continue;
                }
                double iteration = a.duration > 0 ? std::floor(std::max(0., elapsed) / a.duration) : 0;
                double f = a.duration > 0 ? std::fmod(std::max(0., elapsed), a.duration) / a.duration : 1;
                if (end) {
                    iteration = std::max(0., a.count - 1);
                    f = 1;
                } else {
                    animated = true;
                }
                const bool alternate = a.direction == "alternate" || a.direction == "alternate-reverse";
                if ((a.direction == "reverse" || a.direction == "alternate-reverse") !=
                    (alternate && std::fmod(iteration, 2) >= 1)) {
                    f = 1 - f;
                }
                for (const auto& track : plan.tracks) {
                    const auto& values = track.values;
                    auto hi = values.upper_bound(f);
                    if (hi == values.end()) {
                        out.style[track.property] = values.rbegin()->second;
                        typed(track.property, values.rbegin()->second, values.rbegin()->second, 0);
                        continue;
                    }
                    auto lo = hi;
                    if (lo != values.begin()) {
                        --lo;
                    }
                    const auto amount = hi->first == lo->first ? 0 : (f - lo->first) / (hi->first - lo->first);
                    const auto eased = easing(a.timing, amount);
                    out.style[track.property] =
                        lerp_value(lo->second, hi->second, eased, incremental ? &interpolation : nullptr);
                    typed(track.property, lo->second, hi->second, eased);
                }
            }
            if (out.paint) {
                for (unsigned i = constants::scene::paint_slot_border_color; i < constants::scene::paint_property_count;
                     ++i) {
                    const auto key = scene_paint_properties()[i];
                    const auto v = get(out.style, key, "currentcolor");
                    if (v == "currentcolor" || v == "currentColor") {
                        out.paint->values[i] = out.paint->values[constants::scene::paint_slot_color];
                    }
                }
            }
            if (st.current != out.style) {
                st.current = out.style;
            }
            // A -weft-effect(name) background: the registered effect, with the parameters of the custom properties.
            if (const auto image = get(out.style, PropertyId::BackgroundImage, "none");
                image.starts_with("-weft-effect(")) {
                constexpr std::size_t open = std::string_view("-weft-effect(").size();
                const auto name = image.ends_with(')') ? trim(image.substr(open, image.size() - open - 1)) : "";
                if (name.empty() || !std::all_of(name.begin(), name.end(), [](char ch) {
                        return std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_';
                    })) {
                    fail("-weft-effect() takes one effect name");
                }
                if (!effects) {
                    fail("-weft-effect(" + name + ") without registered effects");
                }
                out.effect = effects->resolve(name, c.custom_properties);
                if (out.effect->program->animated) {
                    animated = true;
                    work.paint_changed = true;
                }
            }
            work.geometry_changed |= !scene_geometry_own_equal(output, out, reuse_style);
            work.paint_changed |= output.identity != out.identity || output.tag != out.tag ||
                                  output.space != out.space || output.text != out.text ||
                                  output.attributes != out.attributes || (!reuse_style && output.style != out.style) ||
                                  bool(output.paint) != bool(out.paint) ||
                                  (output.paint && out.paint && output.paint->values != out.paint->values) ||
                                  !scene_effect_equal(output.effect, out.effect);
            auto retained = std::move(output.children);
            std::size_t ci{};
            // Text beside elements becomes #text children. In an inline formatting context (an element that is not
            // flex or grid, whose in-flow element children are all inline-level) every text node does, spaces too:
            // they collapse with their neighbours in the lines. Elsewhere spaces-only text between boxes goes.
            bool elements{};
            const auto own_display = get(out.style, PropertyId::Display, "inline");
            bool inline_context = own_display != "flex" && own_display != "inline-flex" && own_display != "grid" &&
                                  own_display != "inline-grid";
            {
                std::size_t k{};
                for (const auto& n : d.children) {
                    if (n.type != constants::element_node_type) {
                        continue;
                    }
                    elements = true;
                    const auto& values = c.children.at(k++).values;
                    const auto display = values.text(PropertyId::Display, "inline");
                    const auto position = values.text(PropertyId::Position, "static");
                    if (display != "none" && position != "absolute" && position != "fixed" && display != "inline" &&
                        display != "inline-block" && display != "inline-flex" && display != "inline-grid") {
                        inline_context = false;
                    }
                }
            }
            inline_context = inline_context && elements;
            const bool mixed = elements && (inline_context || !trim(out.text).empty());
            if (mixed) {
                out.text.clear();
            }
            auto prior_child = [](const auto& children, NodeIdentity id,
                                  std::size_t hint) -> const typename std::decay_t<decltype(children)>::value_type* {
                if (hint < children.size() && children[hint].identity == id) {
                    return &children[hint];
                }
                for (const auto& value : children) {
                    if (value.identity == id) {
                        return &value;
                    }
                }
                return nullptr;
            };
            // At most one scene child per DOM child. SceneNode's move may throw (std::map), so a reallocation
            // would copy every subtree built so far.
            out.children.reserve(d.children.size());
            std::size_t di{};
            for (const auto& child : d.children) {
                const auto dom_index = di++;
                if (child.type == constants::element_node_type) {
                    const DomSnapshot* od =
                        old_dom ? prior_child(old_dom->children, child.identity, dom_index) : nullptr;
                    const ComputedStyle* oc =
                        old_computed ? prior_child(old_computed->children, child.identity, ci) : nullptr;
                    const SceneNode* on =
                        old_node ? prior_child(old_node->children, child.identity, out.children.size()) : nullptr;
                    auto current_index = out.children.size();
                    if (current_index >= retained.size() || retained[current_index].identity != child.identity) {
                        current_index = 0;
                        while (current_index < retained.size() && retained[current_index].identity != child.identity) {
                            ++current_index;
                        }
                    }
                    out.children.push_back(current_index < retained.size() ? std::move(retained[current_index])
                                                                           : SceneNode{});
                    build_into(out.children.back(), child, c.children.at(ci++), &out.style, od, oc, on,
                               old_node ? &old_node->style : nullptr);
                } else if (mixed && child.type == constants::text_node_type &&
                           (inline_context || !trim(child.data).empty())) {
                    // Anonymous inline text: the element's inherited properties and initial others.
                    SceneNode run;
                    run.identity = child.identity;
                    run.tag = "#text";
                    run.text = transformed(css::collapse_spaces(child.data), out.style);
                    for (const auto k : inherited_properties) {
                        if (const auto* v = out.style.find(k)) {
                            run.style.set(k, *v);
                        }
                    }
                    run.style[PropertyId::Display] = "inline";
                    run.style[PropertyId::Position] = "static";
                    out.children.push_back(std::move(run));
                }
            }
            if (out.style.contains(PropertyId::FontWeight) && (out.tag == "b" || out.tag == "strong") &&
                std::find(c.specified_properties.begin(), c.specified_properties.end(), "font-weight") ==
                    c.specified_properties.end()) {
                out.style[PropertyId::FontWeight] = constants::scene::bold_font_weight;
            }
            if (out.children.size() != retained.size()) {
                work.paint_changed = true;
                work.geometry_changed = true;
            } else {
                for (std::size_t n = 0; n < out.children.size(); ++n) {
                    if (out.children[n].identity != retained[n].identity) {
                        work.paint_changed = true;
                        work.geometry_changed = true;
                    }
                }
            }
            output = std::move(out);
        }

        // viewBox width over height; NaN when the viewBox is not four numbers with a positive size.
        static float view_box_ratio(std::string view) {
            std::replace(view.begin(), view.end(), ',', ' ');
            float x{};
            float top{};
            float w{};
            float h{};
            std::istringstream values(view);
            if (!(values >> x >> top >> w >> h) || !std::isfinite(w) || !std::isfinite(h) || w <= 0 || h <= 0) {
                return std::numeric_limits<float>::quiet_NaN();
            }
            return w / h;
        }

        // One run of the shared Yoga adapter over a subtree. Each HTML element is a box, except inline content: the
        // lines of an inline formatting context place inline elements and text, and an atomic inline is laid out by
        // a run of its own. The scene's inline layout measures a context: an element's own text in the element's
        // box, or its inline children in an anonymous block box. An svg element is a replaced box; the scene draws
        // its content in its own coordinates.
        struct LayoutRun {
            struct Context {
                SceneNode* root{};
                bool anonymous{};
                std::unique_ptr<SceneInlineContent> content;
                // Layouts by available width, the most recent last.
                std::vector<std::pair<float, SceneInlineLayout>> memo;
            };

            std::vector<LayoutBox> boxes;
            // The node of each box; null for an anonymous block.
            std::vector<SceneNode*> targets;
            std::map<std::size_t, Context> contexts;
            // Fixed boxes, laid out as absolute ones in their containing block (with these styles) and then placed in
            // their parent's coordinates: each box with its parent's.
            std::deque<SceneStyle> fixed_styles;
            std::vector<std::pair<std::size_t, std::size_t>> fixed;
        };

        // A box that contains its fixed descendants (and absolute ones): one with a transform, or one about to have
        // one.
        static bool contains_fixed(const SceneStyle& s) {
            return s.text(PropertyId::Transform, "none") != "none" ||
                   s.text(PropertyId::WillChange, "auto").find("transform") != std::string_view::npos;
        }

        // The anonymous block box that holds an element's inline children.
        static const SceneStyle& anonymous_block() {
            static const SceneStyle style = [] {
                SceneStyle s;
                s[PropertyId::Display] = "block";
                return s;
            }();
            return style;
        }

        static bool out_of_flow(const SceneNode& n) {
            const auto p = n.style.text(PropertyId::Position, "static");
            return p == "absolute" || p == "fixed";
        }

        // Clears the layout of a subtree whose boxes the lines of an inline formatting context place.
        static void clear_layout(SceneNode& n) {
            n.box = {};
            n.lines.clear();
            n.fragments.clear();
            for (auto& c : n.children) {
                clear_layout(c);
            }
        }

        // `fixed_block` is the box of the containing block of fixed boxes here: the run's top when it is the scene's
        // root (the viewport), or the nearest box with a transform; none inside an inline-block without one.
        static void collect_boxes(SceneNode& n, std::size_t parent, LayoutRun& run,
                                  std::optional<std::size_t> fixed_block) {
            bool dashed{};
            for (std::size_t side = 0; side < border_style_edges.size(); ++side) {
                const auto style = n.style.text(border_style_edges[side], "solid");
                if (style != "solid" && style != "dotted" && style != "dashed" && style != "none" &&
                    style != "hidden") {
                    fail("scene border style " + std::string(style) + " outside scene profile");
                }
                dashed |=
                    (style == "dotted" || style == "dashed") && length(n.style.text(border_width_edges[side], "0")) > 0;
            }
            if (dashed && std::any_of(radius_corners.begin(), radius_corners.end(), [&](PropertyId k) {
                    return length(n.style.text(k, "0")) > 0;
                })) {
                fail("dotted or dashed border with rounded corners outside scene profile");
            }
            // The scene scrolls only boxes whose content fits (checked after layout).
            const auto overflow = n.style.text(PropertyId::Overflow, "visible");
            if (overflow != "visible" && overflow != "hidden" && overflow != "auto") {
                fail("unsupported scene value overflow:" + std::string(overflow));
            }
            n.lines.clear();
            n.fragments.clear();
            const auto index = run.boxes.size();
            LayoutBox box;
            box.style = &n.style;
            box.parent = parent;
            // A fixed box is an absolute box of its containing block. Away from its parent, a static position (both
            // insets of an axis auto) would be the parent's: outside the profile.
            if (n.style.text(PropertyId::Position, "static") == "fixed" &&
                n.style.text(PropertyId::Display, "inline") != "none") {
                if (!fixed_block) {
                    fail("position: fixed inside an inline-block outside scene profile");
                }
                const auto automatic = [&](PropertyId a, PropertyId b) {
                    return n.style.text(a, "auto") == "auto" && n.style.text(b, "auto") == "auto";
                };
                if (*fixed_block != parent && (automatic(PropertyId::Left, PropertyId::Right) ||
                                               automatic(PropertyId::Top, PropertyId::Bottom))) {
                    fail("position: fixed with a static position outside scene profile");
                }
                auto& style = run.fixed_styles.emplace_back(n.style);
                style[PropertyId::Position] = "absolute";
                box.style = &style;
                box.parent = *fixed_block;
                run.fixed.emplace_back(index, parent);
            }
            const auto inner_block = contains_fixed(n.style) ? std::optional<std::size_t>(index) : fixed_block;
            if (n.space == constants::svg_namespace_uri) {
                box.replaced = true;
                if (const auto it = n.attributes.find("width"); it != n.attributes.end()) {
                    box.replaced_width = it->second;
                }
                if (const auto it = n.attributes.find("height"); it != n.attributes.end()) {
                    box.replaced_height = it->second;
                }
                if (const auto it = n.attributes.find("viewBox"); n.tag == "svg" && it != n.attributes.end()) {
                    box.ratio = view_box_ratio(it->second);
                }
                run.boxes.push_back(box);
                run.targets.push_back(&n);
                return;
            }
            if (!n.text.empty() && !n.children.empty()) {
                fail("mixed inline content outside scene formatting profile");
            }
            box.text = !n.text.empty();
            run.boxes.push_back(box);
            run.targets.push_back(&n);
            if (box.text) {
                run.contexts[index].root = &n;
                return;
            }
            if (scene_inline_children(n)) {
                LayoutBox anonymous;
                anonymous.style = &anonymous_block();
                anonymous.parent = index;
                anonymous.text = true;
                auto& context = run.contexts[run.boxes.size()];
                context.root = &n;
                context.anonymous = true;
                run.boxes.push_back(anonymous);
                run.targets.push_back(nullptr);
                // Positioned children stay boxes of the element; the lines place the others.
                for (auto& c : n.children) {
                    if (out_of_flow(c) && c.style.text(PropertyId::Display) != "none") {
                        collect_boxes(c, index, run, inner_block);
                    } else {
                        clear_layout(c);
                    }
                }
                return;
            }
            for (auto& c : n.children) {
                collect_boxes(c, index, run, inner_block);
            }
        }

        // A box's padding box in its own coordinates, less its corners' largest radius: what it shows of its content
        // when it clips.
        static std::array<float, 4> clip_rect(const SceneNode& c) {
            const float base = c.box[2];
            float radius{};
            for (const auto k : radius_corners) {
                radius = std::max(radius, float(length(c.style.text(k, "0"), base)));
            }
            return {edge_length(c.style, PropertyId::BorderLeftWidth, base) + radius,
                    edge_length(c.style, PropertyId::BorderTopWidth, base) + radius,
                    c.box[2] - edge_length(c.style, PropertyId::BorderRightWidth, base) - radius,
                    c.box[3] - edge_length(c.style, PropertyId::BorderBottomWidth, base) - radius};
        }

        // A fixed box is painted and hit where it sits in the tree, inside the clips of the boxes between it and its
        // containing block, which CSS does not apply to it: they must hold what its containing block shows of it
        // (`bound`: the viewport at the root, a containing block's padding box when it clips), so they change
        // nothing. `at` is n's origin in its containing block, `clips` the clips there.
        static void check_fixed_clips(const SceneNode& n, std::array<float, 2> at,
                                      std::vector<std::array<float, 4>>& clips, const std::array<float, 4>& bound) {
            const auto bounds = [](auto&& self, const SceneNode& c, std::array<float, 2> origin,
                                   std::array<float, 4>& out) -> void {
                if (c.style.text(PropertyId::Display, "inline") == "none") {
                    return;
                }
                out = {std::min(out[0], origin[0]), std::min(out[1], origin[1]), std::max(out[2], origin[0] + c.box[2]),
                       std::max(out[3], origin[1] + c.box[3])};
                for (const auto& child : c.children) {
                    self(self, child, {origin[0] + child.box[0], origin[1] + child.box[1]}, out);
                }
            };
            for (const auto& c : n.children) {
                if (c.style.text(PropertyId::Display, "inline") == "none") {
                    continue;
                }
                const std::array<float, 2> origin{at[0] + c.box[0], at[1] + c.box[1]};
                if (c.style.text(PropertyId::Position, "static") == "fixed") {
                    std::array<float, 4> extent{origin[0], origin[1], origin[0], origin[1]};
                    bounds(bounds, c, origin, extent);
                    extent = {std::max(extent[0], bound[0]), std::max(extent[1], bound[1]),
                              std::min(extent[2], bound[2]), std::min(extent[3], bound[3])};
                    constexpr float tolerance = constants::scene::overflow_tolerance_px;
                    for (const auto& clip : clips) {
                        if (extent[2] <= extent[0] || extent[3] <= extent[1]) {
                            break;
                        }
                        if (extent[0] < clip[0] - tolerance || extent[1] < clip[1] - tolerance ||
                            extent[2] > clip[2] + tolerance || extent[3] > clip[3] + tolerance) {
                            fail("position: fixed beyond the overflow clip of a box it escapes outside scene profile");
                        }
                    }
                }
                if (contains_fixed(c.style)) {
                    std::vector<std::array<float, 4>> inner;
                    constexpr float unbounded = std::numeric_limits<float>::infinity();
                    check_fixed_clips(c, {0, 0}, inner,
                                      c.style.text(PropertyId::Overflow, "visible") != "visible"
                                          ? clip_rect(c)
                                          : std::array<float, 4>{-unbounded, -unbounded, unbounded, unbounded});
                    continue;
                }
                if (c.space == constants::svg_namespace_uri) {
                    continue;
                }
                const bool clips_overflow = c.style.text(PropertyId::Overflow, "visible") != "visible";
                if (clips_overflow) {
                    const auto r = clip_rect(c);
                    clips.push_back({origin[0] + r[0], origin[1] + r[1], origin[0] + r[2], origin[1] + r[3]});
                }
                check_fixed_clips(c, origin, clips, bound);
                if (clips_overflow) {
                    clips.pop_back();
                }
            }
        }

        // An edge length of a box in px: px, em of its font size, or a percentage of `base`.
        static float edge_length(const SceneStyle& s, PropertyId id, float base) {
            const auto v = css::trim(s.text(id, "0"));
            if (v == "auto") {
                return 0;
            }
            if (v.ends_with('%')) {
                return float(length(v, base));
            }
            const auto [digits, unit] = css::split_unit(v, {.px = true, .em = true});
            const auto n = float(number(digits));
            return unit == css::Unit::Em ? n * float(length(s.text(PropertyId::FontSize, constants::default_font_size)))
                                         : n;
        }

        // A laid-out box's baseline from its border box top: the last line box of its content (CSS 2.1 10.8.1; a flex
        // container's first item's), none for a replaced element, a box that clips its overflow, or a box without
        // lines.
        std::optional<float> baseline_of(const SceneNode& n) const {
            if (n.space == constants::svg_namespace_uri || n.style.text(PropertyId::Overflow, "visible") != "visible") {
                return std::nullopt;
            }
            if (const auto it = context_baselines.find(&n); it != context_baselines.end()) {
                return it->second;
            }
            const auto d = n.style.text(PropertyId::Display, "inline");
            const auto in_flow = [](const SceneNode& c) {
                return c.style.text(PropertyId::Display) != "none" && !out_of_flow(c);
            };
            if (d == "flex" || d == "inline-flex") {
                for (const auto& c : n.children) {
                    if (in_flow(c)) {
                        const auto b = baseline_of(c);
                        return b ? std::optional<float>(c.box[1] + *b) : std::nullopt;
                    }
                }
                return std::nullopt;
            }
            for (auto c = n.children.rbegin(); c != n.children.rend(); ++c) {
                if (in_flow(*c)) {
                    if (const auto b = baseline_of(*c)) {
                        return c->box[1] + *b;
                    }
                }
            }
            return std::nullopt;
        }

        // Lays out an atomic inline's subtree, shrunk to fit the available width, and returns its extents; its box
        // is placed later.
        SceneAtomicExtent lay_out_atomic(SceneNode& node, float available) {
            SceneAtomicExtent e;
            const float base = std::isfinite(available) ? available : 0;
            e.margin = {edge_length(node.style, PropertyId::MarginTop, base),
                        edge_length(node.style, PropertyId::MarginRight, base),
                        edge_length(node.style, PropertyId::MarginBottom, base),
                        edge_length(node.style, PropertyId::MarginLeft, base)};
            LayoutOptions options;
            if (std::isfinite(available)) {
                options.fit_width = available;
            }
            layout_run(node, options);
            e.width = node.box[2];
            e.height = node.box[3];
            // Without lines, the bottom margin edge.
            e.baseline = baseline_of(node).value_or(e.height + e.margin[2]);
            return e;
        }

        SceneAtomicExtent atomic_extent(SceneNode& node, float available) {
            const auto key = std::pair{static_cast<const SceneNode*>(&node), available};
            if (const auto it = atomic_memo.find(key); it != atomic_memo.end()) {
                return it->second;
            }
            const auto e = lay_out_atomic(node, available);
            atomic_memo.emplace(key, e);
            return e;
        }

        // Places a context's lines at its final width: text lines on their owners, inline box fragments, atomic
        // inlines (laid out again at that width), and each node's box around its pieces, relative to its parent.
        // origin is the top left of the content box in root's border box.
        void place(SceneNode& root, const SceneInlineLayout& layout, std::array<float, 2> origin, float available) {
            struct Bounds {
                float left = std::numeric_limits<float>::infinity();
                float top = std::numeric_limits<float>::infinity();
                float right = -std::numeric_limits<float>::infinity();
                float bottom = -std::numeric_limits<float>::infinity();

                void add(const std::array<float, 4>& r) {
                    left = std::min(left, r[0]);
                    top = std::min(top, r[1]);
                    right = std::max(right, r[0] + r[2]);
                    bottom = std::max(bottom, r[1] + r[3]);
                }
            };

            std::map<const SceneNode*, Bounds> bounds;
            std::map<const SceneNode*, std::vector<SceneLine>> lines;
            std::map<const SceneNode*, std::vector<SceneFragment>> fragments;
            for (const auto& p : layout.pieces) {
                const std::array<float, 4> r{origin[0] + p.rect[0], origin[1] + p.rect[1], p.rect[2], p.rect[3]};
                switch (p.kind) {
                    case SceneInlinePiece::Kind::Text:
                        if (p.node == &root) {
                            root.lines.push_back({p.text, r, p.baseline});
                        } else {
                            lines[p.node].push_back({p.text, r, p.baseline});
                            // An inline element's box is its fragments'; a text node's its lines'.
                            if (p.node->tag == "#text") {
                                bounds[p.node].add(r);
                            }
                        }
                        break;
                    case SceneInlinePiece::Kind::Box:
                        fragments[p.node].push_back({r, p.first, p.last});
                        bounds[p.node].add(r);
                        break;
                    case SceneInlinePiece::Kind::Atomic:
                        lay_out_atomic(const_cast<SceneNode&>(*p.node), available);
                        bounds[p.node].add(r);
                        break;
                }
            }
            const auto assign = [&](auto&& self, SceneNode& parent, float px, float py) -> void {
                for (auto& c : parent.children) {
                    if (c.style.text(PropertyId::Display) == "none" || (&parent == &root && out_of_flow(c))) {
                        continue;
                    }
                    // A node without pieces (an empty inline element) sits at its parent's origin.
                    std::array<float, 4> at{px, py, 0, 0};
                    if (const auto b = bounds.find(&c); b != bounds.end()) {
                        at = {b->second.left, b->second.top, b->second.right - b->second.left,
                              b->second.bottom - b->second.top};
                    }
                    if (scene_atomic_inline(c)) {
                        c.box[0] = at[0] - px;
                        c.box[1] = at[1] - py;
                        continue;
                    }
                    c.box = {at[0] - px, at[1] - py, at[2], at[3]};
                    if (const auto l = lines.find(&c); l != lines.end()) {
                        for (auto line : l->second) {
                            line.rect[0] -= at[0];
                            line.rect[1] -= at[1];
                            c.lines.push_back(std::move(line));
                        }
                    }
                    if (const auto f = fragments.find(&c); f != fragments.end()) {
                        for (auto fragment : f->second) {
                            fragment.rect[0] -= at[0];
                            fragment.rect[1] -= at[1];
                            c.fragments.push_back(fragment);
                        }
                    }
                    // A relatively positioned inline element moves with its content.
                    if (c.style.text(PropertyId::Position, "static") == "relative") {
                        const auto left = c.style.text(PropertyId::Left, "auto");
                        const auto top = c.style.text(PropertyId::Top, "auto");
                        if (left != "auto") {
                            c.box[0] += edge_length(c.style, PropertyId::Left, available);
                        }
                        if (top != "auto") {
                            c.box[1] += edge_length(c.style, PropertyId::Top, available);
                        }
                    }
                    self(self, c, at[0], at[1]);
                }
            };
            assign(assign, root, 0, 0);
        }

        // Lays out the subtree of `top` and writes the boxes, lines and fragments of its nodes; options size `top`.
        // `scene_root`: top is the scene's root, the containing block of fixed boxes without a transformed ancestor.
        void layout_run(SceneNode& top, const LayoutOptions& options, bool scene_root = false) {
            LayoutRun run;
            collect_boxes(top, LayoutBox::root, run, scene_root ? std::optional<std::size_t>(0) : std::nullopt);
            const auto atomic = [&](const SceneNode& node, float available) {
                return atomic_extent(const_cast<SceneNode&>(node), available);
            };
            const auto content = [&](std::size_t i) -> LayoutRun::Context& {
                auto& c = run.contexts.at(i);
                if (!c.content) {
                    c.content = std::make_unique<SceneInlineContent>(*text, *c.root);
                }
                return c;
            };
            const auto at = [&](std::size_t i, float available) -> const SceneInlineLayout& {
                auto& c = content(i);
                for (const auto& [width, layout] : c.memo) {
                    if (width == available) {
                        return layout;
                    }
                }
                if (c.memo.size() >= std::size_t(constants::scene::inline_layout_memo_entries)) {
                    c.memo.erase(c.memo.begin());
                }
                c.memo.emplace_back(available, c.content->layout(available, atomic));
                return c.memo.back().second;
            };
            const auto lines = [&](std::size_t i, float available) {
                const auto& l = at(i, available);
                return TextMetrics{l.width, l.height, l.baseline, l.lines};
            };
            const auto min_content = [&](std::size_t i) {
                return at(i, 0).width;
            };
            const auto wraps = [&](std::size_t i) {
                return content(i).content->breakable();
            };
            const auto geometry = layout_boxes(run.boxes, {lines, min_content, wraps}, options);
            for (std::size_t i = 0; i < run.boxes.size(); ++i) {
                if (auto* n = run.targets[i]) {
                    const auto& g = geometry[i];
                    n->box = {g.left, g.top, g.width, g.height};
                }
            }
            // Fixed boxes in their parent's coordinates: no box between them and their containing block has a
            // transform.
            for (const auto& [i, parent] : run.fixed) {
                run.targets[i]->box[0] = geometry[i].x - geometry[parent].x;
                run.targets[i]->box[1] = geometry[i].y - geometry[parent].y;
            }
            constexpr float unbounded = std::numeric_limits<float>::infinity();
            for (auto& [i, c] : run.contexts) {
                auto& root = *c.root;
                std::array<float, 2> origin{};
                float available{};
                float shift_x{};
                float shift_y{};
                if (c.anonymous) {
                    origin = {geometry[i].left, geometry[i].top};
                    available = geometry[i].width;
                } else {
                    const auto& s = root.style;
                    const float base = root.box[2];
                    const float left = edge_length(s, PropertyId::BorderLeftWidth, base) +
                                       edge_length(s, PropertyId::PaddingLeft, base);
                    const float right = edge_length(s, PropertyId::BorderRightWidth, base) +
                                        edge_length(s, PropertyId::PaddingRight, base);
                    const float over =
                        edge_length(s, PropertyId::BorderTopWidth, base) + edge_length(s, PropertyId::PaddingTop, base);
                    const float bottom = edge_length(s, PropertyId::BorderBottomWidth, base) +
                                         edge_length(s, PropertyId::PaddingBottom, base);
                    origin = {left, over};
                    available = std::max(0.f, root.box[2] - left - right);
                    const auto d = s.text(PropertyId::Display, "block");
                    if (d == "flex" || d == "inline-flex") {
                        // The text is an anonymous flex item: as wide as its widest line up to the content box,
                        // placed by justify-content and align-items.
                        available = std::min(available, at(i, unbounded).width);
                        const float free_x = std::max(0.f, root.box[2] - left - right) - available;
                        const float free_y = std::max(0.f, root.box[3] - over - bottom) - at(i, available).height;
                        const auto justify = s.text(PropertyId::JustifyContent, "flex-start");
                        const auto align = s.text(PropertyId::AlignItems, "stretch");
                        shift_x = justify == "center"                         ? free_x / 2
                                  : justify == "flex-end" || justify == "end" ? free_x
                                                                              : 0;
                        shift_y = align == "center" ? free_y / 2 : align == "flex-end" || align == "end" ? free_y : 0;
                    }
                }
                const auto& layout = at(i, available);
                origin[0] += shift_x;
                origin[1] += shift_y;
                if (layout.lines) {
                    context_baselines[&root] = origin[1] + layout.last_baseline;
                }
                place(root, layout, origin, available);
            }
            for (std::size_t i = 1; i < run.boxes.size(); ++i) {
                const auto* child = run.targets[i];
                const auto* parent = run.targets[run.boxes[i].parent];
                if (!child || !parent || get(parent->style, PropertyId::Overflow) != "auto") {
                    continue;
                }
                // In the layout's coordinates: a fixed box against its containing block.
                const auto& g = geometry[i];
                const auto& p = geometry[run.boxes[i].parent];
                if (g.left < -constants::scene::overflow_tolerance_px ||
                    g.top < -constants::scene::overflow_tolerance_px ||
                    g.left + g.width > p.width + constants::scene::overflow_tolerance_px ||
                    g.top + g.height > p.height + constants::scene::overflow_tolerance_px) {
                    fail("scene scrolling outside supported non-overflowing auto profile");
                }
            }
        }

        // Element geometry in tree order: border boxes in layout coordinates (SceneGeometry applies transforms), an
        // inline element's fragments as its client rects.
        static void list_elements(const SceneNode& n, float x, float y, std::vector<ElementGeometry>& out) {
            const float ax = x + n.box[0];
            const float ay = y + n.box[1];
            if (n.tag != "#text") {
                ElementGeometry e;
                e.identity = n.identity;
                e.rect = {ax, ay, n.box[2], n.box[3]};
                if (n.fragments.empty()) {
                    e.client_rects = {e.rect};
                    e.client_width = n.box[2];
                    e.client_height = n.box[3];
                } else {
                    for (const auto& f : n.fragments) {
                        e.client_rects.push_back({ax + f.rect[0], ay + f.rect[1], f.rect[2], f.rect[3]});
                    }
                }
                e.scroll_width = e.client_width;
                e.scroll_height = e.client_height;
                out.push_back(e);
            }
            if (n.space == constants::svg_namespace_uri) {
                return;
            }
            for (const auto& c : n.children) {
                list_elements(c, ax, ay, out);
            }
        }

        // Lays out the frame in the viewport: parent-local boxes, lines and fragments on the scene nodes, and element
        // geometry in tree order.
        void layout_frame(SceneFrame& f, float w, float h) {
            atomic_memo.clear();
            context_baselines.clear();
            LayoutOptions options;
            options.viewport = std::array{w, h};
            layout_run(f.root, options, true);
            std::vector<std::array<float, 4>> clips;
            check_fixed_clips(f.root, {0, 0}, clips, {0, 0, w, h});
            atomic_memo.clear();
            f.elements.clear();
            list_elements(f.root, 0, 0, f.elements);
        }
    };

    namespace {
        // Conservative layout signature: only known paint-only properties are omitted.
        // A transform may change the containing block when it crosses `none`.
        bool same_layout_style(const Style& a, const Style& b) {
            static const auto ignored = [] {
                std::array<bool, property_count> out{};
                for (const auto id : {PropertyId::Opacity,
                                      PropertyId::TransformOrigin,
                                      PropertyId::TransformStyle,
                                      PropertyId::BackgroundColor,
                                      PropertyId::BackgroundImage,
                                      PropertyId::BackgroundSize,
                                      PropertyId::BackgroundPosition,
                                      PropertyId::Color,
                                      PropertyId::Fill,
                                      PropertyId::Stroke,
                                      PropertyId::StrokeWidth,
                                      PropertyId::TextAnchor,
                                      PropertyId::DominantBaseline,
                                      PropertyId::BorderTopColor,
                                      PropertyId::BorderRightColor,
                                      PropertyId::BorderBottomColor,
                                      PropertyId::BorderLeftColor,
                                      PropertyId::BorderColor,
                                      PropertyId::BorderRadius,
                                      PropertyId::BorderTopLeftRadius,
                                      PropertyId::BorderTopRightRadius,
                                      PropertyId::BorderBottomRightRadius,
                                      PropertyId::BorderBottomLeftRadius,
                                      PropertyId::Animation,
                                      PropertyId::AnimationDelay,
                                      PropertyId::AnimationDuration,
                                      PropertyId::AnimationTimingFunction,
                                      PropertyId::TransitionProperty,
                                      PropertyId::TransitionDuration,
                                      PropertyId::TransitionDelay,
                                      PropertyId::TransitionTimingFunction,
                                      PropertyId::PointerEvents,
                                      PropertyId::Cursor,
                                      PropertyId::ZIndex,
                                      PropertyId::Transform}) {
                    out[std::size_t(id)] = true;
                }
                return out;
            }();
            if ((get(a, PropertyId::Transform, "none") == "none") !=
                (get(b, PropertyId::Transform, "none") == "none")) {
                return false;
            }
            auto x = a.begin();
            auto y = b.begin();
            for (;;) {
                while (x != a.end() && ignored[std::size_t(x->first)]) {
                    ++x;
                }
                while (y != b.end() && ignored[std::size_t(y->first)]) {
                    ++y;
                }
                if (x == a.end() || y == b.end()) {
                    return x == a.end() && y == b.end();
                }
                if (*x != *y) {
                    return false;
                }
                ++x;
                ++y;
            }
        }

        bool same_layout(const SceneNode& a, const SceneNode& b) {
            if (a.identity != b.identity || a.tag != b.tag || a.space != b.space || a.text != b.text ||
                !same_layout_style(a.style, b.style)) {
                return false;
            }
            if (a.space == constants::svg_namespace_uri) {
                for (const auto* key : {"width", "height", "viewBox"}) {
                    if (get(a.attributes, key) != get(b.attributes, key)) {
                        return false;
                    }
                }
                return true;
            }
            if (a.children.size() != b.children.size()) {
                return false;
            }
            for (std::size_t i = 0; i < a.children.size(); ++i) {
                if (!same_layout(a.children[i], b.children[i])) {
                    return false;
                }
            }
            return true;
        }

        void copy_boxes(SceneNode& a, const SceneNode& b) {
            a.box = b.box;
            a.lines = b.lines;
            a.fragments = b.fragments;
            if (a.space != constants::svg_namespace_uri) {
                for (std::size_t i = 0; i < a.children.size(); ++i) {
                    copy_boxes(a.children[i], b.children[i]);
                }
            }
        }

        // Whether the build took `from` whole from `to`.
        template <class T>
        bool taken_from(const Reused<T>& reused, const T& from, const T& to) {
            const auto it = std::lower_bound(reused.begin(), reused.end(), &from, [](const auto& entry, const T* key) {
                return std::less<const T*>{}(entry.first, key);
            });
            return it != reused.end() && it->first == &from && it->second == &to;
        }

        template <class T>
        void sort_reused(Reused<T>& reused) {
            std::sort(reused.begin(), reused.end(), [](const auto& a, const auto& b) {
                return std::less<const T*>{}(a.first, b.first);
            });
        }

        template <class T>
        void assign_changed(T& to, const T& from) {
            if (!(to == from)) {
                to = from;
            }
        }

        // Brings to.children in line with from.children. Children match one to one by identity; `follow` brings each
        // matched child up to date where it is, before any child moves, and a child new to this parent is copied.
        template <class T, class Follow>
        void follow_children(T& to, const T& from, const Follow& follow) {
            const auto count = from.children.size();
            if (to.children.size() == count &&
                std::equal(from.children.begin(), from.children.end(), to.children.begin(), [](const T& a, const T& b) {
                    return a.identity == b.identity;
                })) {
                for (std::size_t i = 0; i < count; ++i) {
                    follow(to.children[i], from.children[i]);
                }
                return;
            }
            std::vector<bool> taken(to.children.size());
            std::vector<T*> matched(count);
            for (std::size_t i = 0; i < count; ++i) {
                const auto& id = from.children[i].identity;
                auto j = i;
                if (j >= to.children.size() || taken[j] || to.children[j].identity != id) {
                    j = 0;
                    while (j < to.children.size() && (taken[j] || to.children[j].identity != id)) {
                        ++j;
                    }
                }
                if (j < to.children.size()) {
                    taken[j] = true;
                    matched[i] = &to.children[j];
                    follow(*matched[i], from.children[i]);
                }
            }
            std::vector<T> children;
            children.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                if (matched[i]) {
                    children.push_back(std::move(*matched[i]));
                } else {
                    children.push_back(from.children[i]);
                }
            }
            to.children = std::move(children);
        }

        // Makes the previous frame's node `to` equal the built node `from`. Where the build took `from` whole from
        // `to`, only the boxes layout wrote can differ.
        void follow(SceneNode& to, const SceneNode& from, const Reused<SceneNode>& reused) {
            if (taken_from(reused, from, to)) {
                copy_boxes(to, from);
                return;
            }
            to.identity = from.identity;
            assign_changed(to.tag, from.tag);
            assign_changed(to.space, from.space);
            assign_changed(to.text, from.text);
            assign_changed(to.attributes, from.attributes);
            assign_changed(to.style, from.style);
            to.box = from.box;
            assign_changed(to.lines, from.lines);
            assign_changed(to.fragments, from.fragments);
            if (bool(to.paint) != bool(from.paint) || (to.paint && to.paint->values != from.paint->values)) {
                to.paint = from.paint;
            }
            if (!scene_effect_equal(to.effect, from.effect)) {
                to.effect = from.effect;
            }
            follow_children(to, from, [&](SceneNode& a, const SceneNode& b) {
                follow(a, b, reused);
            });
        }

        // Makes the previous computed style `to` equal `from`; where the build took `from` whole, they are equal.
        void follow(ComputedStyle& to, const ComputedStyle& from, const Reused<ComputedStyle>& reused) {
            if (taken_from(reused, from, to)) {
                return;
            }
            to.identity = from.identity;
            assign_changed(to.local_name, from.local_name);
            assign_changed(to.values, from.values);
            assign_changed(to.custom_properties, from.custom_properties);
            assign_changed(to.invalid_at_computed_value, from.invalid_at_computed_value);
            assign_changed(to.specified_properties, from.specified_properties);
            follow_children(to, from, [&](ComputedStyle& a, const ComputedStyle& b) {
                follow(a, b, reused);
            });
        }
    } // namespace

    SceneSolver::Work SceneSolver::work() const {
        impl_->owner.require(Diag::SceneOwner);
        return impl_->work;
    }

    SceneSolver::SceneSolver(std::string s, std::string k, SceneText& text, bool incremental)
        : impl_(std::make_unique<Impl>()) {
        impl_->sheet = std::move(s);
        impl_->incremental = incremental;
        impl_->compiled = std::make_unique<CompiledStyle>(impl_->sheet, scene_style_profile());
        impl_->keyframes = parse_keyframes(std::move(k));
        impl_->text = &text;
    }

    SceneSolver::~SceneSolver() = default;

    void SceneSolver::set_profiling(bool enabled) {
        impl_->owner.require(Diag::SceneOwner);
        impl_->profiling = enabled;
    }

    void SceneSolver::set_effects(const SceneEffects* effects) {
        impl_->owner.require(Diag::SceneOwner);
        impl_->effects = effects;
    }

    void SceneSolver::reset() {
        impl_->owner.require(Diag::SceneOwner);
        impl_->style_memo.clear();
        impl_->states.clear();
        impl_->interpolation = {};
        impl_->paint_cache = {};
        impl_->style_input = {};
        impl_->cascade.reset();
        impl_->computed.reset();
        impl_->previous.reset();
        impl_->previous_dom.reset();
        impl_->previous_computed.reset();
        impl_->reused_nodes.clear();
        impl_->reused_styles.clear();
        impl_->work = {};
        impl_->now = -1;
    }

    SceneFrame SceneSolver::frame(const DomSnapshot& d, float w, float h, double time) {
        SceneFrame out;
        update(d, out, w, h, time);
        return out;
    }

    void SceneSolver::update(const DomSnapshot& d, SceneFrame& f, float w, float h, double time) {
        auto& i = *impl_;
        i.owner.require(Diag::SceneOwner);
        if (!std::isfinite(time) || time < i.now || !std::isfinite(w) || !std::isfinite(h) || w <= 0 || h <= 0 ||
            w > constants::scene::max_viewport_dimension || h > constants::scene::max_viewport_dimension) {
            fail("monotonic clock/viewport");
        }
        const auto clock = [&] {
            return i.profiling ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        };
        const auto profile_start = clock();
        auto ms = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        i.now = time;
        i.live.clear();
        i.animated = false;
        i.work.geometry_changed = i.work.paint_changed = false;
        // Viewport lengths follow the viewport: the values that used one are expanded again, though the cascade
        // stands.
        i.viewport = {w, h};
        bool viewport_moved{};
        if (i.incremental && i.computed && i.expanded_viewport != i.viewport) {
            for (auto& [id, state] : i.states) {
                if (state.viewport_units) {
                    state.expand_dirty = true;
                    viewport_moved = true;
                }
            }
        }
        const bool input_changed = !i.incremental || i.compiled->refresh_input(d, i.style_input);
        if (input_changed || viewport_moved) {
            if (i.incremental) {
                if (!i.cascade) {
                    i.cascade.emplace();
                }
                if (!i.computed) {
                    i.computed.emplace();
                }
                try {
                    if (input_changed) {
                        std::vector<NodeIdentity> changed;
                        i.compiled->update(d, *i.cascade, i.style_memo, &changed);
                        for (auto id : changed) {
                            i.states[identity(id)].expand_dirty = true;
                        }
                    }
                    i.expand_into(*i.cascade, *i.computed);
                    i.expanded_viewport = i.viewport;
                } catch (...) {
                    i.cascade.reset();
                    i.computed.reset();
                    i.style_input = {};
                    throw;
                }
            } else {
                i.computed = compute_style(d, i.sheet, scene_style_profile());
            }
            ++i.work.style_evaluations;
        } else {
            ++i.work.style_reuses;
        }
        const auto build_start = clock();
        i.work.style_ms += ms(profile_start, build_start);
        i.reused_nodes.clear();
        i.reused_styles.clear();
        i.build_into(f.root, d, *i.computed, nullptr, i.previous_dom ? &*i.previous_dom : nullptr,
                     i.previous_computed ? &*i.previous_computed : nullptr, i.previous ? &i.previous->root : nullptr);
        f.elements.clear();
        f.time_ms = time;
        f.animated = i.animated;
        for (auto it = i.states.begin(); it != i.states.end();) {
            if (!i.live.contains(it->first)) {
                it = i.states.erase(it);
            } else {
                ++it;
            }
        }
        const auto layout_start = clock();
        i.work.build_ms += ms(build_start, layout_start);
        bool reuse = i.incremental && i.previous && w == i.previous_width && h == i.previous_height &&
                     same_layout(f.root, i.previous->root);
        if (reuse) {
            copy_boxes(f.root, i.previous->root);
            f.elements = i.previous->elements;
            ++i.work.layout_reuses;
        } else {
            i.work.geometry_changed = i.work.paint_changed = true;
            i.layout_frame(f, w, h);
            ++i.work.layout_evaluations;
        }
        // The retained frame is a layout reference only. Paint-only animation does
        // not change its boxes/signature, so it needs no deep copy on a reuse.
        // Returned frames remain complete, independently owned values.
        if (!reuse) {
            // Only what the build did not take whole from the previous frame is compared and copied.
            if (i.incremental && i.previous && i.previous_computed) {
                try {
                    sort_reused(i.reused_nodes);
                    sort_reused(i.reused_styles);
                    follow(i.previous->root, f.root, i.reused_nodes);
                    i.previous->elements = f.elements;
                    i.previous->animated = f.animated;
                    i.previous->time_ms = f.time_ms;
                    follow(*i.previous_computed, *i.computed, i.reused_styles);
#ifndef NDEBUG
                    if (!scene_paint_source_equal(i.previous->root, f.root) || !(*i.previous_computed == *i.computed)) {
                        throw std::logic_error("WEFT_SCENE_CORRUPTION: previous frame differs from the update");
                    }
#endif
                } catch (...) {
                    i.previous.reset();
                    i.previous_computed.reset();
                    throw;
                }
            } else {
                i.previous = f;
                i.previous_computed = i.computed;
            }
            // Only subtrees whose revision moved are copied.
            if (i.previous_dom) {
                try {
                    dom::update_copy(*i.previous_dom, d);
                } catch (...) {
                    i.previous_dom.reset();
                    throw;
                }
            } else {
                i.previous_dom = d;
            }
        }
        i.previous_width = w;
        i.previous_height = h;
        i.work.layout_ms += ms(layout_start, clock());
    }
} // namespace weft
