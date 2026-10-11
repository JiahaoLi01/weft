#include "scene/scene.hpp"
#include "base/css_scalar.hpp"
#include "base/owner_thread.hpp"
#include "constants/scene.hpp"
#include "layout/flex.hpp"
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <limits>
#include <optional>
#include <sstream>
#include <charconv>
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
        constexpr Edges margin_edges{PropertyId::MarginTop, PropertyId::MarginRight, PropertyId::MarginBottom,
                                     PropertyId::MarginLeft};
        constexpr Edges padding_edges{PropertyId::PaddingTop, PropertyId::PaddingRight, PropertyId::PaddingBottom,
                                      PropertyId::PaddingLeft};
        constexpr Edges border_width_edges{PropertyId::BorderTopWidth, PropertyId::BorderRightWidth,
                                           PropertyId::BorderBottomWidth, PropertyId::BorderLeftWidth};
        constexpr Edges border_color_edges{PropertyId::BorderTopColor, PropertyId::BorderRightColor,
                                           PropertyId::BorderBottomColor, PropertyId::BorderLeftColor};
        constexpr Edges border_style_edges{PropertyId::BorderTopStyle, PropertyId::BorderRightStyle,
                                           PropertyId::BorderBottomStyle, PropertyId::BorderLeftStyle};
        constexpr Edges inset_edges{PropertyId::Top, PropertyId::Right, PropertyId::Bottom, PropertyId::Left};
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
                                                    PropertyId::PointerEvents};

        bool specified(const ComputedStyle& c, PropertyId k) {
            return std::find(c.specified_properties.begin(), c.specified_properties.end(), property_name(k)) !=
                   c.specified_properties.end();
        }

        // Element attributes stay a string map.
        std::string get(const std::map<std::string, std::string>& s, const std::string& k, const char* fallback = "") {
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

        Style expand(Style s) {
            if (s.contains(PropertyId::Rotate)) {
                s[PropertyId::Transform] =
                    "rotate(" + s.at(PropertyId::Rotate) + ") " +
                    (get(s, PropertyId::Transform) == "none" ? std::string{} : get(s, PropertyId::Transform));
                s.erase(PropertyId::Rotate);
            }
            if (s.contains(PropertyId::Background)) {
                const auto v = s.at(PropertyId::Background);
                s[PropertyId::BackgroundImage] = v.find("gradient(") != v.npos ? v : "none";
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
                    auto style = expand(declarations(body.substr(start + 1, end - start - 1)));
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
    } // namespace

    StyleProfile scene_style_profile() {
        StyleProfile p;
        p.scene_syntax = true;
        p.single_value_inset = true;
        p.four_value_edges = true;
        p.ua_stylesheet =
            "div{display:block}span,b,strong{display:inline}svg{display:inline}*{box-sizing:border-box;border-style:solid;border-width:0}";
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
                              "text-align",
                              "white-space",
                              "touch-action",
                              "user-select",
                              "will-change"}) {
            p.properties[k] = {"", false, CssGrammar::Tokens, {}};
        }
        for (const auto* k :
             {"font-size", "font-weight", "font-family", "line-height", "letter-spacing", "color", "fill",
              "fill-opacity", "fill-rule", "stroke", "stroke-width", "stroke-opacity", "stroke-linecap",
              "stroke-linejoin", "stroke-dasharray", "stroke-dashoffset", "text-anchor", "dominant-baseline"}) {
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
        std::optional<DomSnapshot> style_input;
        std::optional<ComputedStyle> cascade;
        std::optional<ComputedStyle> computed;
        std::optional<SceneFrame> previous;
        std::optional<DomSnapshot> previous_dom;
        std::optional<ComputedStyle> previous_computed;
        float previous_width{};
        float previous_height{};
        Keyframes keyframes;
        InterpolationCache interpolation;
        PaintCache paint_cache;
        SceneMeasure measure;
        std::map<SceneId, State> states;
        std::set<SceneId> live;
        double now = -1;
        bool animated{};

        // Cascade output remains canonical. Shorthand expansion is a separate
        // owner derivation, so it cannot turn memo hits into full property copies.
        void expand_into(const ComputedStyle& raw, ComputedStyle& out) {
            auto& state = states[identity(raw.identity)];
            if (out.identity != raw.identity || state.expand_dirty) {
                out.values = expand(raw.values);
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

        bool same_computed(const ComputedStyle& a, const ComputedStyle& b) const {
            if (a.identity != b.identity || a.local_name != b.local_name || a.values != b.values ||
                a.specified_properties != b.specified_properties || a.children.size() != b.children.size()) {
                return false;
            }
            for (std::size_t n = 0; n < a.children.size(); ++n) {
                if (!same_computed(a.children[n], b.children[n])) {
                    return false;
                }
            }
            return true;
        }

        bool static_subtree(const SceneNode& n) const {
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
            if (incremental && old_dom && old_computed && old_node && d == *old_dom &&
                same_inheritance(parent, old_parent) && same_computed(c, *old_computed) && static_subtree(*old_node)) {
                keep_live(*old_node);
                ++work.static_subtree_reuses;
                if (!scene_paint_source_equal(output, *old_node)) {
                    work.geometry_changed |= !scene_geometry_equal(output, *old_node);
                    work.paint_changed = true;
                    output = *old_node;
                }
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
            out.box = output.box;
            out.identity = d.identity;
            out.tag = d.local_name;
            out.space = d.namespace_uri;
            out.attributes = d.attributes;
            if (reuse_style) {
                out.style = std::move(output.style);
            } else {
                out.style = incremental ? c.values : expand(c.values);
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
            work.geometry_changed |= !scene_geometry_own_equal(output, out, reuse_style);
            work.paint_changed |= output.identity != out.identity || output.tag != out.tag ||
                                  output.space != out.space || output.text != out.text ||
                                  output.attributes != out.attributes || (!reuse_style && output.style != out.style) ||
                                  bool(output.paint) != bool(out.paint) ||
                                  (output.paint && out.paint && output.paint->values != out.paint->values);
            auto retained = std::move(output.children);
            std::size_t ci{};
            const bool mixed = std::any_of(d.children.begin(), d.children.end(),
                                           [](const auto& n) {
                                               return n.type == constants::element_node_type;
                                           }) &&
                               !trim(out.text).empty();
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
                } else if (mixed && child.type == constants::text_node_type && !trim(child.data).empty()) {
                    SceneNode text;
                    text.identity = child.identity;
                    text.tag = "#text";
                    text.text = css::collapse_spaces(child.data);
                    text.style = out.style;
                    text.style[PropertyId::Display] = "inline";
                    text.style[PropertyId::Position] = "static";
                    text.style.erase(PropertyId::Width);
                    text.style.erase(PropertyId::Height);
                    text.style[PropertyId::BackgroundColor] = "transparent";
                    for (std::size_t side = 0; side < padding_edges.size(); ++side) {
                        text.style[padding_edges[side]] = "0";
                        text.style[border_width_edges[side]] = "0";
                        text.style[margin_edges[side]] = "0";
                    }
                    text.paint.reset();
                    out.children.push_back(std::move(text));
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

        // The boxes of the shared Yoga adapter: each HTML element and anonymous text run. An svg element is a
        // replaced box; the scene draws its content in its own coordinates.
        static void collect_boxes(SceneNode& n, std::size_t parent, std::vector<LayoutBox>& boxes,
                                  std::vector<SceneNode*>& targets) {
            for (const auto k : border_style_edges) {
                if (n.style.text(k, "solid") != "solid") {
                    fail("scene border requires solid style");
                }
            }
            // The scene scrolls only boxes whose content fits (checked after layout).
            const auto overflow = n.style.text(PropertyId::Overflow, "visible");
            if (overflow != "visible" && overflow != "hidden" && overflow != "auto") {
                fail("unsupported scene value overflow:" + std::string(overflow));
            }
            const auto index = boxes.size();
            LayoutBox box;
            box.style = &n.style;
            box.parent = parent;
            const bool svg = n.space == constants::svg_namespace_uri;
            if (svg) {
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
            } else {
                if (!n.text.empty() && !n.children.empty()) {
                    fail("mixed inline content outside scene formatting profile");
                }
                box.text = n.children.empty() && !n.text.empty();
            }
            boxes.push_back(box);
            targets.push_back(&n);
            if (!svg) {
                for (auto& c : n.children) {
                    collect_boxes(c, index, boxes, targets);
                }
            }
        }

        // Lays out the frame's boxes in the viewport: parent-local boxes on the scene nodes and element geometry
        // in tree order.
        void layout_frame(SceneFrame& f, float w, float h) {
            std::vector<LayoutBox> boxes;
            std::vector<SceneNode*> targets;
            collect_boxes(f.root, LayoutBox::root, boxes, targets);
            // Scene text is one line at its natural width: it does not wrap.
            std::vector<std::optional<SceneTextMetrics>> metrics(boxes.size());
            const auto natural = [&](std::size_t i) -> const SceneTextMetrics& {
                if (!metrics[i]) {
                    metrics[i] = measure(targets[i]->text, targets[i]->style);
                }
                return *metrics[i];
            };
            const auto lines = [&](std::size_t i, float) {
                const auto& m = natural(i);
                return TextMetrics{m.width, m.height, m.baseline, 1};
            };
            const auto min_content = [&](std::size_t i) {
                return natural(i).width;
            };
            const auto wraps = [](std::size_t) {
                return false;
            };
            LayoutOptions options;
            options.viewport = std::array{w, h};
            const auto geometry = layout_boxes(boxes, {lines, min_content, wraps}, options);
            for (std::size_t i = 0; i < boxes.size(); ++i) {
                auto& n = *targets[i];
                const auto& g = geometry[i];
                n.box = {g.left, g.top, g.width, g.height};
                if (n.tag == "#text") {
                    continue;
                }
                ElementGeometry e;
                e.identity = n.identity;
                e.rect = {g.x, g.y, g.width, g.height};
                e.client_rects = {e.rect};
                e.client_width = g.width;
                e.client_height = g.height;
                e.scroll_width = e.client_width;
                e.scroll_height = e.client_height;
                f.elements.push_back(e);
            }
            for (std::size_t i = 1; i < boxes.size(); ++i) {
                const auto& parent = *targets[boxes[i].parent];
                if (get(parent.style, PropertyId::Overflow) != "auto") {
                    continue;
                }
                const auto& child = *targets[i];
                if (child.box[0] < -constants::scene::overflow_tolerance_px ||
                    child.box[1] < -constants::scene::overflow_tolerance_px ||
                    child.box[0] + child.box[2] > parent.box[2] + constants::scene::overflow_tolerance_px ||
                    child.box[1] + child.box[3] > parent.box[3] + constants::scene::overflow_tolerance_px) {
                    fail("scene scrolling outside supported non-overflowing auto profile");
                }
            }
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
            if (a.space != constants::svg_namespace_uri) {
                for (std::size_t i = 0; i < a.children.size(); ++i) {
                    copy_boxes(a.children[i], b.children[i]);
                }
            }
        }
    } // namespace

    SceneSolver::Work SceneSolver::work() const {
        impl_->owner.require(Diag::SceneOwner);
        return impl_->work;
    }

    SceneSolver::SceneSolver(std::string s, std::string k, SceneMeasure measure, bool incremental)
        : impl_(std::make_unique<Impl>()) {
        impl_->sheet = std::move(s);
        impl_->incremental = incremental;
        impl_->compiled = std::make_unique<CompiledStyle>(impl_->sheet, scene_style_profile());
        impl_->keyframes = parse_keyframes(std::move(k));
        impl_->measure = std::move(measure);
    }

    SceneSolver::~SceneSolver() = default;

    void SceneSolver::set_profiling(bool enabled) {
        impl_->owner.require(Diag::SceneOwner);
        impl_->profiling = enabled;
    }

    void SceneSolver::reset() {
        impl_->owner.require(Diag::SceneOwner);
        impl_->style_memo.clear();
        impl_->states.clear();
        impl_->interpolation = {};
        impl_->paint_cache = {};
        impl_->style_input.reset();
        impl_->cascade.reset();
        impl_->computed.reset();
        impl_->previous.reset();
        impl_->previous_dom.reset();
        impl_->previous_computed.reset();
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
        auto style_key = i.incremental ? i.compiled->input_key(d) : d;
        if (!i.incremental || !i.style_input || *i.style_input != style_key) {
            if (i.incremental) {
                if (!i.cascade) {
                    i.cascade.emplace();
                }
                if (!i.computed) {
                    i.computed.emplace();
                }
                try {
                    std::vector<NodeIdentity> changed;
                    i.compiled->update(d, *i.cascade, i.style_memo, &changed);
                    for (auto id : changed) {
                        i.states[identity(id)].expand_dirty = true;
                    }
                    i.expand_into(*i.cascade, *i.computed);
                } catch (...) {
                    i.cascade.reset();
                    i.computed.reset();
                    i.style_input.reset();
                    throw;
                }
            } else {
                i.computed = compute_style(d, i.sheet, scene_style_profile());
            }
            i.style_input = std::move(style_key);
            ++i.work.style_evaluations;
        } else {
            ++i.work.style_reuses;
        }
        const auto build_start = clock();
        i.work.style_ms += ms(profile_start, build_start);
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
            i.previous = f;
            i.previous_dom = d;
            i.previous_computed = i.computed;
        }
        i.previous_width = w;
        i.previous_height = h;
        i.work.layout_ms += ms(layout_start, clock());
    }
} // namespace weft
