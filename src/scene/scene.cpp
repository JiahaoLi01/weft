#include "scene/scene.hpp"
#include "scene/scene_values.hpp"
#include "constants/style.hpp"
#include "scene/scene_background.hpp"
#include "scene/scene_border_image.hpp"
#include "scene/scene_field.hpp"
#include "base/css_scalar.hpp"
#include "base/owner_thread.hpp"
#include "constants/scene.hpp"
#include "layout/flex.hpp"
#include "layout/grid.hpp"
#include "paint/particles.hpp"
#include "scene/scene_filter.hpp"
#include "scene/scene_inline.hpp"
#include "scene/scene_transform.hpp"
#include "style/background_layer.hpp"
#include "text/case_mapping.hpp"
#include <chrono>
#include <algorithm>
#include <atomic>
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
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>

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
                                                    PropertyId::TextTransform,
                                                    PropertyId::Visibility,
                                                    PropertyId::TextDecorationLine,
                                                    PropertyId::TextDecorationColor,
                                                    PropertyId::TextDecorationStyle,
                                                    PropertyId::TextDecorationThickness,
                                                    PropertyId::OverflowWrap,
                                                    PropertyId::WordBreak,
                                                    PropertyId::CaretColor,
                                                    PropertyId::BorderCollapse,
                                                    PropertyId::BorderSpacing,
                                                    PropertyId::EmptyCells,
                                                    PropertyId::StrokeMiterlimit};

        // The text a computed content value gives a generated box (CSS Generated Content 3 §1): its strings (with their
        // escapes) and attr(name), the element's attribute, in order; quotes and images give nothing, and so do counters
        // (CSS Lists 3 counters are not kept), which problem then names.
        std::string generated_text(std::string_view v, const AttributeMap& attributes, std::string& problem) {
            std::string out;
            const auto utf8 = [&](std::uint32_t cp) {
                if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                    cp = 0xFFFD;
                }
                if (cp < 0x80) {
                    out += char(cp);
                } else if (cp < 0x800) {
                    out += char(0xC0 | (cp >> 6));
                    out += char(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    out += char(0xE0 | (cp >> 12));
                    out += char(0x80 | ((cp >> 6) & 0x3F));
                    out += char(0x80 | (cp & 0x3F));
                } else {
                    out += char(0xF0 | (cp >> 18));
                    out += char(0x80 | ((cp >> 12) & 0x3F));
                    out += char(0x80 | ((cp >> 6) & 0x3F));
                    out += char(0x80 | (cp & 0x3F));
                }
            };
            std::size_t i = 0;
            while (i < v.size()) {
                const char c = v[i];
                if (c == '"' || c == '\'') {
                    for (++i; i < v.size() && v[i] != c;) {
                        if (v[i] != '\\' || i + 1 == v.size()) {
                            out += v[i++];
                            continue;
                        }
                        ++i;
                        if (std::isxdigit(static_cast<unsigned char>(v[i]))) {
                            std::uint32_t cp{};
                            for (int n = 0; n < 6 && i < v.size() && std::isxdigit(static_cast<unsigned char>(v[i]));
                                 ++n, ++i) {
                                const char h = char(std::tolower(static_cast<unsigned char>(v[i])));
                                cp = cp * 16 + std::uint32_t(h <= '9' ? h - '0' : h - 'a' + 10);
                            }
                            if (i < v.size() && v[i] == ' ') {
                                ++i;
                            }
                            utf8(cp);
                        } else if (v[i] == '\n') {
                            ++i;
                        } else {
                            out += v[i++];
                        }
                    }
                    ++i;
                } else if (v.substr(i).starts_with("counter(") || v.substr(i).starts_with("counters(")) {
                    problem = "content " + std::string(v) + " outside scene profile (counters give nothing)";
                    const auto close = v.find(')', i);
                    i = close == v.npos ? v.size() : close + 1;
                } else if (v.substr(i).starts_with("attr(")) {
                    const auto close = v.find(')', i);
                    auto name = trim(v.substr(i + 5, (close == v.npos ? v.size() : close) - i - 5));
                    name = name.substr(0, name.find_first_of(" ,"));
                    if (const auto it = attributes.find(name); it != attributes.end()) {
                        out += it->second;
                    }
                    i = close == v.npos ? v.size() : close + 1;
                } else {
                    ++i;
                }
            }
            return out;
        }

        // The DOM stand-in of a generated box (ComputedStyle::generated) for a scene build: an element of its local
        // name and identity whose one child is the text its content gives, of the element's attributes for attr(), and
        // what of its content the scene does not take (generated_text).
        DomSnapshot generated_dom(const ComputedStyle& g, const AttributeMap& attributes, std::string& problem) {
            DomSnapshot n;
            n.identity = g.identity;
            n.type = constants::element_node_type;
            n.local_name = g.local_name;
            n.namespace_uri = constants::xhtml_namespace_uri;
            const auto* content = g.values.find(PropertyId::Content);
            if (auto text = generated_text(content ? content->text() : std::string_view{}, attributes, problem);
                !text.empty()) {
                DomSnapshot t;
                t.identity = g.identity;
                t.identity.view_epoch |= generated_text_bit;
                t.type = constants::text_node_type;
                t.data = std::move(text);
                n.children.push_back(std::move(t));
            }
            return n;
        }

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

        // A value with its em lengths as px of em_px, and its rem lengths as px of the default font size: a number
        // that starts no word (not a digit of a hex color or of a name) and ends in em or rem. Every other part stays.
        std::string resolve_ems(std::string_view v, double em_px) {
            const auto digit = [&](std::size_t k) {
                return k < v.size() && v[k] >= '0' && v[k] <= '9';
            };
            const auto word = [&](std::size_t k) {
                const char ch = v[k];
                return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' ||
                       ch == '_' || ch == '#' || ch == '.';
            };
            std::string out;
            std::size_t done{};
            for (std::size_t i = 0; i < v.size(); ++i) {
                if (i && word(i - 1)) {
                    continue;
                }
                std::size_t k = i;
                if (v[k] == '-' || v[k] == '+') {
                    ++k;
                }
                const std::size_t digits = k;
                while (digit(k)) {
                    ++k;
                }
                if (k < v.size() && v[k] == '.' && digit(k + 1)) {
                    ++k;
                    while (digit(k)) {
                        ++k;
                    }
                }
                if (k == digits || (k == digits + 1 && v[digits] == '.')) {
                    continue;
                }
                const bool rem = v.substr(k, 3) == "rem";
                const auto unit = rem ? 3 : v.substr(k, 2) == "em" ? 2 : 0;
                if (!unit || (k + unit < v.size() && word(k + unit) && v[k + unit] != '.')) {
                    i = k - 1;
                    continue;
                }
                const double n = number(v.substr(i, k - i));
                out.append(v.substr(done, i - done));
                out += numeric(n * (rem ? constants::default_font_size_px : em_px)) + "px";
                done = k + unit;
                i = done - 1;
            }
            if (!done) {
                return std::string(v);
            }
            out.append(v.substr(done));
            return out;
        }

        // Whether a value holds a math function the layout and paint code read only once it is reduced.
        bool math_functions(std::string_view value) {
            return value.find("calc(") != value.npos || value.find("min(") != value.npos ||
                   value.find("max(") != value.npos || value.find("clamp(") != value.npos;
        }

        // The font size of a resolved style in px: the default without one.
        double font_px(const Style* style) {
            if (!style) {
                return constants::default_font_size_px;
            }
            return length(style->text(PropertyId::FontSize, constants::default_font_size));
        }

        // A style's em and rem lengths as px, as CSS computes them: its font size's of the parent's font size (a
        // percentage too), every other length's of its own; rem lengths of the default font size. Custom properties
        // and font families stay as they are.
        void resolve_font_relative(Style& style, const Style* parent) {
            if (const auto* size = style.find(PropertyId::FontSize)) {
                const auto text = trim(size->text());
                if (text.ends_with('%')) {
                    style.set(PropertyId::FontSize, numeric(number(text.substr(0, text.size() - 1)) /
                                                            constants::percent_scale * font_px(parent)) +
                                                        "px");
                } else if (text.find("em") != text.npos) {
                    style.set(PropertyId::FontSize, resolve_ems(text, font_px(parent)));
                }
            }
            const double own = font_px(&style);
            std::vector<std::pair<PropertyId, std::string>> resolved;
            for (const auto& [property, value] : style) {
                if (property == PropertyId::FontSize || property == PropertyId::FontFamily ||
                    value.text().find("em") == std::string_view::npos) {
                    continue;
                }
                auto text = resolve_ems(value.text(), own);
                if (text != value.text()) {
                    resolved.emplace_back(property, std::move(text));
                }
            }
            for (auto& [property, text] : resolved) {
                if (math_functions(text)) {
                    text = simplify_math_text(text);
                }
                style.set(property, std::move(text));
            }
        }

        std::vector<std::string> split(std::string_view s, char delimiter = ' ') {
            return css::split(s, delimiter).parts;
        }

        Style declarations(std::string_view s, std::vector<std::string>* dropped = nullptr) {
            Style out;
            for (auto part : split(s, ';')) {
                auto p = part.find(':');
                if (p == part.npos) {
                    if (dropped) {
                        dropped->push_back("keyframes: a declaration without a colon (" + part + ")");
                        continue;
                    }
                    fail("declaration colon");
                }
                const auto name = trim(part.substr(0, p));
                const auto id = find_property(name);
                if (!id) {
                    if (dropped) {
                        dropped->push_back("keyframes: the scene does not animate " + name);
                        continue;
                    }
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

        // The longhands of a background shorthand of one layer (style/background_layer.hpp), each its part of the
        // value or its initial value, as the style module expands it in a document's styles.
        void expand_background(Style& s, const std::string& value) {
            // The value's components: those between spaces, a '/' outside parentheses one of its own.
            std::string spaced;
            int depth{};
            for (const char ch : value) {
                depth += ch == '(' ? 1 : ch == ')' ? -1 : 0;
                spaced += ch == '/' && !depth ? std::string(" / ") : std::string(1, ch);
            }
            const auto texts = split(spaced);
            std::vector<std::string> names;
            names.reserve(texts.size());
            std::vector<BackgroundComponent> components;
            for (const auto& t : texts) {
                using Kind = BackgroundComponent::Kind;
                const auto open = t.find('(');
                std::string name = t.substr(0, open == t.npos ? t.size() : open);
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
                    return char(std::tolower(ch));
                });
                names.push_back(std::move(name));
                const char first = t.empty() ? '\0' : t[0];
                const bool numeric = std::isdigit(static_cast<unsigned char>(first)) || first == '.' ||
                                     ((first == '-' || first == '+') && t.size() > 1 &&
                                      (std::isdigit(static_cast<unsigned char>(t[1])) || t[1] == '.'));
                const auto kind = t == "/"                                      ? Kind::Slash
                                  : first == '#'                                ? Kind::Hash
                                  : open != t.npos && t.ends_with(')')          ? Kind::Function
                                  : numeric                                     ? Kind::Length
                                  : std::all_of(t.begin(), t.end(), ident_char) ? Kind::Ident
                                                                                : Kind::Other;
                components.push_back({kind, names.back()});
            }
            const auto parts = background_parts(components);
            if (!parts) {
                fail("background shorthand outside one layer: " + value);
            }
            constexpr std::array<std::tuple<PropertyId, BackgroundPart, const char*>, 8> longhands{{
                {PropertyId::BackgroundColor, BackgroundPart::Color, "transparent"},
                {PropertyId::BackgroundImage, BackgroundPart::Image, "none"},
                {PropertyId::BackgroundPosition, BackgroundPart::Position, "0% 0%"},
                {PropertyId::BackgroundSize, BackgroundPart::Size, "auto"},
                {PropertyId::BackgroundRepeat, BackgroundPart::Repeat, "repeat"},
                {PropertyId::BackgroundAttachment, BackgroundPart::Attachment, "scroll"},
                {PropertyId::BackgroundOrigin, BackgroundPart::Origin, "padding-box"},
                {PropertyId::BackgroundClip, BackgroundPart::Clip, "border-box"},
            }};
            for (const auto& [id, part, initial] : longhands) {
                std::string taken;
                for (std::size_t i = 0; i < parts->size(); ++i) {
                    const auto p = (*parts)[i];
                    if (p == part || (p == BackgroundPart::OriginAndClip &&
                                      (part == BackgroundPart::Origin || part == BackgroundPart::Clip))) {
                        taken += (taken.empty() ? "" : " ") + texts[i];
                    }
                }
                s[id] = taken.empty() ? std::string(initial) : taken;
            }
        }

        // A scene element's computed values from its cascaded ones: shorthands and aliases expanded, viewport
        // lengths in px of `viewport`. `viewport_units` is set when some value used one.
        Style expand(Style s, std::array<float, 2> viewport, bool* viewport_units = nullptr) {
            // The individual transform properties (CSS Transforms 2) apply before transform, translate first, then
            // rotate, then scale; each folds into transform's functions. A third value (z) of translate and scale is
            // left out (the scene's transforms are flat).
            if (s.contains(PropertyId::Translate) || s.contains(PropertyId::Rotate) || s.contains(PropertyId::Scale)) {
                std::string functions;
                const auto take = [&](PropertyId id) {
                    std::vector<std::string> parts;
                    if (s.contains(id)) {
                        const auto text = css::trim(s.at(id));
                        s.erase(id);
                        if (text != "none") {
                            parts = split(text);
                        }
                    }
                    return parts;
                };
                if (const auto t = take(PropertyId::Translate); !t.empty()) {
                    functions += "translate(" + t[0] + ", " + (t.size() > 1 ? t[1] : std::string("0px")) + ") ";
                }
                if (const auto r = take(PropertyId::Rotate); !r.empty()) {
                    // rotate: <angle>, or z <angle> about the z axis.
                    functions += "rotate(" + r.back() + ") ";
                }
                if (const auto sc = take(PropertyId::Scale); !sc.empty()) {
                    const auto factor = [](const std::string& v) {
                        return v.ends_with('%') ? numeric(std::stod(v.substr(0, v.size() - 1)) / 100) : v;
                    };
                    functions += "scale(" + factor(sc[0]) + ", " + factor(sc.size() > 1 ? sc[1] : sc[0]) + ") ";
                }
                if (!functions.empty()) {
                    const auto transform = get(s, PropertyId::Transform, "none");
                    s[PropertyId::Transform] =
                        css::trim(functions + (transform == "none" ? std::string{} : std::string(transform)));
                }
            }
            if (s.contains(PropertyId::Background)) {
                expand_background(s, s.at(PropertyId::Background));
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
            // an axis that clips is auto, so auto wins; scroll and clip stay.
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
                    // Math functions over the lengths, now in px, reduce to them (clamp(420px, 40vw, 640px)).
                    if (math_functions(*px)) {
                        *px = simplify_math_text(*px);
                    }
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

        // A property's value t of the way from a to b: lerp_value, but visibility is visible all the way between its
        // ends when either end is (CSS Display 3 §4), so what fades in or out with it shows through the fade.
        std::string lerp_property(std::string_view property, const std::string& a, const std::string& b, double t,
                                  InterpolationCache* cache = nullptr) {
            if (property == "visibility" && a != b && (a == "visible" || b == "visible")) {
                return t <= 0 ? a : t >= 1 ? b : "visible";
            }
            return lerp_value(a, b, t, cache);
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

        // One item of an element's transition lists: what it transitions (a property name or all) with its
        // duration and delay (ms) and timing function.
        struct TransitionItem {
            std::string property;
            double duration{};
            double delay{};
            std::string timing;
        };

        // The transitions of a style's transition longhands (CSS Transitions 1 §2): one for each item of
        // transition-property, with the duration, delay and timing function of its index, each of those lists
        // repeated as often as it takes to be as long (excess items are not used); none for transition-property: none.
        std::vector<TransitionItem> transition_items(const Style& s) {
            const auto* property = s.find(PropertyId::TransitionProperty);
            const auto* durations = s.find(PropertyId::TransitionDuration);
            const auto* delays = s.find(PropertyId::TransitionDelay);
            if (!durations && !delays) {
                // Every transition takes 0s and starts at once: none starts.
                return {};
            }
            const auto list = [](const CssValue* value, const char* initial) {
                auto items = value ? split(value->text(), ',') : std::vector<std::string>{};
                if (items.empty()) {
                    items.emplace_back(initial);
                }
                return items;
            };
            const auto names = list(property, "all");
            const auto duration_list = list(durations, "0s");
            const auto delay_list = list(delays, "0s");
            const auto timing_list = list(s.find(PropertyId::TransitionTimingFunction), "ease");
            std::vector<TransitionItem> items;
            for (std::size_t i = 0; i < names.size(); ++i) {
                auto name = names[i];
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
                    return char(std::tolower(ch));
                });
                if (name == "none") {
                    if (names.size() > 1) {
                        fail("transition-property: none in a list of properties");
                    }
                    return {};
                }
                TransitionItem item{std::move(name), duration(duration_list[i % duration_list.size()]),
                                    duration(delay_list[i % delay_list.size()]), timing_list[i % timing_list.size()]};
                if (item.duration < 0) {
                    fail("transition-duration: a negative duration " + duration_list[i % duration_list.size()]);
                }
                // An unsupported timing function is the element's error when its transitions start, not when one
                // runs.
                (void)easing(item.timing, 0);
                items.push_back(std::move(item));
            }
            return items;
        }

        // Whether a transition-property name covers a longhand: all, the longhand, or a shorthand of it (CSS
        // Transitions 1 §2.1: a shorthand starts the transitions of its longhands) — Tailwind's transition-colors names
        // border-color, and the scene has only the side colors.
        bool transition_covers(std::string_view name, std::string_view property) {
            if (name == "all" || name == property) {
                return true;
            }
            if (name == "margin" || name == "padding" || name == "background") {
                return property.size() > name.size() && property.starts_with(name) && property[name.size()] == '-';
            }
            if (name == "inset") {
                return property == "top" || property == "right" || property == "bottom" || property == "left";
            }
            if (name == "gap") {
                return property == "row-gap" || property == "column-gap";
            }
            if (name == "border-radius") {
                return property.starts_with("border-") && property.ends_with("-radius");
            }
            // border, border-<side> and border-width / -style / -color: the sides' widths, styles and colors.
            constexpr std::string_view border = "border-";
            if (name != "border" && !name.starts_with(border)) {
                return false;
            }
            const auto rest = name == "border" ? std::string_view{} : name.substr(border.size());
            for (const std::string_view side : {"top", "right", "bottom", "left"}) {
                for (const std::string_view part : {"width", "style", "color"}) {
                    if (property == std::string(border) + std::string(side) + "-" + std::string(part) &&
                        (rest.empty() || rest == side || rest == part)) {
                        return true;
                    }
                }
            }
            return false;
        }

        // The item that transitions a property: the last that names it, a shorthand of it or all.
        const TransitionItem* transition_item(const std::vector<TransitionItem>& items, std::string_view property) {
            for (auto i = items.rbegin(); i != items.rend(); ++i) {
                if (transition_covers(i->property, property)) {
                    return &*i;
                }
            }
            return nullptr;
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

        // dropped (a forgiving page): what the scene cannot take is dropped with a note there, each @keyframes rule
        // on its own (alone: the text is one rule, parsed in place).
        Keyframes parse_keyframes(std::string text, std::vector<std::string>* dropped = nullptr, bool alone = false) {
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
                if (dropped && !alone) {
                    // A forgiving page: a keyframes rule the scene cannot take is dropped, the others stay.
                    std::vector<std::string> inner;
                    try {
                        auto one = parse_keyframes("@keyframes " + name + "{" + body + "}", &inner, true);
                        for (auto& [k, frames] : one) {
                            out[k] = std::move(frames);
                        }
                    } catch (const std::invalid_argument& error) {
                        dropped->push_back("@keyframes " + name + " dropped: " + error.what());
                    }
                    dropped->insert(dropped->end(), inner.begin(), inner.end());
                    p = e;
                    continue;
                }
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
                    auto style =
                        expand(declarations(body.substr(start + 1, end - start - 1), dropped), {}, &viewport_units);
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

        using SceneId = std::array<std::uint64_t, 3>;

        struct State {
            Style target;
            Style current;
            // A generated box's DOM stand-in (generated_dom) the last build read: what its next refresh compares with.
            std::optional<DomSnapshot> stand_in;
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

            // A particle emission the element shows (SceneSolver::set_particles): a -weft-particles stream from its
            // start until its stop, or a burst, at the frame times in ms; its seed; and where a burst flies: a target
            // element's centre or a viewport point, which the layout of its first frame places in the element's border
            // box px.
            struct Emission {
                std::shared_ptr<const SceneEmitter> emitter;
                paint::ParticleEmission::Kind kind{};
                double start{};
                std::optional<double> stop;
                std::uint32_t count{};
                float duration{};
                std::uint32_t seed{};
                std::optional<NodeIdentity> target;
                std::optional<std::array<float, 2>> point;
                bool placed{};
                std::optional<std::array<float, 2>> local;
            };

            // Emissions until their particles die, and how many the element has started (their seeds).
            std::vector<Emission> emissions;
            std::uint32_t emitted{};

            // The incremental build (SceneSolver::Impl::refresh): the parent element (zeros for the root), whether the
            // element showed (no display: none on it or an ancestor), and the style its children inherit when its build
            // gave it another font weight after them (a b or strong element).
            SceneId parent{};
            bool shown = true;
            std::optional<Style> children_style;
        };

        SceneId identity(NodeIdentity n) {
            return {n.view_epoch, n.slot, n.generation};
        }

        // The seed of an element's emission: its identity and how many emissions it started before, mixed.
        std::uint32_t emission_seed(const SceneId& id, std::uint32_t emitted) {
            std::uint64_t h = id[0] * 0x9e3779b97f4a7c15ull ^ id[1] * 0xc2b2ae3d27d4eb4full ^
                              id[2] * 0x165667b19e3779f9ull ^ emitted * 0xd6e8feb86659fd93ull;
            h ^= h >> 31;
            h *= 0xbf58476d1ce4e5b9ull;
            h ^= h >> 29;
            return std::uint32_t(h);
        }

        // Whether two styles lay out alike: they differ in paint-only properties alone (below the solver).
        bool same_layout_style(const Style& a, const Style& b);
    } // namespace

    std::vector<std::size_t> scene_child_order(const SceneNode& n) {
        std::vector<std::size_t> out(n.children.size());
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = i;
        }
        if (!scene_orders_children(n)) {
            return out;
        }
        std::vector<int> order(out.size());
        bool reordered{};
        for (std::size_t i = 0; i < out.size(); ++i) {
            const auto& c = n.children[i];
            const auto* v = c.style.find(PropertyId::Order);
            const auto position = c.style.text(PropertyId::Position, "static");
            if (!v || position == "absolute" || position == "fixed") {
                continue;
            }
            const auto value = css::parse_integer(trim(v->text()));
            if (!value) {
                fail("order " + v->text() + " outside scene profile");
            }
            order[i] = *value;
            reordered = reordered || *value != 0;
        }
        if (reordered) {
            std::stable_sort(out.begin(), out.end(), [&](std::size_t a, std::size_t b) {
                return order[a] < order[b];
            });
        }
        return out;
    }

    StyleProfile scene_style_profile(bool forgiving) {
        StyleProfile p;
        p.scene_syntax = true;
        p.forgiving = forgiving;
        p.single_value_inset = true;
        p.four_value_edges = true;
        p.ua_stylesheet =
            "html,body,div,p,h1,h2,h3,h4,h5,h6,ul,ol,li,dl,dt,dd,section,article,header,footer,nav,main,aside,form,"
            "blockquote,"
            "figure,figcaption,address,hr{display:block}button{display:inline-block;text-align:center}"
            "span,b,strong{display:inline}"
            "svg{display:inline}*{box-sizing:border-box;border-style:solid;border-width:0}rp{display:none}"
            "rt{line-height:normal}";
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
                              "translate",
                              "scale",
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
                              "justify-self",
                              "grid-template-columns",
                              "grid-template-rows",
                              "grid-auto-columns",
                              "grid-auto-rows",
                              "grid-auto-flow",
                              "grid-row-start",
                              "grid-row-end",
                              "grid-column-start",
                              "grid-column-end",
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
                              "background-repeat",
                              "background-attachment",
                              "background-origin",
                              "background-clip",
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
                              "-weft-particles",
                              "backdrop-filter",
                              "overflow-x",
                              "overflow-y",
                              "text-overflow",
                              "filter",
                              "vertical-align",
                              "will-change",
                              "order",
                              "table-layout",
                              "clip-path",
                              "-webkit-clip-path"}) {
            p.properties[k] = {"", false, CssGrammar::Tokens, {}};
        }
        // Inherited. The text-decoration longhands too: the scene propagates an element's decorations to its
        // descendants' text by inheritance.
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
                              "text-transform",
                              "text-shadow",
                              "visibility",
                              "ruby-position",
                              "ruby-align",
                              "text-decoration-line",
                              "text-decoration-color",
                              "text-decoration-style",
                              "text-decoration-thickness",
                              "overflow-wrap",
                              "word-break",
                              "caret-color",
                              "border-collapse",
                              "border-spacing",
                              "empty-cells",
                              "stroke-miterlimit"}) {
            p.properties[k] = {"", true, CssGrammar::Tokens, {}};
        }
        // Tables (lay_out_table).
        p.properties["table-layout"].initial = "auto";
        p.properties["border-collapse"].initial = "separate";
        p.properties["border-spacing"].initial = "0";
        p.properties["empty-cells"].initial = "show";
        // Validated where an element has them (build_own): clip-path none only; stroke-miterlimit the stroker's 4.
        p.properties["clip-path"].initial = "none";
        p.properties["-webkit-clip-path"].initial = "none";
        p.properties["stroke-miterlimit"].initial = "4";
        p.properties["caret-color"].initial = "auto";
        p.properties["visibility"].initial = "visible";
        p.properties["pointer-events"] = {"auto", true, CssGrammar::Tokens, {}};
        p.properties["font-size"].initial = constants::default_font_size;
        p.properties["color"].initial = constants::default_color;
        p.properties["fill"].initial = constants::default_color;
        p.properties["stroke"].initial = "none";
        p.properties["transition-property"].initial = "all";
        p.properties["transition-duration"].initial = "0s";
        p.properties["transition-delay"].initial = "0s";
        p.properties["transition-timing-function"].initial = "ease";
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
        // border-image (CSS Backgrounds 3 §6): a url() image sliced in nine and drawn over the border area instead of
        // the border styles (scene_border_image.hpp).
        p.properties["border-image-source"] = {"none", false, CssGrammar::Tokens, {}};
        p.properties["border-image-slice"] = {"100%", false, CssGrammar::Tokens, {}};
        p.properties["border-image-width"] = {"1", false, CssGrammar::Tokens, {}};
        p.properties["border-image-outset"] = {"0", false, CssGrammar::Tokens, {}};
        p.properties["border-image-repeat"] = {"stretch", false, CssGrammar::Tokens, {}};
        // outline (CSS UI 4 §5): a ring outside the border box over the content (scene_values::outline); no layout.
        p.properties["outline-width"] = {"medium", false, CssGrammar::Tokens, {}};
        p.properties["outline-style"] = {"none", false, CssGrammar::Tokens, {}};
        p.properties["outline-color"] = {"currentcolor", false, CssGrammar::Tokens, {}};
        p.properties["outline-offset"] = {"0", false, CssGrammar::Tokens, {}};
        // content: the text of a ::before or ::after box (generated_text); normal and none make no box.
        p.properties["content"] = {"normal", false, CssGrammar::Tokens, {}};
        if (forgiving) {
            // A document's head and its non-rendered elements, as a browser's UA sheet hides them.
            p.ua_stylesheet += "head,style,script,title,meta,link,template,noscript,base{display:none}";
            // Chrome's focus ring where an element shows its focus (-webkit-focus-ring-color of a light scheme).
            p.ua_stylesheet += ":focus-visible{outline:auto 1px #101010}";
            // Text fields as Chrome's UA sheet styles them (html.css): an input of a text type and a textarea (an
            // inset border draws as Chrome's native text field), dimmed when disabled, the focus ring on the border
            // box, and the placeholder's gray.
            p.ua_stylesheet +=
                "input:is(:not([type]),[type=text i],[type=search i],[type=password i],[type=email i],[type=url i],"
                "[type=tel i],[type=number i]),textarea{display:inline-block;box-sizing:content-box;margin:0;"
                "font-family:Arial;font-size:13.333333px;font-weight:400;font-style:normal;line-height:normal;"
                "letter-spacing:normal;text-transform:none;text-shadow:none;text-align:left;color:#000;"
                "background-color:#fff;cursor:text;padding:1px 2px;border:2px inset #767676}"
                "textarea{padding:2px;border-width:1px;font-family:monospace;white-space:pre-wrap;"
                "overflow-wrap:break-word;overflow:auto}"
                "input:disabled,textarea:disabled{color:#545454;background-color:rgba(239,239,239,0.3);"
                "border-color:rgba(118,118,118,0.3);cursor:default}"
                "input:focus-visible,textarea:focus-visible{outline-offset:0}"
                "::placeholder{color:#757575}";
            // Hints that change nothing a scene draws: dropped without a warning.
            p.ignored = {
                "-webkit-font-smoothing", "-moz-osx-font-smoothing", "text-rendering", "-webkit-text-size-adjust",
                "-moz-text-size-adjust", "text-size-adjust", "tab-size", "-moz-tab-size", "-webkit-tap-highlight-color",
                "font-feature-settings", "font-variation-settings", "font-synthesis", "font-kerning",
                "font-optical-sizing", "appearance", "-webkit-appearance", "-moz-appearance", "resize", "accent-color",
                "scrollbar-width", "scrollbar-color", "scrollbar-gutter", "scroll-behavior", "overscroll-behavior",
                "overscroll-behavior-x", "overscroll-behavior-y", "-webkit-overflow-scrolling", "print-color-adjust",
                "-webkit-print-color-adjust", "color-scheme", "forced-color-adjust", "caption-side",
                // Counters draw nothing themselves; counter() in content is a diagnostic (generated_text).
                "counter-reset", "counter-increment", "counter-set", "list-style", "list-style-type",
                "list-style-position", "list-style-image", "quotes", "hyphens", "-webkit-hyphens", "orphans", "widows",
                "break-inside", "break-before", "break-after", "page-break-inside", "box-decoration-break",
                "-webkit-box-decoration-break", "contain", "content-visibility", "contain-intrinsic-size",
                "-webkit-backdrop-filter", "speak", "text-indent", "-webkit-text-decoration", "unicode-bidi",
                "direction", "writing-mode", "image-rendering", "shape-rendering", "text-wrap", "text-wrap-mode",
                "text-wrap-style", "font-display", "src", "unicode-range"};
            p.ignored_prefixes = {"-ms-", "-moz-", "-o-"};
        }
        return p;
    }

    namespace {
        // The next SceneFrame::revision of any solver.
        std::atomic<std::uint64_t> next_revision{1};
    } // namespace

    struct SceneSolver::Impl {
        const OwnerThread owner;
        std::string sheet;
        std::unique_ptr<CompiledStyle> compiled;
        StyleMemo style_memo;
        bool incremental = true;
        // A forgiving page (StyleProfile::forgiving), and what the solver itself dropped since the last take.
        bool forgiving{};
        // The document an update reads, while it builds (url() filters name its elements).
        const DomSnapshot* document{};
        std::vector<std::string> warnings;
        std::set<std::string> warned;
        bool profiling = true;
        SceneSolver::Work work;
        StyleInput style_input;
        std::optional<ComputedStyle> cascade;
        std::optional<ComputedStyle> computed;
        // The document the last update read (the incremental path's cascade and build compare what changed with it).
        std::optional<DomSnapshot> previous_dom;
        float previous_width{};
        float previous_height{};
        // The incremental path: the scene the last update gave, which the next one changes where its input changed,
        // sharing the rest, and its element geometry; the count of updates (ComputedStyle::stamp) and the stamps of
        // the last build and expansion; the elements whose frames follow the time (running transitions and
        // animations, particles, effects that read the time) and those the update in progress finds so. While an
        // update builds: the elements whose subtrees hold such an element or one with bursts, which it builds though
        // their DOM did not change; the elements it built; those of the subtrees it dropped; and whether what it built
        // moved the layout.
        std::shared_ptr<const SceneNode> retained;
        std::vector<ElementGeometry> retained_elements;
        std::uint64_t stamp{};
        std::uint64_t built{};
        std::uint64_t expanded{};
        std::set<SceneId> active;
        std::set<SceneId> next_active;
        std::set<SceneId> marked;
        std::set<SceneId> seen;
        std::vector<SceneId> removed;
        bool layout_dirty{};
        bool element_geometry = true;

        // Where the frame being built must be laid out again (refresh): each node whose own layout input or children
        // changed, by its child indices from the root (refresh_path while it is built), with its node in the last
        // frame (none for a new one).
        struct LayoutPoint {
            std::vector<std::uint32_t> path;
            const SceneNode* old{};
        };

        std::vector<LayoutPoint> layout_points;
        std::vector<std::uint32_t> refresh_path;
        // The revision of the content the last update gave (0 before the first; after a reset the next update lays
        // the frame out anew, a new revision).
        std::uint64_t revision{};
        // The frame's viewport, which viewport lengths resolve against, and the one the computed styles last used.
        std::array<float, 2> viewport{};
        std::array<float, 2> expanded_viewport{};
        Keyframes keyframes;
        InterpolationCache interpolation;
        PaintCache paint_cache;
        SceneText* text{};
        const SceneEffects* effects{};
        SceneImages* images{};
        const SceneParticles* particles{};

        // Bursts emit_particles asked for since the last update, by element, and how many.
        struct Burst {
            std::string name;
            SceneBurst burst;
        };

        std::map<SceneId, std::vector<Burst>> pending;
        std::size_t pending_count{};
        // While a frame is built: whether a display: none ancestor hides the element, the particles its emissions
        // draw, and the elements with bursts whose flights its layout places.
        bool hidden{};
        std::uint64_t frame_particles{};
        std::vector<SceneId> flights;
        // While a frame is laid out: the extents of the atomic inlines measured, by node and available width, and
        // the last and the first line's baseline of each inline formatting context root placed, from its border box
        // top (a table cell's baseline is its first line's).
        std::map<std::pair<const SceneNode*, float>, SceneAtomicExtent> atomic_memo;
        std::map<const SceneNode*, float> context_baselines;
        std::map<const SceneNode*, float> context_first_baselines;

        // An inline formatting context: its content (shaped and broken once), its lines by available width (the most
        // recent last) and its min-content width.
        struct InlineContext {
            std::unique_ptr<SceneInlineContent> content;
            std::vector<std::pair<float, SceneInlineLayout>> memo;
            std::optional<float> min_content;
        };

        // The contexts layout reached through their roots' storage, from frame to frame by that storage, which an
        // entry holds (so its key stays that node): equal handles hold equal subtrees (SceneChildren), so a root of the
        // same storage has the same content and lines at a width. An entry goes when nothing else holds its root.
        struct KeptContext {
            SceneChildren::Handle root;
            InlineContext context;
        };

        std::unordered_map<const SceneNode*, KeptContext> kept_contexts;

        void sweep_contexts() {
            std::erase_if(kept_contexts, [](const auto& entry) {
                return entry.second.root.use_count() <= 1;
            });
        }

        std::map<SceneId, State> states;
        std::set<SceneId> live;
        double now = -1;
        bool animated{};

        // Cascade output remains canonical. Shorthand expansion is a separate owner derivation, so it cannot turn
        // memo hits into full property copies. A subtree whose cascade did not change since the last expansion (its
        // subtree stamp) stays as it is unless `all` (the viewport moved: values with viewport lengths expand again).
        // A node whose expanded values change takes the update's stamp, as the cascade's nodes do.
        void expand_into(const ComputedStyle& raw, ComputedStyle& out, bool all = true) {
            if (!all && out.identity == raw.identity && raw.subtree_stamp <= expanded) {
                return;
            }
            auto& state = states[identity(raw.identity)];
            bool changed = out.identity != raw.identity;
            if (changed || state.expand_dirty || raw.stamp > expanded) {
                state.viewport_units = false;
                auto values = expand(raw.values, viewport, &state.viewport_units);
                state.expand_dirty = false;
                if (changed || !(values == out.values)) {
                    out.values = std::move(values);
                    changed = true;
                }
            }
            out.identity = raw.identity;
            if (out.local_name != raw.local_name) {
                out.local_name = raw.local_name;
                changed = true;
            }
            if (out.custom_properties != raw.custom_properties) {
                out.custom_properties = raw.custom_properties;
                changed = true;
            }
            if (out.invalid_at_computed_value != raw.invalid_at_computed_value) {
                out.invalid_at_computed_value = raw.invalid_at_computed_value;
                changed = true;
            }
            if (out.specified_properties != raw.specified_properties) {
                out.specified_properties = raw.specified_properties;
                changed = true;
            }
            if (changed) {
                out.stamp = stamp;
            }
            auto prior = std::move(out.children);
            out.children.clear();
            out.children.reserve(prior.capacity());
            out.subtree_stamp = out.stamp;
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
                expand_into(child, out.children.back(), all);
                out.subtree_stamp = std::max(out.subtree_stamp, out.children.back().subtree_stamp);
            }
            auto prior_generated = std::move(out.generated);
            out.generated.clear();
            for (const auto& g : raw.generated) {
                ComputedStyle mirrored;
                for (auto& p : prior_generated) {
                    if (p.identity == g.identity) {
                        mirrored = std::move(p);
                        break;
                    }
                }
                expand_into(g, mirrored, all);
                out.subtree_stamp = std::max(out.subtree_stamp, mirrored.subtree_stamp);
                out.generated.push_back(std::move(mirrored));
            }
        }

        // What an element's build decides of its children: whether it shows (no display: none on it or an
        // ancestor), and whether its text nodes are #text children (mixed content), all of them, spaces too (an inline
        // formatting context) or those with more than spaces.
        struct Built {
            bool shown{};
            bool inline_context{};
            bool mixed{};
        };

        // An element's own scene values from its DOM node d, its computed style c and its parent's style: everything
        // but its children, into out, which keeps the layout output of `before` (the element's last node, or an empty
        // one) until layout runs again. Updates the element's state (transitions, animations, particles) at the
        // frame's time, and the frame's work flags against before.
        // A note of what a forgiving page lost, once a message.
        void warn(std::string message) {
            if (warnings.size() < std::size_t(constants::style::max_style_warnings) && warned.insert(message).second) {
                warnings.push_back(std::move(message));
            }
        }

        // What the scene draws without (a value it does not take, problem): a forgiving page's warning, a strict page's
        // error. Nothing when problem is empty.
        void undrawn(std::string problem) {
            if (problem.empty()) {
                return;
            }
            if (!forgiving) {
                fail(problem);
            }
            warn(std::move(problem));
        }

        // A forgiving page's filter the scene cannot draw is dropped with a warning, as CSS drops what it does not
        // take, instead of failing every frame that shows it: one on an SVG element inside svg, and one of functions
        // the scene does not take (scene_filter), its url() filters looked up in the document the update reads.
        void drop_filter(SceneNode& out) {
            const auto filter = std::string(get(out.style, PropertyId::Filter, "none"));
            if (css::trim(filter).empty() || css::trim(filter) == "none") {
                return;
            }
            std::string why;
            if (!out.space.empty() && out.space != constants::xhtml_namespace_uri && out.tag != "svg") {
                why = "the scene draws no filter of an SVG element inside svg";
            } else if (const auto parsed = scene_filter(
                           filter, out.style, document ? scene_filter_resolver(*document) : SceneFilterResolve{});
                       !parsed) {
                // The diagnostic's message says "<code>: filter <value>: <why>".
                why = parsed.error().message;
                const auto said = "filter " + std::string(css::trim(filter)) + ": ";
                if (const auto at = why.find(said); at != std::string::npos) {
                    why = why.substr(at + said.size());
                }
            } else {
                return;
            }
            warn("filter " + filter + " on <" + out.tag + "> (" + why + ")");
            out.style.erase(PropertyId::Filter);
        }

        // A forgiving page's grid value the layout does not take (layout/grid.hpp: named lines and areas, units other
        // than px, % and fr, values out of the grammar) is dropped with a warning, as CSS drops a value it does not
        // take, instead of failing every frame that lays it out. The property takes its initial value: no explicit
        // tracks, auto tracks, row flow, automatic placement, the container's item alignment.
        void drop_grid(SceneNode& out) {
            using P = PropertyId;
            for (const auto id : {P::GridTemplateColumns, P::GridTemplateRows, P::GridAutoColumns, P::GridAutoRows,
                                  P::GridAutoFlow, P::GridRowStart, P::GridRowEnd, P::GridColumnStart, P::GridColumnEnd,
                                  P::JustifyItems, P::JustifySelf}) {
                const auto* value = out.style.find(id);
                if (!value) {
                    continue;
                }
                const auto name = property_name(id);
                try {
                    if (id == P::GridTemplateColumns || id == P::GridTemplateRows) {
                        (void)grid::template_tracks(name, value->text());
                    } else if (id == P::GridAutoColumns || id == P::GridAutoRows) {
                        (void)grid::auto_tracks(name, value->text());
                    } else if (id == P::GridAutoFlow) {
                        (void)grid::auto_flow(value->text());
                    } else if (id == P::JustifyItems || id == P::JustifySelf) {
                        (void)grid::self_alignment(name, value->text());
                    } else {
                        (void)grid::line(name, value->text());
                    }
                } catch (const std::invalid_argument& e) {
                    // The diagnostic says "<code>: <property> <value>: <why>".
                    std::string why = e.what();
                    why = why.substr(why.rfind(": ") + 2);
                    warn(std::string(name) + " " + css::trim(value->text()) + " on <" + out.tag + "> (" + why + ")");
                    out.style.erase(id);
                }
            }
        }


        // Sets a color of a node's style, and its compiled paint value when it has them.
        static void set_color(SceneNode& out, PropertyId k, int slot, const std::string& color) {
            out.style[k] = color;
            if (out.paint) {
                out.paint->values[std::size_t(slot)] = parse_scene_paint_value(color);
            }
        }

        // A length in px as style text.
        static std::string px_text(float v) {
            char buffer[32];
            const auto [end, error] = std::to_chars(buffer, buffer + sizeof buffer, v);
            return error == std::errc{} ? std::string(buffer, end) + "px" : std::string("0px");
        }

        // A text field's node (SceneNode::field, dom/text_field.hpp): its text is what the field shows (its placeholder,
        // in its ::placeholder's color, while its value is empty), on one line that does not wrap (an input's) or on
        // the lines its white-space breaks (a textarea's). An input's line stands centered in its content box, as
        // Chrome's inner editor does: the node lays out as a flex box whose justify-content follows its text-align. A
        // field of no width or height of its own takes Blink's intrinsic size: an input its size attribute's average
        // characters (20 without one) and the overhang of its widest character, a textarea its cols' average
        // characters (20) and a scrollbar's width, by its rows (2) of its line height (LayoutTextControlSingleLine and
        // -MultiLine; the average character is the font's 'x'). An inset border (the UA style sheet's) draws as Chrome's
        // native text field: a one-px ring of its color inside the border box.
        void field_own(SceneNode& out, const DomSnapshot& d, const ComputedStyle& c) {
            const auto& f = *d.field;
            auto field = std::make_shared<SceneField>();
            field->state = d.field;
            field->focused = d.focused;
            out.text = scene_field_text(f, field->placeholder);
            out.text_breaks.clear();
            // A value the field changes, its authored one kept for getComputedStyle.
            const auto keep = [&](PropertyId k) {
                if (std::none_of(field->authored.begin(), field->authored.end(), [&](const auto& a) {
                        return a.first == k;
                    })) {
                    const auto* v = out.style.find(k);
                    field->authored.emplace_back(k, v ? std::optional<std::string>(std::string(v->text()))
                                                      : std::nullopt);
                }
            };
            const auto set = [&](PropertyId k, std::string v) {
                keep(k);
                out.style[k] = std::move(v);
            };
            const ComputedStyle* placeholder_style{};
            const ComputedStyle* selection_style{};
            for (const auto& g : c.generated) {
                if (g.local_name == "::placeholder") {
                    placeholder_style = &g;
                } else if (g.local_name == "::selection") {
                    selection_style = &g;
                }
            }
            const auto own_value = [](const ComputedStyle* s, PropertyId k) {
                const auto* v = s ? s->values.find(k) : nullptr;
                auto text = v ? std::string(trim(v->text())) : std::string();
                return text == "inherit" || text == "unset" || text == "initial" ? std::string() : text;
            };
            const auto current = std::string(out.style.text(PropertyId::Color, constants::default_color));
            const auto resolve = [&](std::string color) {
                return color.empty() || color == "currentcolor" || color == "currentColor" ? current : color;
            };
            if (field->placeholder) {
                if (const auto color = own_value(placeholder_style, PropertyId::Color); !color.empty()) {
                    keep(PropertyId::Color);
                    set_color(out, PropertyId::Color, constants::scene::paint_slot_color, resolve(color));
                }
            }
            // Chrome's selection in a focused field: white text on blue, unless the field's ::selection says otherwise
            // (its background alone keeps the text's color).
            const auto background = own_value(selection_style, PropertyId::BackgroundColor);
            const auto foreground = own_value(selection_style, PropertyId::Color);
            field->selection_background =
                !background.empty()   ? resolve(background)
                : foreground.empty() ? std::string(constants::scene::field_selection_background)
                                     : std::string("transparent");
            field->selection_color = !foreground.empty() ? resolve(foreground)
                                     : background.empty() ? std::string(constants::scene::field_selection_color)
                                                          : current;
            const auto caret = std::string(trim(out.style.text(PropertyId::CaretColor, "auto")));
            field->caret_color = caret == "auto" ? current : resolve(caret);
            if (std::all_of(border_style_edges.begin(), border_style_edges.end(), [&](PropertyId k) {
                    return out.style.text(k, "solid") == "inset";
                })) {
                field->native = true;
                field->ring = resolve(std::string(trim(out.style.text(PropertyId::BorderTopColor, "currentcolor"))));
                for (std::size_t side = 0; side < border_style_edges.size(); ++side) {
                    set(border_style_edges[side], "solid");
                    keep(border_color_edges[side]);
                    set_color(out, border_color_edges[side], constants::scene::paint_slot_border_color + int(side),
                              "transparent");
                }
            }
            const bool multiline = f.multiline;
            if (!multiline) {
                set(PropertyId::WhiteSpace, "pre");
                const auto display = std::string(out.style.text(PropertyId::Display, "inline"));
                if (display != "none") {
                    const bool block = display == "block" || display == "flex" || display == "grid";
                    set(PropertyId::Display, block ? "flex" : "inline-flex");
                    set(PropertyId::FlexDirection, "row");
                    set(PropertyId::AlignItems, "center");
                    const auto align = std::string(out.style.text(PropertyId::TextAlign, "left"));
                    set(PropertyId::JustifyContent, align == "center"                    ? "center"
                                                    : align == "right" || align == "end" ? "flex-end"
                                                                                         : "flex-start");
                }
            }
            const auto count = [&](const char* name, int fallback) {
                const auto it = d.attributes.find(name);
                if (it == d.attributes.end()) {
                    return fallback;
                }
                const auto v = trim(it->second);
                int n{};
                const auto [end, error] = std::from_chars(v.data(), v.data() + v.size(), n);
                return error == std::errc{} && end == v.data() + v.size() && n > 0 ? n : fallback;
            };
            const auto font = scene_font(out.style);
            if (multiline) {
                field->newline_width = text->measure(" ", out.style).width;
            }
            const bool border_box = trim(out.style.text(PropertyId::BoxSizing, "content-box")) == "border-box";
            const auto edges = [&](PropertyId a, PropertyId b, PropertyId e, PropertyId g) {
                return border_box ? edge_length(out.style, a, 0) + edge_length(out.style, b, 0) +
                                        edge_length(out.style, e, 0) + edge_length(out.style, g, 0)
                                  : 0.f;
            };
            if (trim(out.style.text(PropertyId::Width, "auto")) == "auto") {
                const auto [x, widest] = text->field_char_widths(font);
                const float average = std::round(x);
                const float content =
                    multiline ? std::ceil(average * float(count("cols", constants::scene::field_default_cols))) +
                                    constants::scene::field_scrollbar_px
                              : std::ceil(average * float(count("size", constants::scene::field_default_size))) +
                                    std::round(widest) - average;
                out.style[PropertyId::Width] =
                    px_text(content + edges(PropertyId::PaddingLeft, PropertyId::PaddingRight,
                                            PropertyId::BorderLeftWidth, PropertyId::BorderRightWidth));
            }
            if (trim(out.style.text(PropertyId::Height, "auto")) == "auto") {
                const auto strut = text->strut(font);
                const float line = strut.above + strut.below;
                const float content =
                    multiline ? line * float(count("rows", constants::scene::field_default_rows)) : line;
                out.style[PropertyId::Height] =
                    px_text(content + edges(PropertyId::PaddingTop, PropertyId::PaddingBottom,
                                            PropertyId::BorderTopWidth, PropertyId::BorderBottomWidth));
            }
            out.field = std::move(field);
        }

        // Built::mixed and its kin, and the field's: whether a node's own values changed what its node draws.
        static bool same_field(const std::shared_ptr<const SceneField>& a, const std::shared_ptr<const SceneField>& b) {
            return a == b || (a && b && *a == *b);
        }

        Built build_own(SceneNode& out, const SceneNode& last, const DomSnapshot& d, const ComputedStyle& c,
                        const Style* parent) {
            // Layout outputs stay until layout runs again.
            out.box = last.box;
            out.lines = last.lines;
            out.fragments = last.fragments;
            out.scrolled = last.scrolled;
            out.scroll_shift = last.scroll_shift;
            out.identity = d.identity;
            out.tag = d.local_name;
            out.space = d.namespace_uri;
            out.attributes = d.attributes;
            if (auto anchoring = scene_anchoring(out.attributes); !anchoring) {
                throw DiagnosticError(anchoring.error());
            } else if (*anchoring && !out.space.empty() && out.space != constants::xhtml_namespace_uri &&
                       out.tag != "svg") {
                fail("data-anchor on an SVG element inside svg");
            } else {
                out.anchoring = *anchoring;
            }
            out.style = incremental ? c.values : expand(c.values, viewport);
            if (forgiving) {
                drop_filter(out);
                drop_grid(out);
            }
            if (const auto overflow = get(out.style, PropertyId::Overflow, "visible");
                overflow != "visible" && overflow != "clip") {
                out.scroll = {float(d.scroll_left), float(d.scroll_top)};
            } else {
                // No scroll container: nothing scrolled, whatever layout made of it before.
                out.scroll = {};
                out.scrolled = {};
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
            out.text_breaks.clear();
            for (const auto& t : d.children) {
                if (t.type == constants::text_node_type) {
                    if (!out.text.empty() && !t.data.empty()) {
                        out.text_breaks.push_back(std::uint32_t(out.text.size()));
                    }
                    out.text += t.data;
                }
            }
            if ((!c.generated.empty() || std::any_of(d.children.begin(), d.children.end(),
                                                     [](const auto& n) {
                                                         return n.type == constants::element_node_type;
                                                     })) &&
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
                // Decorations propagate (CSS Text Decoration 3 §2.1): an element of no lines of its own draws its
                // parent's, in the parent's color, style and thickness; one of its own lines starts from the initial
                // values of the decoration properties it leaves out.
                constexpr PropertyId decoration_properties[]{PropertyId::TextDecorationColor,
                                                             PropertyId::TextDecorationStyle,
                                                             PropertyId::TextDecorationThickness};
                if (specified(c, PropertyId::TextDecorationLine)) {
                    for (const auto k : decoration_properties) {
                        if (!specified(c, k)) {
                            out.style.erase(k);
                        }
                    }
                } else if (parent->text(PropertyId::TextDecorationLine, "none") != "none") {
                    for (const auto k : decoration_properties) {
                        const auto decoration = parent->text(k, "currentcolor");
                        if (k == PropertyId::TextDecorationColor &&
                            (decoration == "currentcolor" || decoration == "currentColor")) {
                            out.style.set(k, std::string(parent->text(PropertyId::Color, constants::default_color)));
                            continue;
                        }
                        if (const auto* v = parent->find(k)) {
                            out.style.set(k, *v);
                        } else {
                            out.style.erase(k);
                        }
                    }
                }
            }
            resolve_font_relative(out.style, parent);
            const auto untransformed = out.text.size();
            out.text = transformed(std::move(out.text), out.style);
            if (out.text.size() != untransformed) {
                // A case mapping that changed the length moved the boundaries; the text lays out as one run.
                out.text_breaks.clear();
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
            // CSS Transitions 1 §3, with the after-change style's transition lists (read, and so checked, whenever
            // the style changes): a changed property starts a transition when the item that names it has a combined
            // duration above 0s; a running transition ends (its property shows the target) when its property changes
            // without starting one, leaves the target or is no longer named.
            const auto items = target_changed ? transition_items(out.style) : std::vector<TransitionItem>{};
            if (!st.target.empty() && target_changed) {
                const auto& target = out.style;
                std::erase_if(st.transitions, [&](const auto& running) {
                    const auto id = find_property(running.first);
                    return !id || !target.find(*id) || !transition_item(items, running.first);
                });
                for (const auto& [property, v] : target) {
                    const auto k = property_name(property);
                    const auto* before = st.target.find(property);
                    if (!before || *before == v || k.starts_with("transition-") || k.starts_with("animation") ||
                        k.starts_with("--")) {
                        continue;
                    }
                    const auto* item = transition_item(items, k);
                    if (!item || std::max(item->duration, 0.) + item->delay <= 0) {
                        st.transitions.erase(std::string(k));
                        continue;
                    }
                    const auto* current = st.current.find(property);
                    st.transitions[std::string(k)] = {current ? current->text() : before->text(), v.text(),
                                                      item->timing, now + item->delay, item->duration};
                }
            }
            if (target_changed) {
                if (incremental) {
                    // Slots whose text stayed keep the values compiled from the last target.
                    st.paint = st.target.empty() ? compile_scene_paint_values(out.style)
                                                 : compile_scene_paint_values(out.style, st.target, st.paint);
                    ++work.paint_compilations;
                }
                st.target = out.style;
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
            // An element that does not show (display: none, or inside such an element) runs no transitions or
            // animations (CSS Transitions 1 §3, CSS Animations 1 §3): its values are its target's, and its animations
            // start over when it shows again.
            const bool showing = !hidden && get(target, PropertyId::Display) != "none";
            if (!showing) {
                st.transitions.clear();
                st.animation_value.clear();
                st.plans.clear();
                st.plans_valid = false;
            }
            for (auto i = st.transitions.begin(); i != st.transitions.end();) {
                const auto& tr = i->second;
                // A transition of 0s shows its start value through its delay, then its end value.
                const double t = tr.duration > 0 ? (now - tr.start) / tr.duration : now >= tr.start ? 1 : 0;
                if (t >= 1) {
                    i = st.transitions.erase(i);
                    continue;
                }
                animated = true;
                const auto amount = easing(tr.timing, std::max(0., t));
                out.style[i->first] =
                    lerp_property(i->first, tr.from, tr.to, amount, incremental ? &interpolation : nullptr);
                typed(i->first, tr.from, tr.to, amount);
                ++i;
            }
            const auto av = showing ? get(target, PropertyId::Animation, "none") : std::string();
            if (st.animation_value != av) {
                st.animation_value = av;
                st.animation_start = now;
            }
            if (showing && (!incremental || target_changed || !st.plans_valid)) {
                st.plans_valid = false;
                auto plans = compile_animations(target, keyframes);
                st.plans = std::move(plans);
                st.plans_valid = true;
                ++work.animation_compilations;
            } else if (showing) {
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
                    out.style[track.property] = lerp_property(track.property, lo->second, hi->second, eased,
                                                              incremental ? &interpolation : nullptr);
                    typed(track.property, lo->second, hi->second, eased);
                }
            }
            // A keyframe's em lengths, as the element's own.
            if (!st.plans.empty()) {
                resolve_font_relative(out.style, parent);
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
            // A -weft-texture(name) background (a texture the host binds to the name for each frame) or a url() one
            // (an image of the solver's images), laid out as CSS lays out a background image (scene_background.hpp).
            // A gradient or an effect covers the border box and repeats: other boxes, repeats and attachments are
            // outside the profile for them.
            if (const auto image = get(out.style, PropertyId::BackgroundImage, "none"); image != "none") {
                const auto texture = scene_values::texture_name(image);
                if (texture && texture->empty()) {
                    fail("-weft-texture() takes one texture name of letters, digits, '-' and '_', at most " +
                         std::to_string(constants::scene::max_texture_name_bytes) + " bytes");
                }
                const auto url = scene_values::image_url(image);
                if (url && url->empty()) {
                    fail("url() takes one URL");
                }
                if (url && !images) {
                    fail("url(" + *url + ") without images");
                }
                try {
                    if (texture || url) {
                        const auto layout = scene_background_layout(out.style);
                        for (const auto b : {layout.origin, layout.clip}) {
                            if (b == BackgroundBox::Content) {
                                (void)scene_background_box(out.style, b, 0, 0);
                            }
                        }
                    } else {
                        const auto repeat = split(get(out.style, PropertyId::BackgroundRepeat, "repeat"));
                        if (!std::all_of(repeat.begin(), repeat.end(), [](const std::string& w) {
                                return w == "repeat";
                            })) {
                            fail("background-repeat " + get(out.style, PropertyId::BackgroundRepeat) +
                                 " of a gradient or an effect");
                        }
                        for (const auto& [property, initial] : {std::pair{PropertyId::BackgroundAttachment, "scroll"},
                                                                std::pair{PropertyId::BackgroundOrigin, "padding-box"},
                                                                std::pair{PropertyId::BackgroundClip, "border-box"}}) {
                            if (const auto v = trim(get(out.style, property, initial)); v != initial) {
                                fail(std::string(property_name(property)) + " " + v + " of a gradient or an effect");
                            }
                        }
                    }
                } catch (const std::invalid_argument& e) {
                    fail(e.what());
                }
                if (url) {
                    out.background_image = images->image(*url);
                }
            }
            // border-image-source: a url() image, drawn in nine pieces instead of the border styles
            // (scene_border_image.hpp). What the scene cannot draw is a forgiving page's warning (its border styles
            // draw) and a strict page's error; repeat, round and space draw as stretch.
            if (const auto source = get(out.style, PropertyId::BorderImageSource, "none"); source != "none") {
                std::string problem;
                const auto url = scene_values::image_url(source);
                if (!url || url->empty()) {
                    problem = "border-image-source " + source + " (the scene draws a url() image)";
                } else if (!images) {
                    problem = "border-image-source url(" + *url + ") without images";
                } else {
                    try {
                        (void)scene_border_image(out.style, 1, 1, 1, 1);
                    } catch (const std::invalid_argument& e) {
                        problem = e.what();
                    }
                }
                if (problem.empty()) {
                    if (!scene_border_image_stretches(out.style)) {
                        warn("border-image-repeat " + get(out.style, PropertyId::BorderImageRepeat) +
                             " drawn as stretch");
                    }
                    out.border_image = images->image(*url);
                } else if (forgiving) {
                    warn(problem);
                } else {
                    fail(problem);
                }
            }
            const bool shown = !hidden && get(out.style, PropertyId::Display) != "none";
            emit(out, st, id, c, shown);
            // clip-path: none (an SVG element's clip-path attribute is its own, scene_display_list); the element of
            // another draws unclipped. stroke-miterlimit: the stroker's 4 (a miter longer than 4 times the stroke width
            // is beveled).
            for (const auto clip : {PropertyId::ClipPath, PropertyId::WebkitClipPath}) {
                if (const auto v = trim(get(out.style, clip, "none")); v != "none") {
                    undrawn(std::string(property_name(clip)) + " " + std::string(v) +
                            " outside scene profile (drawn unclipped)");
                }
            }
            if (const auto v = trim(get(out.style, PropertyId::StrokeMiterlimit, "4")); v != "4") {
                const auto n = css::parse_number<double>(std::string(v));
                if (!n || *n != 4) {
                    undrawn("stroke-miterlimit " + std::string(v) + " outside scene profile (the miter limit is 4)");
                }
            }
            // backdrop-filter: the functions the scene takes, on an HTML element or an svg root.
            if (const auto* filter = out.style.find(PropertyId::BackdropFilter)) {
                const auto parsed = scene_backdrop_filter(filter->text());
                if (!parsed) {
                    throw DiagnosticError(parsed.error());
                }
                if (*parsed && !out.space.empty() && out.space != constants::xhtml_namespace_uri && out.tag != "svg") {
                    fail("backdrop-filter on an SVG element inside svg");
                }
            }
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
                        display != "inline-block" && display != "inline-flex" && display != "inline-grid" &&
                        display != "inline-table") {
                        inline_context = false;
                    }
                }
                // Its ::before and ::after boxes are element children too (a text field's ::placeholder and
                // ::selection are not boxes).
                for (const auto& g : c.generated) {
                    if (g.local_name != "::before" && g.local_name != "::after") {
                        continue;
                    }
                    elements = true;
                    const auto display = g.values.text(PropertyId::Display, "inline");
                    const auto position = g.values.text(PropertyId::Position, "static");
                    if (display != "none" && position != "absolute" && position != "fixed" && display != "inline" &&
                        display != "inline-block" && display != "inline-flex" && display != "inline-grid" &&
                        display != "inline-table") {
                        inline_context = false;
                    }
                }
            }
            inline_context = inline_context && elements;
            const bool mixed = elements && (inline_context || !trim(out.text).empty());
            if (mixed) {
                out.text.clear();
            }
            if (out.text.empty()) {
                out.text_breaks.clear();
            }
            if (d.field) {
                field_own(out, d, c);
            }
            work.geometry_changed |= !scene_geometry_own_equal(last, out);
            work.paint_changed |=
                last.identity != out.identity || last.tag != out.tag || last.space != out.space ||
                last.text != out.text || last.text_breaks != out.text_breaks || last.attributes != out.attributes ||
                last.style != out.style || bool(last.paint) != bool(out.paint) ||
                (last.paint && out.paint && last.paint->values != out.paint->values) ||
                !scene_effect_equal(last.effect, out.effect) || last.background_image != out.background_image ||
                last.border_image != out.border_image || last.particles != out.particles ||
                !same_field(last.field, out.field);
            return {shown, inline_context, mixed};
        }

        // Whether an element with mixed content shows text node t as a #text child.
        static bool shows_text(const DomSnapshot& t, const Built& built) {
            return built.mixed && t.type == constants::text_node_type &&
                   (built.inline_context || !trim(t.data).empty());
        }

        // A text node an element with mixed content shows: anonymous inline text, of the element's inherited
        // properties and initial others. Its spaces collapse here only where its white-space collapses them all;
        // pre, pre-wrap and break-spaces keep its spaces and newlines, and pre-line its newlines, for the inline
        // layout to break the lines at (as it does with an element's own text).
        static SceneNode text_run(const DomSnapshot& t, const Style& style) {
            SceneNode run;
            run.identity = t.identity;
            run.tag = "#text";
            const auto white_space = style.text(PropertyId::WhiteSpace, "normal");
            const bool collapses = white_space == "normal" || white_space == "nowrap";
            run.text = transformed(collapses ? css::collapse_spaces(t.data) : std::string(t.data), style);
            for (const auto k : inherited_properties) {
                if (const auto* v = style.find(k)) {
                    run.style.set(k, *v);
                }
            }
            run.style[PropertyId::Display] = "inline";
            run.style[PropertyId::Position] = "static";
            return run;
        }

        // Whether a b or strong element of no font-weight of its own takes the bold weight again after its children
        // took the weight its animations gave it.
        static bool bold_again(const SceneNode& out, const ComputedStyle& c) {
            return out.style.contains(PropertyId::FontWeight) && (out.tag == "b" || out.tag == "strong") &&
                   std::find(c.specified_properties.begin(), c.specified_properties.end(), "font-weight") ==
                       c.specified_properties.end() &&
                   out.style.text(PropertyId::FontWeight) != constants::scene::bold_font_weight;
        }

        // The full path: d's scene node anew into output (its last node, whose layout output it keeps).
        void build_into(SceneNode& output, const DomSnapshot& d, const ComputedStyle& c,
                        const Style* parent = nullptr) {
            SceneNode out;
            const auto own_built = build_own(out, output, d, c, parent);
            const auto& prior = std::as_const(output.children);
            // At most one scene child per DOM child.
            out.children.reserve(d.children.size());
            const bool hidden_before = hidden;
            hidden = !own_built.shown;
            // The element's ::before first and ::after last (ComputedStyle::generated), built from their stand-ins. A
            // text field shows its value as its own text: its DOM children (a textarea's default value) build nothing.
            const auto generated_child = [&](const char* name) {
                for (const auto& g : c.generated) {
                    if (g.local_name == name && !d.field) {
                        std::string problem;
                        auto stand_in = generated_dom(g, d.attributes, problem);
                        undrawn(problem);
                        const auto at = find_child(prior, stand_in.identity, out.children.size());
                        out.children.push_back(at < prior.size() ? prior[at] : SceneNode{});
                        build_into(out.children.edit_back(), stand_in, g, &out.style);
                        states[identity(stand_in.identity)].stand_in = std::move(stand_in);
                    }
                }
            };
            generated_child("::before");
            std::size_t ci{};
            static const std::vector<DomSnapshot> no_children;
            for (const auto& child : d.field ? no_children : d.children) {
                if (child.type == constants::element_node_type) {
                    const auto at = find_child(prior, child.identity, out.children.size());
                    out.children.push_back(at < prior.size() ? prior[at] : SceneNode{});
                    build_into(out.children.edit_back(), child, c.children.at(ci++), &out.style);
                } else if (shows_text(child, own_built)) {
                    out.children.push_back(text_run(child, out.style));
                }
            }
            generated_child("::after");
            hidden = hidden_before;
            if (bold_again(out, c)) {
                out.style[PropertyId::FontWeight] = constants::scene::bold_font_weight;
            }
            if (out.children.size() != prior.size()) {
                work.paint_changed = true;
                work.geometry_changed = true;
            } else {
                for (std::size_t n = 0; n < out.children.size(); ++n) {
                    if (std::as_const(out.children)[n].identity != prior[n].identity) {
                        work.paint_changed = true;
                        work.geometry_changed = true;
                    }
                }
            }
            output = std::move(out);
        }

        using Handle = std::shared_ptr<const SceneNode>;

        // The index of the node of identity id among children, looked for at hint first; children.size() for none.
        template <class Children>
        static std::size_t find_child(const Children& children, NodeIdentity id, std::size_t hint) {
            if (hint < children.size() && children[hint].identity == id) {
                return hint;
            }
            for (std::size_t k = 0; k < children.size(); ++k) {
                if (children[k].identity == id) {
                    return k;
                }
            }
            return children.size();
        }

        // Whether two copies of a DOM node hold the same values of its own for a scene build.
        static bool same_dom_own(const DomSnapshot& a, const DomSnapshot& b) {
            return a.local_name == b.local_name && a.namespace_uri == b.namespace_uri &&
                   a.scroll_left == b.scroll_left && a.scroll_top == b.scroll_top && a.attributes == b.attributes &&
                   DomSnapshot::same_field(a.field, b.field) && (!a.field || a.focused == b.focused);
        }

        // Whether two copies of a DOM node have the same children: the same nodes in the same order, and text of the
        // same data.
        static bool same_dom_children(const DomSnapshot& a, const DomSnapshot& b) {
            if (a.children.size() != b.children.size()) {
                return false;
            }
            for (std::size_t k = 0; k < a.children.size(); ++k) {
                const auto& x = a.children[k];
                const auto& y = b.children[k];
                if (x.identity != y.identity || x.type != y.type ||
                    (x.type != constants::element_node_type && x.data != y.data)) {
                    return false;
                }
            }
            return true;
        }

        // Whether children inherit the same from two styles: the inherited properties.
        static bool same_inherited(const Style& a, const Style& b) {
            for (const auto k : inherited_properties) {
                const auto* x = a.find(k);
                const auto* y = b.find(k);
                if (!x != !y || (x && !(*x == *y))) {
                    return false;
                }
            }
            return true;
        }

        // Whether a laid-out node keeps its layout as its values change from a to b: the values layout reads.
        static bool same_layout_own(const SceneNode& a, const SceneNode& b) {
            if (a.identity != b.identity || a.tag != b.tag || a.space != b.space || a.text != b.text ||
                a.scroll != b.scroll || !same_layout_style(a.style, b.style)) {
                return false;
            }
            // A text field's text scrolls with its state.
            if (bool(a.field) != bool(b.field) ||
                (a.field && (a.field->state->scroll_x != b.field->state->scroll_x ||
                             a.field->state->scroll_y != b.field->state->scroll_y))) {
                return false;
            }
            if (a.space == constants::svg_namespace_uri) {
                for (const auto* key : {"width", "height", "viewBox"}) {
                    if (get(a.attributes, key) != get(b.attributes, key)) {
                        return false;
                    }
                }
            }
            return true;
        }

        // The elements of a subtree the update drops: their states go unless the update builds them elsewhere.
        void drop(const SceneNode& n) {
            if (n.tag != "#text") {
                removed.push_back(identity(n.identity));
            }
            for (const auto& c : n.children) {
                drop(c);
            }
        }

        // The incremental path: d's scene node (an element) brought up to date from old, its node from the last
        // update (null for none), od, the copy of d that update read, its computed style c and its parent's style.
        // inherited: what its parent passes down changed; force: an ancestor started or stopped showing, so the
        // subtree is built anew; in_svg: d lies inside an svg element, whose content layout does not see. The node is
        // built again when anything it is made of changed (its DOM node and text children, its computed style or its
        // element children's, what it inherits) or it follows the time; a child whose DOM subtree, computed styles and
        // inheritance did not change and that holds nothing that follows the time keeps its storage, shared with old's.
        // Returns old when nothing changed.
        Handle refresh(const Handle& old, const DomSnapshot& d, const DomSnapshot* od, const ComputedStyle& c,
                       const Style* parent, const SceneId& parent_id, bool inherited, bool force, bool in_svg) {
            const auto id = identity(d.identity);
            seen.insert(id);
            auto& st = states[id];
            st.parent = parent_id;
            const bool fresh = !old || old->identity != d.identity || !od || od->identity != d.identity;
            bool restyled_children{};
            {
                std::size_t k{};
                for (const auto& child : d.children) {
                    if (child.type == constants::element_node_type && c.children.at(k++).stamp > built) {
                        restyled_children = true;
                    }
                }
            }
            // Its generated boxes: one restyled, or another list of them than its last node has.
            bool generated_changed{};
            {
                std::vector<NodeIdentity> had;
                if (!fresh) {
                    for (const auto& child : old->children) {
                        if (is_generated(child.identity) && child.tag != "#text") {
                            had.push_back(child.identity);
                        }
                    }
                }
                std::vector<NodeIdentity> has;
                for (const auto& g : c.generated) {
                    has.push_back(g.identity);
                    restyled_children = restyled_children || g.stamp > built;
                }
                generated_changed = had != has;
            }
            const bool structure = fresh || generated_changed || !same_dom_children(d, *od);
            const bool own = fresh || force || inherited || structure || restyled_children || c.stamp > built ||
                             active.contains(id) || pending.contains(id) || !same_dom_own(d, *od);
            // The node as it was (none when fresh), and its own part built again; copied only for a new node.
            static const SceneNode none;
            const SceneNode& last = fresh ? none : *old;
            std::optional<SceneNode> rebuilt;
            // The style the children inherit from, before and after.
            const Style* before = fresh ? nullptr : st.children_style ? &*st.children_style : &old->style;
            Built built_now{st.shown, false, false};
            bool own_changed = fresh;
            bool inherit = fresh;
            bool force_children = force;
            if (own) {
                SceneNode out;
                const bool animated_before = animated;
                animated = false;
                built_now = build_own(out, last, d, c, parent);
                if (animated) {
                    next_active.insert(id);
                }
                animated = animated || animated_before;
                // The children inherit the style before bold_again.
                std::optional<Style> pre;
                if (bold_again(out, c)) {
                    pre = out.style;
                    out.style[PropertyId::FontWeight] = constants::scene::bold_font_weight;
                }
                if (!in_svg && (fresh || !same_layout_own(last, out))) {
                    layout_dirty = true;
                    layout_points.push_back({refresh_path, fresh ? nullptr : &last});
                }
                own_changed = own_changed || !scene_own_equal(last, out);
                force_children = force || (!fresh && st.shown != built_now.shown);
                st.shown = built_now.shown;
                inherit = inherit || !before || !same_inherited(*before, pre ? *pre : out.style);
                rebuilt = std::move(out);
                st.children_style = std::move(pre);
            }
            const SceneNode& current = rebuilt ? *rebuilt : last;
            const Style& child_parent = st.children_style ? *st.children_style : current.style;
            const auto& prior = last.children;
            SceneChildren next;
            next.reserve(d.children.size());
            bool handles_changed{};
            const bool hidden_before = hidden;
            hidden = !built_now.shown;
            const bool svg_inside = in_svg || current.space == constants::svg_namespace_uri;
            // The element's ::before first and ::after last, refreshed from their stand-ins against the last ones.
            const auto generated_child = [&](const char* name) {
                for (const auto& g : c.generated) {
                    if (g.local_name != name || d.field) {
                        continue;
                    }
                    std::string problem;
                    auto stand_in = generated_dom(g, d.attributes, problem);
                    undrawn(problem);
                    const auto gid = identity(stand_in.identity);
                    const auto at = fresh ? prior.size() : find_child(prior, stand_in.identity, next.size());
                    const Handle kept = at < prior.size() ? prior.handle(at) : nullptr;
                    auto& stored = states[gid].stand_in;
                    const std::optional<DomSnapshot> last_stand_in = std::move(stored);
                    refresh_path.push_back(std::uint32_t(next.size()));
                    Handle h = refresh(kept, stand_in, last_stand_in ? &*last_stand_in : nullptr, g, &child_parent, id,
                                       inherit, force_children, svg_inside);
                    refresh_path.pop_back();
                    states[gid].stand_in = std::move(stand_in);
                    handles_changed = handles_changed || h != kept;
                    next.push_back(std::move(h));
                }
            };
            generated_child("::before");
            std::size_t ci{};
            std::size_t old_hint{};
            static const std::vector<DomSnapshot> no_children;
            for (const auto& child : d.field ? no_children : d.children) {
                if (child.type == constants::element_node_type) {
                    const auto& cc = c.children.at(ci++);
                    const auto at = fresh ? prior.size() : find_child(prior, child.identity, next.size());
                    const Handle kept = at < prior.size() ? prior.handle(at) : nullptr;
                    const DomSnapshot* odc{};
                    if (od) {
                        const auto k = find_child(od->children, child.identity, old_hint);
                        if (k < od->children.size()) {
                            odc = &od->children[k];
                            old_hint = k + 1;
                        }
                    }
                    Handle h;
                    if (kept && !inherit && !force_children && odc && child.revision != 0 &&
                        odc->revision == child.revision && cc.subtree_stamp <= built &&
                        !marked.contains(identity(child.identity))) {
                        ++work.static_subtree_reuses;
                        h = kept;
                    } else {
                        refresh_path.push_back(std::uint32_t(next.size()));
                        h = refresh(kept, child, odc, cc, &child_parent, id, inherit, force_children, svg_inside);
                        refresh_path.pop_back();
                    }
                    handles_changed = handles_changed || h != kept;
                    next.push_back(std::move(h));
                } else if (own) {
                    if (shows_text(child, built_now)) {
                        auto run = text_run(child, child_parent);
                        const auto at = find_child(prior, child.identity, next.size());
                        // Its layout outputs stay until layout runs again, as an element's do (build_own): a run
                        // built again for an element that follows the time is the run it was when nothing changed.
                        if (at < prior.size()) {
                            const auto& was = prior[at];
                            run.box = was.box;
                            run.lines = was.lines;
                            run.fragments = was.fragments;
                            run.scrolled = was.scrolled;
                            run.scroll_shift = was.scroll_shift;
                        }
                        if (at < prior.size() && prior[at] == run) {
                            next.push_back(prior.handle(at));
                        } else {
                            // A run whose change layout does not read (its color) keeps its lines.
                            if (!in_svg && (at >= prior.size() || !same_layout_own(prior[at], run))) {
                                layout_dirty = true;
                                layout_points.push_back({refresh_path, fresh ? nullptr : &last});
                            }
                            handles_changed = true;
                            next.push_back(std::move(run));
                        }
                    }
                } else if (child.type == constants::text_node_type) {
                    // Kept: the text nodes and what their parent gives them did not change.
                    const auto at = find_child(prior, child.identity, next.size());
                    if (at < prior.size()) {
                        next.push_back(prior.handle(at));
                    }
                }
            }
            generated_child("::after");
            hidden = hidden_before;
            // Children that went: their elements' states go unless built elsewhere; a new order is new content.
            const auto& built_children = std::as_const(next);
            bool same_order = built_children.size() == prior.size();
            for (std::size_t k = 0; same_order && k < built_children.size(); ++k) {
                same_order = built_children[k].identity == prior[k].identity;
            }
            if (!same_order) {
                work.paint_changed = true;
                work.geometry_changed = true;
                handles_changed = true;
                if (!svg_inside) {
                    layout_dirty = true;
                    layout_points.push_back({refresh_path, fresh ? nullptr : &last});
                }
                for (std::size_t k = 0; k < prior.size(); ++k) {
                    if (find_child(built_children, prior[k].identity, k) == built_children.size()) {
                        drop(prior[k]);
                    }
                }
            }
            if (!own_changed && !handles_changed) {
                return old;
            }
            SceneNode n = rebuilt ? std::move(*rebuilt) : SceneNode(last);
            n.children = std::move(next);
            return std::make_shared<const SceneNode>(std::move(n));
        }

        // The element's particles at the frame (SceneSolver::set_particles): its -weft-particles stream started,
        // kept or stopped, the bursts asked of it since the last frame started, and the emissions whose particles may
        // live; none while it does not show.
        void emit(SceneNode& out, State& st, const SceneId& id, const ComputedStyle& c, bool shown) {
            using Kind = paint::ParticleEmission::Kind;
            const auto bursts = pending.empty() ? pending.end() : pending.find(id);
            const auto* property = out.style.find(PropertyId::WeftParticles);
            if (!property && st.emissions.empty() && bursts == pending.end()) {
                return;
            }
            auto value = property ? trim(property->text()) : std::string();
            if (value.empty()) {
                value = "none";
            }
            if (value == "none" && st.emissions.empty() && bursts == pending.end()) {
                return;
            }
            // They show in an HTML element's box or an svg root's (scene_display_list).
            if (!out.space.empty() && out.space != constants::xhtml_namespace_uri && out.tag != "svg") {
                fail("particles on an SVG element inside svg");
            }
            // Emissions that started live out their particles without the registry.
            if (!particles && (value != "none" || bursts != pending.end())) {
                fail("particles " + (value != "none" ? value : bursts->second.front().name) +
                     " without registered particles");
            }
            std::shared_ptr<const SceneEmitter> stream;
            if (value != "none") {
                if (!std::all_of(value.begin(), value.end(), [](char ch) {
                        return std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_';
                    })) {
                    fail("-weft-particles takes none or one particle name");
                }
                stream = particles->resolve(value, c.custom_properties);
                // A stream of no particles is none.
                if (!(stream->emitter.rate > 0)) {
                    stream.reset();
                }
            }
            if (!shown) {
                st.emissions.clear();
                return;
            }
            const auto running = std::find_if(st.emissions.begin(), st.emissions.end(), [](const State::Emission& e) {
                return e.kind == Kind::Stream && !e.stop;
            });
            const bool keep =
                running != st.emissions.end() && stream && (running->emitter == stream || *running->emitter == *stream);
            if (running != st.emissions.end() && !keep) {
                running->stop = now;
            }
            if (stream && !keep) {
                State::Emission e;
                e.emitter = std::move(stream);
                e.kind = Kind::Stream;
                e.start = now;
                e.seed = emission_seed(id, st.emitted++);
                e.placed = true;
                st.emissions.push_back(std::move(e));
            }
            if (bursts != pending.end()) {
                bool flies{};
                for (const auto& b : bursts->second) {
                    State::Emission e;
                    e.emitter = particles->resolve(b.name, c.custom_properties);
                    e.kind = Kind::Burst;
                    e.start = now;
                    e.count = b.burst.count;
                    e.duration = b.burst.duration;
                    e.seed = emission_seed(id, st.emitted++);
                    e.target = b.burst.target;
                    e.point = b.burst.point;
                    e.placed = !e.target && !e.point;
                    flies = flies || !e.placed;
                    st.emissions.push_back(std::move(e));
                }
                if (flies) {
                    flights.push_back(id);
                }
            }
            // A stopped stream's particles live out the longest life after its stop, a burst's after its duration.
            constexpr double ms = constants::milliseconds_per_second;
            std::erase_if(st.emissions, [&](const State::Emission& e) {
                const auto& life = e.emitter->emitter.life;
                const double longest = double(std::max(life[0], life[1])) * ms;
                const double end = e.kind == Kind::Burst ? e.start + double(e.duration) * ms + longest
                                   : e.stop              ? *e.stop + longest
                                                         : std::numeric_limits<double>::infinity();
                return now >= end;
            });
            for (const auto& e : st.emissions) {
                paint::ParticleEmission emission;
                emission.kind = e.kind;
                emission.elapsed = (now - e.start) / ms;
                if (e.stop) {
                    emission.stopped = (*e.stop - e.start) / ms;
                }
                emission.count = e.count;
                emission.duration = e.duration;
                emission.seed = e.seed;
                emission.target = e.local;
                frame_particles += paint::particle_timing({e.emitter->emitter, emission}).count;
                out.particles.push_back({e.emitter, emission});
            }
            if (!out.particles.empty()) {
                animated = true;
            }
        }

        // Places the flights of the bursts that started at the laid out frame f: a target element's centre, or a
        // point, in the emitting element's border box px. A burst whose target the frame does not show (absent, or
        // display: none), or whose element maps the plane to a line, flies nowhere.
        void place_flights(SceneNode& root) {
            std::set<SceneId> wanted(flights.begin(), flights.end());
            for (const auto& id : flights) {
                for (const auto& e : states.at(id).emissions) {
                    if (!e.placed && e.target) {
                        wanted.insert(identity(*e.target));
                    }
                }
            }

            struct Placed {
                const SceneNode* node{};
                ScenePlane plane;
                // The child indices from the root down to the node.
                std::vector<std::size_t> path;
            };

            std::map<SceneId, Placed> found;
            std::vector<std::size_t> path;
            // Each element's border box px to the viewport, as SceneGeometry maps them.
            const auto walk = [&](auto&& self, const SceneNode& n, const ScenePlane& parent) -> void {
                if (n.tag == "#text" || get(n.style, PropertyId::Display) == "none") {
                    return;
                }
                const auto plane =
                    parent *
                    (n.fragments.empty() ? scene_plane(n) : affine_plane(SceneAffine::translation(n.box[0], n.box[1])));
                if (const auto id = identity(n.identity); wanted.contains(id)) {
                    found[id] = {&n, plane, path};
                }
                if (n.space == constants::svg_namespace_uri) {
                    return;
                }
                for (std::size_t k = 0; k < n.children.size(); ++k) {
                    path.push_back(k);
                    self(self, n.children[k], plane);
                    path.pop_back();
                }
            };
            walk(walk, std::as_const(root), ScenePlane{});

            // Where each burst flies, in its element's px; written once every target is read.
            struct Flight {
                const std::vector<std::size_t>* path{};
                std::size_t emission{};
                std::array<float, 2> local{};
            };

            std::vector<Flight> placed;
            for (const auto& id : flights) {
                const auto emitter = found.find(id);
                ScenePlane inverse;
                const bool mapped = emitter != found.end() && emitter->second.plane.inverse(inverse);
                auto& emissions = states.at(id).emissions;
                for (std::size_t k = 0; k < emissions.size(); ++k) {
                    auto& e = emissions[k];
                    if (e.placed) {
                        continue;
                    }
                    e.placed = true;
                    std::optional<std::array<double, 2>> at;
                    if (e.point) {
                        at = std::array<double, 2>{(*e.point)[0], (*e.point)[1]};
                    } else if (const auto t = found.find(identity(*e.target)); t != found.end()) {
                        const auto& box = t->second.node->box;
                        at = t->second.plane.point(box[2] / 2., box[3] / 2.);
                    }
                    if (!at || !mapped) {
                        continue;
                    }
                    const auto local = inverse.point((*at)[0], (*at)[1]);
                    if (std::abs(local[0]) <= constants::max_logical_px_magnitude &&
                        std::abs(local[1]) <= constants::max_logical_px_magnitude) {
                        e.local = std::array<float, 2>{float(local[0]), float(local[1])};
                        placed.push_back({&emitter->second.path, k, *e.local});
                    }
                }
            }
            // Only the paths to the emitters change.
            for (const auto& flight : placed) {
                SceneNode* node = &root;
                for (const auto k : *flight.path) {
                    node = &node->children.edit(k);
                }
                node->particles.at(flight.emission).emission.target = flight.local;
            }
            flights.clear();
        }

        // The incremental update (SceneSolver::update): the cascade and the expansion from the last document, the
        // scene built where its input changed, layout where what it reads changed, and f given the scene, which
        // shares its unchanged subtrees with the last frame's.
        template <class Clock>
        void update_incremental(const DomSnapshot& d, SceneFrame& f, float w, float h, double time, bool viewport_moved,
                                const Clock& clock) {
            const auto ms = [](auto a, auto b) {
                return std::chrono::duration<double, std::milli>(b - a).count();
            };
            const auto style_start = clock();
            try {
                ++stamp;
                // A forgiving sheet's @media conditions on the viewport: when their outcomes change, other rules match
                // and the cascade starts over.
                const bool media_changed = compiled->set_viewport(w, h);
                const bool input_changed = compiled->refresh_input(d, style_input) || media_changed;
                if (input_changed || viewport_moved || !computed) {
                    if (!cascade) {
                        cascade.emplace();
                    }
                    if (!computed) {
                        computed.emplace();
                    }
                    if (input_changed) {
                        compiled->update(d, previous_dom && !media_changed ? &*previous_dom : nullptr, *cascade,
                                         style_memo, stamp);
                    }
                    expand_into(*cascade, *computed, viewport_moved);
                    expanded = stamp;
                    expanded_viewport = viewport;
                    ++work.style_evaluations;
                } else {
                    ++work.style_reuses;
                }
                const auto build_start = clock();
                work.style_ms += ms(style_start, build_start);
                // The ancestors of elements that follow the time or have bursts are built though their DOM did not
                // change.
                marked.clear();
                const auto mark = [&](SceneId id) {
                    while (marked.insert(id).second) {
                        const auto it = states.find(id);
                        if (it == states.end() || it->second.parent == SceneId{}) {
                            return;
                        }
                        id = it->second.parent;
                    }
                };
                for (const auto& id : active) {
                    mark(id);
                }
                for (const auto& [id, bursts] : pending) {
                    mark(id);
                }
                next_active.clear();
                seen.clear();
                removed.clear();
                layout_dirty = false;
                layout_points.clear();
                refresh_path.clear();
                auto root = retained;
                const bool kept = root && previous_dom && previous_dom->identity == d.identity && d.revision != 0 &&
                                  previous_dom->revision == d.revision && computed->subtree_stamp <= built &&
                                  !marked.contains(identity(d.identity));
                if (kept) {
                    ++work.static_subtree_reuses;
                } else {
                    root = refresh(retained, d, previous_dom ? &*previous_dom : nullptr, *computed, nullptr, SceneId{},
                                   false, false, false);
                }
                // Bursts on elements the frame did not show are dropped.
                pending.clear();
                pending_count = 0;
                if (frame_particles > constants::scene::max_particles_per_frame) {
                    fail("particles: " + std::to_string(frame_particles) + " in a frame, more than " +
                         std::to_string(constants::scene::max_particles_per_frame));
                }
                for (const auto& id : removed) {
                    if (!seen.contains(id)) {
                        states.erase(id);
                    }
                }
                active = std::move(next_active);
                next_active.clear();
                const auto layout_start = clock();
                work.build_ms += ms(build_start, layout_start);
                const bool layout = !retained || layout_dirty || w != previous_width || h != previous_height;
                if (layout) {
                    work.geometry_changed = work.paint_changed = true;
                    SceneNode laid_root = *root;
                    // Only the subtrees around what changed where they are laid out apart; else the whole frame.
                    if (retained && w == previous_width && h == previous_height &&
                        layout_part(laid_root, retained_elements)) {
                        ++work.partial_layouts;
                    } else {
                        layout_frame(laid_root, w, h, retained_elements);
                    }
                    root = std::make_shared<const SceneNode>(std::move(laid_root));
                    ++work.layout_evaluations;
                } else {
                    ++work.layout_reuses;
                }
                if (!flights.empty()) {
                    SceneNode placed = *root;
                    place_flights(placed);
                    root = std::make_shared<const SceneNode>(std::move(placed));
                }
                retained = std::move(root);
                built = stamp;
                if (previous_dom) {
                    dom::update_copy(*previous_dom, d);
                } else {
                    previous_dom = d;
                }
                f.root = *retained;
                // f holds the last frame's element geometry unless it is another frame.
                if (layout || f.revision != revision || !revision) {
                    f.elements = retained_elements;
                }
                f.time_ms = time;
                f.animated = animated;
                previous_width = w;
                previous_height = h;
                // Content that did not change keeps its revision.
                if (!revision || work.paint_changed || work.geometry_changed || animated) {
                    revision = next_revision.fetch_add(1, std::memory_order_relaxed);
                }
                f.revision = revision;
                work.layout_ms += ms(layout_start, clock());
            } catch (...) {
                // The next update starts anew.
                cascade.reset();
                computed.reset();
                style_input = {};
                previous_dom.reset();
                retained.reset();
                built = expanded = 0;
                throw;
            }
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
                const SceneNode* root{};
                bool anonymous{};
                // The root's storage, where the run reached the root through its parent's children (none for the
                // run's top): its content and lines are kept for the frames after (kept_contexts).
                SceneChildren::Handle handle;
                // The context's own content and lines, or those kept for its root.
                InlineContext own;
                InlineContext* state{};
            };

            std::vector<LayoutBox> boxes;
            // The node of each box; null for an anonymous block.
            std::vector<const SceneNode*> targets;
            std::map<std::size_t, Context> contexts;
            // Fixed boxes, laid out as absolute ones in their containing block (with these styles) and then placed in
            // their parent's coordinates: each box with its parent's.
            std::deque<SceneStyle> fixed_styles;
            std::vector<std::pair<std::size_t, std::size_t>> fixed;
            // The values the run's top box is laid out with instead of its own (a table cell's at its column's width).
            const SceneStyle* top_style{};
        };

        // A box that contains its fixed descendants (and absolute ones): one with a transform, a backdrop filter or a
        // filter, or one about to have one.
        static bool contains_fixed(const SceneStyle& s) {
            const auto backdrop = css::trim(s.text(PropertyId::BackdropFilter, "none"));
            const auto filter = css::trim(s.text(PropertyId::Filter, "none"));
            const auto will = s.text(PropertyId::WillChange, "auto");
            return s.text(PropertyId::Transform, "none") != "none" || (!backdrop.empty() && backdrop != "none") ||
                   (!filter.empty() && filter != "none") || will.find("transform") != std::string_view::npos ||
                   will.find("filter") != std::string_view::npos;
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

        // Layout output by node while a frame is laid out: the tree stays as it is until layout_frame writes what
        // changed (apply_layout), so subtrees whose layout did not change keep their storage. A node layout does not
        // reach keeps its own.
        struct LaidOut {
            std::array<float, 4> box{};
            std::vector<SceneLine> lines;
            std::vector<SceneFragment> fragments;
            std::array<float, 4> scrolled{};
            // The node's SceneNode::scroll_shift: zero where layout writes its box, its parent's position once
            // scroll_content moved it.
            std::array<float, 2> shift{};
        };

        std::unordered_map<const SceneNode*, LaidOut> laid;

        // The output layout writes for n, starting from n's own.
        LaidOut& laid_out(const SceneNode& n) {
            auto [it, fresh] = laid.try_emplace(&n);
            if (fresh) {
                it->second = {n.box, n.lines, n.fragments, n.scrolled, n.scroll_shift};
            }
            return it->second;
        }

        const std::array<float, 4>& box_of(const SceneNode& n) const {
            const auto it = laid.find(&n);
            return it == laid.end() ? n.box : it->second.box;
        }

        // Clears the layout of a subtree whose boxes the lines of an inline formatting context place.
        void clear_layout(const SceneNode& n) {
            auto& out = laid[&n];
            out.box = {};
            out.lines.clear();
            out.fragments.clear();
            for (const auto& c : n.children) {
                clear_layout(c);
            }
        }

        // n with the layout's output written: n itself when nothing in its subtree changed.
        Handle apply_layout(const Handle& node) {
            const auto& n = *node;
            const auto it = laid.find(&n);
            const bool own =
                it != laid.end() &&
                (it->second.box != n.box || it->second.lines != n.lines || it->second.fragments != n.fragments ||
                 it->second.scrolled != n.scrolled || it->second.shift != n.scroll_shift);
            std::vector<std::pair<std::size_t, Handle>> changed;
            for (std::size_t k = 0; k < n.children.size(); ++k) {
                const auto child = n.children.handle(k);
                if (auto laid_child = apply_layout(child); laid_child != child) {
                    changed.emplace_back(k, std::move(laid_child));
                }
            }
            if (!own && changed.empty()) {
                return node;
            }
            auto copy = std::make_shared<SceneNode>(n);
            if (own) {
                copy->box = it->second.box;
                copy->lines = std::move(it->second.lines);
                copy->fragments = std::move(it->second.fragments);
                copy->scrolled = it->second.scrolled;
                copy->scroll_shift = it->second.shift;
            }
            for (auto& [k, child] : changed) {
                copy->children.set(k, std::move(child));
            }
            return copy;
        }

        // `fixed_block` is the box of the containing block of fixed boxes here: the run's top when it is the scene's
        // root (the viewport), or the nearest box with a transform; none inside an inline-block without one. `handle`
        // is n's storage, where the run reached n through its parent's children.
        void collect_boxes(const SceneNode& n, std::size_t parent, LayoutRun& run,
                           std::optional<std::size_t> fixed_block, const SceneChildren::Handle& handle = {}) {
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
            const auto overflow = n.style.text(PropertyId::Overflow, "visible");
            if (overflow != "visible" && overflow != "hidden" && overflow != "auto" && overflow != "scroll" &&
                overflow != "clip") {
                fail("unsupported scene value overflow:" + std::string(overflow));
            }
            if (const auto truncation = n.style.text(PropertyId::TextOverflow, "clip");
                truncation != "clip" && truncation != "ellipsis") {
                fail("unsupported scene value text-overflow:" + std::string(truncation));
            }
            {
                auto& out = laid_out(n);
                out.lines.clear();
                out.fragments.clear();
            }
            // Tables are inline tables, laid out as atomic inlines (lay_out_table); the run of a cell lays it out.
            if (const auto d = n.style.text(PropertyId::Display, "inline");
                d == "table" || (d.starts_with("table-") && !(parent == LayoutBox::root && run.top_style))) {
                fail("display " + std::string(d) + " outside an inline table outside scene profile");
            }
            const auto index = run.boxes.size();
            LayoutBox box;
            box.style = parent == LayoutBox::root && run.top_style ? run.top_style : &n.style;
            box.parent = parent;
            // A fixed box is an absolute box of its containing block. Away from its parent, a static position (both
            // insets of an axis auto) would be the parent's: outside the profile.
            if (n.style.text(PropertyId::Position, "static") == "fixed" &&
                n.style.text(PropertyId::Display, "inline") == "none") {
                // A fixed box that is not shown has no box to place: absolute, where it is, for the adapter.
                auto& style = run.fixed_styles.emplace_back(n.style);
                style[PropertyId::Position] = "absolute";
                box.style = &style;
            } else if (n.style.text(PropertyId::Position, "static") == "fixed") {
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
                auto& context = run.contexts[index];
                context.root = &n;
                context.handle = handle;
                return;
            }
            if (scene_inline_children(n)) {
                LayoutBox anonymous;
                anonymous.style = &anonymous_block();
                anonymous.parent = index;
                anonymous.text = true;
                auto& context = run.contexts[run.boxes.size()];
                context.root = &n;
                context.handle = handle;
                context.anonymous = true;
                run.boxes.push_back(anonymous);
                run.targets.push_back(nullptr);
                // Positioned children stay boxes of the element; the lines place the others.
                for (std::size_t k = 0; k < n.children.size(); ++k) {
                    const auto& c = n.children[k];
                    if (out_of_flow(c) && c.style.text(PropertyId::Display) != "none") {
                        collect_boxes(c, index, run, inner_block, n.children.handle(k));
                    } else {
                        clear_layout(c);
                    }
                }
                return;
            }
            for (std::size_t k = 0; k < n.children.size(); ++k) {
                collect_boxes(n.children[k], index, run, inner_block, n.children.handle(k));
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

        // A computed length as Blink takes it into layout: whole LayoutUnits, truncated toward zero.
        static float layout_unit(float px) {
            return std::trunc(px * constants::layout_units_per_px) / constants::layout_units_per_px;
        }

        // An edge length of a box in px: px, em of its font size, or a percentage of `base`; in whole LayoutUnits,
        // truncated, as layout takes it (layout_boxes, and Blink).
        static float edge_length(const SceneStyle& s, PropertyId id, float base) {
            const auto v = css::trim(s.text(id, "0"));
            if (v == "auto") {
                return 0;
            }
            if (v.ends_with('%')) {
                return layout_unit(float(length(v, base)));
            }
            const auto [digits, unit] = css::split_unit(v, {.px = true, .em = true});
            const auto n = float(number(digits));
            return layout_unit(unit == css::Unit::Em
                                   ? n * float(length(s.text(PropertyId::FontSize, constants::default_font_size)))
                                   : n);
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
                // The first item in order-modified document order.
                for (const auto i : scene_child_order(n)) {
                    const auto& c = n.children[i];
                    if (in_flow(c)) {
                        const auto b = baseline_of(c);
                        return b ? std::optional<float>(box_of(c)[1] + *b) : std::nullopt;
                    }
                }
                return std::nullopt;
            }
            for (auto c = n.children.rbegin(); c != n.children.rend(); ++c) {
                if (in_flow(*c)) {
                    if (const auto b = baseline_of(*c)) {
                        return box_of(*c)[1] + *b;
                    }
                }
            }
            return std::nullopt;
        }

        // A ruby element (CSS Ruby 1) laid out as an atomic inline: its content in columns, each the base text up to an
        // rt (its #text runs side by side) under the rt's text, as wide as the wider of the two, both centred
        // (ruby-position: over, ruby-align: center). Its baseline is its bases'. As Blink places them, an annotation's
        // em box (SceneText::em_box: the font size about the baseline, split as the OS/2 typo metrics of the faces its
        // text uses split it) stands on its bases' em box, whatever the rt's line-height, and the line makes room only
        // for the annotation's em box: the half-leading of a line taller than its text takes it, a tighter line grows
        // by the rest, and the part of the annotation's line box above its em box may stand above the line box (a
        // negative top margin). An rt of display: none annotates nothing; rp shows nothing.
        SceneAtomicExtent lay_out_ruby(const SceneNode& node, SceneAtomicExtent e) {
            struct Column {
                std::vector<const SceneNode*> base;
                const SceneNode* annotation{};
            };

            std::vector<Column> columns(1);
            for (const auto& c : node.children) {
                if (c.tag == "rt") {
                    columns.back().annotation = &c;
                    columns.emplace_back();
                    continue;
                }
                if (c.tag == "rp" || c.style.text(PropertyId::Display) == "none") {
                    continue;
                }
                if (c.tag != "#text") {
                    fail("ruby base content other than text outside scene profile");
                }
                columns.back().base.push_back(&c);
            }
            const auto shown = [](const SceneNode* a) {
                return a && a->style.text(PropertyId::Display) != "none" && !a->text.empty();
            };
            // The bases' line boxes on one baseline: how far they reach above and below it.
            float base_above{};
            float base_below{};
            std::map<const SceneNode*, SceneTextMetrics> metrics;
            for (const auto& column : columns) {
                for (const auto* b : column.base) {
                    const auto m = text->measure(b->text, b->style);
                    metrics[b] = m;
                    base_above = std::max(base_above, m.baseline);
                    base_below = std::max(base_below, m.height - m.baseline);
                }
            }
            // From the baseline (y down): the top of each annotation's line box, the top of the ruby's box, and the top
            // of the room the line makes for it.
            std::map<const SceneNode*, float> annotation_top;
            float top = -base_above;
            float room = -base_above;
            for (const auto& column : columns) {
                if (!shown(column.annotation)) {
                    continue;
                }
                // A column without base text stands its annotation on the ruby's font.
                float base_ascent = column.base.empty() ? text->strut(scene_font(node.style)).ascent : 0;
                for (const auto* b : column.base) {
                    base_ascent = std::max(base_ascent, text->em_box(b->text, b->style).ascent);
                }
                const auto& a = *column.annotation;
                const auto em = text->em_box(a.text, a.style);
                const auto m = text->measure(a.text, a.style);
                metrics[&a] = m;
                const float baseline = -base_ascent - em.descent;
                annotation_top[&a] = baseline - m.baseline;
                top = std::min(top, baseline - m.baseline);
                room = std::min(room, baseline - em.ascent);
            }
            float x{};
            for (const auto& column : columns) {
                float base_width{};
                for (const auto* b : column.base) {
                    base_width += metrics[b].width;
                }
                const float annotation_width = shown(column.annotation) ? metrics[column.annotation].width : 0;
                const float width = std::max(base_width, annotation_width);
                float bx = x + (width - base_width) / 2;
                for (const auto* b : column.base) {
                    const auto& m = metrics[b];
                    auto& out = laid_out(*b);
                    out.box = {bx, -top - m.baseline, m.width, m.height};
                    out.lines = {{b->text, {0, 0, m.width, m.height}, m.baseline}};
                    out.fragments.clear();
                    bx += m.width;
                }
                if (column.annotation) {
                    auto& out = laid_out(*column.annotation);
                    out.fragments.clear();
                    if (shown(column.annotation)) {
                        const auto& m = metrics[column.annotation];
                        out.box = {x + (width - annotation_width) / 2, annotation_top[column.annotation] - top, m.width,
                                   m.height};
                        out.lines = {{column.annotation->text, {0, 0, m.width, m.height}, m.baseline}};
                    } else {
                        out.box = {};
                        out.lines.clear();
                    }
                }
                x += width;
            }
            auto& out = laid_out(node);
            out.box[2] = x;
            out.box[3] = base_below - top;
            out.lines.clear();
            out.fragments.clear();
            context_baselines[&node] = -top;
            e.width = x;
            e.height = out.box[3];
            e.baseline = -top;
            // The box above the room the line makes takes none of the line's height.
            e.margin[0] -= room - top;
            return e;
        }

        // The first in-flow line box's baseline in a laid-out box, from its border box top (CSS 2.1 §17.5.3, a table
        // cell's baseline): its own lines' first, else its first in-flow child's that has one; none without lines.
        std::optional<float> first_baseline_of(const SceneNode& n) const {
            if (n.space == constants::svg_namespace_uri) {
                return std::nullopt;
            }
            if (const auto it = context_first_baselines.find(&n); it != context_first_baselines.end()) {
                return it->second;
            }
            for (const auto& c : n.children) {
                if (c.tag == "#text" || c.style.text(PropertyId::Display) == "none" || out_of_flow(c)) {
                    continue;
                }
                if (const auto b = first_baseline_of(c)) {
                    return box_of(c)[1] + *b;
                }
            }
            return std::nullopt;
        }

        // A side's border width in px: none when its style is none or hidden.
        static float border_width(const SceneStyle& s, std::size_t side, float base) {
            const auto style = s.text(border_style_edges[side], "solid");
            return style == "none" || style == "hidden" ? 0.f
                                                        : layout_unit(edge_length(s, border_width_edges[side], base));
        }

        // A px length a table's property states with its box's padding and borders when it sizes the content box (a
        // percentage is a diagnostic); none for auto and none.
        static std::optional<float> table_length(const SceneStyle& s, PropertyId id, float base, float edges,
                                                 std::string_view what) {
            const auto v = css::trim(s.text(id, "auto"));
            if (v == "auto" || v == "none") {
                return std::nullopt;
            }
            if (v.ends_with('%')) {
                fail("percentage " + std::string(property_name(id)) + " of a " + std::string(what) +
                     " outside scene profile");
            }
            const bool content_box = css::trim(s.text(PropertyId::BoxSizing, "content-box")) == "content-box";
            return layout_unit(edge_length(s, id, base)) + (content_box ? edges : 0.f);
        }

        // CSS Tables 3 §3.10.4: the columns' widths for the width they share, from their min-content and max-content
        // widths and whether a cell's px width constrains them. Up to the min-content-specified guess (constrained
        // columns at their max-content widths, the others at their min-content ones) each column moves from its
        // min-content width toward its guess in proportion; then from the guess toward its max-content width; past
        // the max-content widths the rest goes to the unconstrained columns by their max-content widths (equally when
        // those are all 0), else to the constrained ones by theirs, else to all equally.
        static std::vector<float> distribute_columns(const std::vector<float>& min, const std::vector<float>& max,
                                                     const std::vector<bool>& fixed, float target) {
            const auto n = min.size();
            std::vector<float> guess(n);
            float sum_min{};
            float sum_guess{};
            float sum_max{};
            for (std::size_t k = 0; k < n; ++k) {
                guess[k] = fixed[k] ? max[k] : min[k];
                sum_min += min[k];
                sum_guess += guess[k];
                sum_max += max[k];
            }
            std::vector<float> out = min;
            if (target <= sum_min || n == 0) {
                return out;
            }
            const auto between = [&](const std::vector<float>& from, const std::vector<float>& to, float from_sum,
                                     float to_sum) {
                const float t = to_sum > from_sum ? (target - from_sum) / (to_sum - from_sum) : 1.f;
                for (std::size_t k = 0; k < n; ++k) {
                    out[k] = from[k] + (to[k] - from[k]) * t;
                }
            };
            if (target <= sum_guess) {
                between(min, guess, sum_min, sum_guess);
                return out;
            }
            if (target <= sum_max) {
                between(guess, max, sum_guess, sum_max);
                return out;
            }
            out = max;
            const float excess = target - sum_max;
            const auto share = [&](auto&& take, bool proportional) {
                float total{};
                std::size_t count{};
                for (std::size_t k = 0; k < n; ++k) {
                    if (take(k)) {
                        total += max[k];
                        ++count;
                    }
                }
                if (!count || (proportional && total <= 0)) {
                    return false;
                }
                for (std::size_t k = 0; k < n; ++k) {
                    if (take(k)) {
                        out[k] += proportional ? excess * max[k] / total : excess / float(count);
                    }
                }
                return true;
            };
            if (share(
                    [&](std::size_t k) {
                        return !fixed[k] && max[k] > 0;
                    },
                    true) ||
                share(
                    [&](std::size_t k) {
                        return !fixed[k];
                    },
                    false) ||
                share(
                    [&](std::size_t k) {
                        return fixed[k] && max[k] > 0;
                    },
                    true)) {
                return out;
            }
            share(
                [](std::size_t) {
                    return true;
                },
                false);
            return out;
        }

        // Lays out a table (display: inline-table) as an atomic inline, shrunk to fit an available width, and returns
        // its extents; its box is placed later. CSS 2.1 §17 with CSS Tables 3's sizing, as Chrome lays tables out, for
        // a table of table-row children whose children are table-cells:
        // - a cell's min-content width is its content's (with its padding and borders) or its min-width, whichever is
        //   wider; its max-content width is its content's (no wider than its max-width), or its px width, which
        //   constrains its column; a px width is no floor: under a min-content constraint the cell is as narrow as
        //   its content;
        // - a column takes its cells' widest of each; an auto-width table fits the available width between its
        //   columns' summed min-content and max-content widths (with its spacing, padding and borders), a px width at
        //   least the min-content one, and its columns share it (distribute_columns);
        // - each cell is laid out at its column's width; a row is as tall as its tallest cell and its baseline-aligned
        //   cells' extents about their baselines (cell and row heights are floors); its cells are as tall as it, their
        //   content placed by their vertical-align: top, middle, bottom, or baseline (any other value);
        // - a row's baseline is its baseline-aligned cells' (their first lines'), or without one the lowest bottom of
        //   its cells' content boxes; an inline table's is its first row's;
        // - separate borders space the columns and rows by border-spacing; collapsed borders have no spacing and no
        //   table padding.
        // Captions, columns, row groups, cell spans, anonymous table boxes, borders of rows (and any border when
        // collapsed), percentage widths and heights, min- and max-heights, fixed layout of a set width and table
        // heights are diagnostics.
        SceneAtomicExtent lay_out_table(const SceneNode& table, float available, SceneAtomicExtent e) {
            const auto& ts = table.style;
            const float base = std::isfinite(available) ? available : 0;

            struct Cell {
                const SceneNode* node{};
                float min{};
                float max{};
                bool fixed{};
                std::string align;
                float height{};
                float specified_height{};
                std::optional<float> baseline;
                float bottom_edge{};
            };

            struct Row {
                const SceneNode* node{};
                std::vector<Cell> cells;
                float height{};
                float baseline{};
                float y{};
            };

            const bool collapse = css::trim(ts.text(PropertyId::BorderCollapse, "separate")) == "collapse";
            const auto no_border = [&](const SceneStyle& s, std::string_view what) {
                for (std::size_t side = 0; side < border_width_edges.size(); ++side) {
                    if (border_width(s, side, base) > 0) {
                        fail("border of a " + std::string(what) + " outside scene profile");
                    }
                }
            };
            const auto no_vertical_bounds = [&](const SceneStyle& s, std::string_view what) {
                for (const auto id : {PropertyId::MinHeight, PropertyId::MaxHeight}) {
                    const auto v = css::trim(s.text(id, "auto"));
                    if (v != "auto" && v != "none" && v != "0" && v != "0px") {
                        fail(std::string(property_name(id)) + " of a " + std::string(what) + " outside scene profile");
                    }
                }
            };
            if (collapse) {
                no_border(ts, "table with collapsed borders");
            }
            if (css::trim(ts.text(PropertyId::Height, "auto")) != "auto") {
                fail("height of a table outside scene profile");
            }
            std::vector<Row> rows;
            for (const auto& r : table.children) {
                if (r.tag == "#text") {
                    fail("text in a table outside scene profile (anonymous table cells)");
                }
                const auto d = r.style.text(PropertyId::Display, "inline");
                if (d == "none") {
                    continue;
                }
                if (out_of_flow(r)) {
                    fail("positioned child of a table outside scene profile");
                }
                if (d != "table-row") {
                    fail("display " + std::string(d) + " in a table outside scene profile (rows only)");
                }
                if (!r.text.empty()) {
                    fail("text in a table row outside scene profile (anonymous table cells)");
                }
                no_border(r.style, "table row");
                no_vertical_bounds(r.style, "table row");
                auto& row = rows.emplace_back();
                row.node = &r;
                for (const auto& c : r.children) {
                    if (c.tag == "#text") {
                        fail("text in a table row outside scene profile (anonymous table cells)");
                    }
                    const auto cd = c.style.text(PropertyId::Display, "inline");
                    if (cd == "none") {
                        continue;
                    }
                    if (out_of_flow(c)) {
                        fail("positioned child of a table row outside scene profile");
                    }
                    if (cd != "table-cell") {
                        fail("display " + std::string(cd) + " in a table row outside scene profile (cells only)");
                    }
                    if (c.attributes.find("colspan") != c.attributes.end() ||
                        c.attributes.find("rowspan") != c.attributes.end()) {
                        fail("table cell spans outside scene profile");
                    }
                    if (collapse) {
                        no_border(c.style, "table cell with collapsed borders");
                    }
                    no_vertical_bounds(c.style, "table cell");
                    if (css::trim(c.style.text(PropertyId::EmptyCells, "show")) != "show") {
                        fail("empty-cells " + std::string(c.style.text(PropertyId::EmptyCells)) +
                             " outside scene profile");
                    }
                    row.cells.push_back({&c});
                }
            }
            // The table's edges: borders, and padding with separate borders.
            std::array<float, 4> edges{};
            for (std::size_t side = 0; side < edges.size(); ++side) {
                static constexpr std::array<PropertyId, 4> padding{PropertyId::PaddingTop, PropertyId::PaddingRight,
                                                                   PropertyId::PaddingBottom, PropertyId::PaddingLeft};
                edges[side] =
                    border_width(ts, side, base) + (collapse ? 0.f : layout_unit(edge_length(ts, padding[side], base)));
            }
            std::array<float, 2> spacing{};
            if (!collapse) {
                const auto words = split(ts.text(PropertyId::BorderSpacing, "0"));
                if (words.empty() || words.size() > spacing.size()) {
                    fail("border-spacing " + std::string(ts.text(PropertyId::BorderSpacing)) +
                         " outside scene profile");
                }
                const float size = float(length(ts.text(PropertyId::FontSize, constants::default_font_size)));
                spacing[0] = std::max(0.f, layout_unit(scene_values::len(words[0], size)));
                spacing[1] =
                    words.size() > 1 ? std::max(0.f, layout_unit(scene_values::len(words[1], size))) : spacing[0];
            }
            // Columns: the cells' intrinsic widths, their content's with their own width auto.
            std::size_t columns{};
            for (const auto& r : rows) {
                columns = std::max(columns, r.cells.size());
            }
            std::vector<float> col_min(columns);
            std::vector<float> col_max(columns);
            std::vector<bool> col_fixed(columns);
            for (auto& r : rows) {
                for (std::size_t k = 0; k < r.cells.size(); ++k) {
                    auto& cell = r.cells[k];
                    const auto& cs = cell.node->style;
                    // A cell lays its content out as a block container does (a block formatting context).
                    SceneStyle measured = cs;
                    for (const auto id :
                         {PropertyId::Width, PropertyId::MinWidth, PropertyId::MaxWidth, PropertyId::Height}) {
                        measured.erase(id);
                    }
                    measured[PropertyId::Display] = "block";
                    const auto content = intrinsic_widths(*cell.node, measured);
                    const float h_edges = border_width(cs, 1, base) + border_width(cs, 3, base) +
                                          layout_unit(edge_length(cs, PropertyId::PaddingLeft, base)) +
                                          layout_unit(edge_length(cs, PropertyId::PaddingRight, base));
                    const auto width = table_length(cs, PropertyId::Width, base, h_edges, "table cell");
                    const auto min_width = table_length(cs, PropertyId::MinWidth, base, h_edges, "table cell");
                    const auto max_width = table_length(cs, PropertyId::MaxWidth, base, h_edges, "table cell");
                    cell.fixed = width.has_value();
                    cell.min = std::max(content.min, min_width.value_or(0.f));
                    cell.max = std::max(
                        cell.min,
                        width ? *width
                              : std::min(content.max, max_width.value_or(std::numeric_limits<float>::infinity())));
                    col_min[k] = std::max(col_min[k], cell.min);
                    col_max[k] = std::max(col_max[k], cell.max);
                    col_fixed[k] = col_fixed[k] || cell.fixed;
                }
            }
            // The table's width, and its columns'.
            const float spacing_x = columns ? spacing[0] * float(columns + 1) : 0.f;
            const float around = spacing_x + edges[1] + edges[3];
            float grid_min = around;
            float grid_max = around;
            for (std::size_t k = 0; k < columns; ++k) {
                grid_min += col_min[k];
                grid_max += col_max[k];
            }
            float used{};
            if (const auto w = table_length(ts, PropertyId::Width, base, edges[1] + edges[3], "table")) {
                if (css::trim(ts.text(PropertyId::TableLayout, "auto")) == "fixed") {
                    fail("table-layout: fixed outside scene profile");
                }
                used = std::max(*w, grid_min);
            } else {
                used = std::max(grid_min, std::min(std::max(0.f, available - e.margin[1] - e.margin[3]), grid_max));
            }
            const auto widths = distribute_columns(col_min, col_max, col_fixed, used - around);
            std::vector<float> column_x(columns);
            float x = edges[3] + spacing[0];
            for (std::size_t k = 0; k < columns; ++k) {
                column_x[k] = x;
                x += widths[k] + spacing[0];
            }
            // Each cell at its column's width, its height auto; then the rows.
            for (auto& r : rows) {
                const auto row_height = table_length(r.node->style, PropertyId::Height, base, 0, "table row");
                float above{};
                float below{};
                bool aligned{};
                r.height = row_height.value_or(0.f);
                for (std::size_t k = 0; k < r.cells.size(); ++k) {
                    auto& cell = r.cells[k];
                    const auto& cs = cell.node->style;
                    SceneStyle sized = cs;
                    for (const auto id : {PropertyId::MinWidth, PropertyId::MaxWidth, PropertyId::Height}) {
                        sized.erase(id);
                    }
                    sized[PropertyId::Display] = "block";
                    sized[PropertyId::Width] = px_text(widths[k]);
                    sized[PropertyId::BoxSizing] = "border-box";
                    LayoutOptions options;
                    options.fit_width = widths[k];
                    layout_run(*cell.node, options, false, &sized);
                    cell.height = box_of(*cell.node)[3];
                    cell.baseline = first_baseline_of(*cell.node);
                    const float v_edges = border_width(cs, 0, base) + border_width(cs, 2, base) +
                                          layout_unit(edge_length(cs, PropertyId::PaddingTop, base)) +
                                          layout_unit(edge_length(cs, PropertyId::PaddingBottom, base));
                    cell.bottom_edge =
                        border_width(cs, 2, base) + layout_unit(edge_length(cs, PropertyId::PaddingBottom, base));
                    cell.specified_height =
                        table_length(cs, PropertyId::Height, base, v_edges, "table cell").value_or(0.f);
                    cell.align = std::string(css::trim(cs.text(PropertyId::VerticalAlign, "baseline")));
                    r.height = std::max({r.height, cell.height, cell.specified_height});
                    if (cell.align != "top" && cell.align != "middle" && cell.align != "bottom") {
                        const float b = cell.baseline.value_or(cell.height - cell.bottom_edge);
                        above = std::max(above, b);
                        below = std::max(below, cell.height - b);
                        aligned = true;
                    }
                }
                r.height = std::max(r.height, above + below);
                if (aligned) {
                    r.baseline = above;
                } else {
                    for (const auto& cell : r.cells) {
                        r.baseline = std::max(r.baseline, r.height - cell.bottom_edge);
                    }
                    if (r.cells.empty()) {
                        r.baseline = r.height;
                    }
                }
            }
            float y = edges[0] + (rows.empty() ? 0.f : spacing[1]);
            for (auto& r : rows) {
                r.y = y;
                y += r.height + spacing[1];
            }
            const float height = y + edges[2];
            // The boxes: rows across the columns, cells as tall as their row, each cell's content where its
            // vertical-align puts it.
            const float row_x = edges[3] + spacing[0];
            const float row_w = columns ? column_x.back() + widths.back() - row_x : 0.f;
            for (auto& r : rows) {
                auto& row_out = laid_out(*r.node);
                row_out.box = {row_x, r.y, row_w, r.height};
                row_out.shift = {};
                row_out.lines.clear();
                row_out.fragments.clear();
                for (std::size_t k = 0; k < r.cells.size(); ++k) {
                    const auto& cell = r.cells[k];
                    const float free = r.height - cell.height;
                    float offset{};
                    if (cell.align == "middle") {
                        offset = std::floor(free / 2 * constants::text::layout_units_per_px) /
                                 constants::text::layout_units_per_px;
                    } else if (cell.align == "bottom") {
                        offset = free;
                    } else if (cell.align != "top") {
                        offset = r.baseline - cell.baseline.value_or(cell.height - cell.bottom_edge);
                    }
                    auto& out = laid_out(*cell.node);
                    out.box = {column_x[k] - row_x, 0, widths[k], r.height};
                    out.shift = {};
                    if (offset != 0) {
                        for (auto& line : out.lines) {
                            line.rect[1] += offset;
                        }
                        for (const auto& c : cell.node->children) {
                            if (c.style.text(PropertyId::Display) == "none" || out_of_flow(c)) {
                                continue;
                            }
                            laid_out(c).box[1] += offset;
                        }
                    }
                }
            }
            auto& out = laid_out(table);
            out.box[2] = used;
            out.box[3] = height;
            out.lines.clear();
            out.fragments.clear();
            const float baseline = rows.empty() ? height + e.margin[2] : rows.front().y + rows.front().baseline;
            context_baselines[&table] = baseline;
            context_first_baselines[&table] = baseline;
            e.width = used;
            e.height = height;
            e.baseline = baseline;
            return e;
        }

        // Lays out an atomic inline's subtree, shrunk to fit the available width, and returns its extents; its box
        // is placed later.
        SceneAtomicExtent lay_out_atomic(const SceneNode& node, float available) {
            SceneAtomicExtent e;
            const float base = std::isfinite(available) ? available : 0;
            e.margin = {edge_length(node.style, PropertyId::MarginTop, base),
                        edge_length(node.style, PropertyId::MarginRight, base),
                        edge_length(node.style, PropertyId::MarginBottom, base),
                        edge_length(node.style, PropertyId::MarginLeft, base)};
            if (scene_ruby(node)) {
                return lay_out_ruby(node, e);
            }
            if (node.style.text(PropertyId::Display, "inline") == "inline-table") {
                return lay_out_table(node, available, e);
            }
            LayoutOptions options;
            if (std::isfinite(available)) {
                options.fit_width = available;
            }
            layout_run(node, options);
            e.width = box_of(node)[2];
            e.height = box_of(node)[3];
            // Without lines, the bottom margin edge.
            e.baseline = baseline_of(node).value_or(e.height + e.margin[2]);
            return e;
        }

        SceneAtomicExtent atomic_extent(const SceneNode& node, float available) {
            const auto key = std::pair{&node, available};
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
        void place(const SceneNode& root, const SceneInlineLayout& layout, std::array<float, 2> origin,
                   float available) {
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
                        // Text that text-overflow hides is a line without text: it draws nothing but keeps its place.
                        if (p.node == &root) {
                            laid_out(root).lines.push_back({p.text, r, p.baseline});
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
                        lay_out_atomic(*p.node, available);
                        bounds[p.node].add(r);
                        break;
                }
            }
            const auto assign = [&](auto&& self, const SceneNode& parent, float px, float py) -> void {
                for (const auto& c : parent.children) {
                    if (c.style.text(PropertyId::Display) == "none" || (&parent == &root && out_of_flow(c))) {
                        continue;
                    }
                    // A node without pieces (an empty inline element) sits at its parent's origin.
                    std::array<float, 4> at{px, py, 0, 0};
                    if (const auto b = bounds.find(&c); b != bounds.end()) {
                        at = {b->second.left, b->second.top, b->second.right - b->second.left,
                              b->second.bottom - b->second.top};
                    }
                    auto& out = laid_out(c);
                    out.shift = {};
                    // A relatively positioned inline-level box moves with its content.
                    const auto relative = [&] {
                        if (c.style.text(PropertyId::Position, "static") != "relative") {
                            return;
                        }
                        if (c.style.text(PropertyId::Left, "auto") != "auto") {
                            out.box[0] += edge_length(c.style, PropertyId::Left, available);
                        } else if (c.style.text(PropertyId::Right, "auto") != "auto") {
                            out.box[0] -= edge_length(c.style, PropertyId::Right, available);
                        }
                        if (c.style.text(PropertyId::Top, "auto") != "auto") {
                            out.box[1] += edge_length(c.style, PropertyId::Top, available);
                        } else if (c.style.text(PropertyId::Bottom, "auto") != "auto") {
                            out.box[1] -= edge_length(c.style, PropertyId::Bottom, available);
                        }
                    };
                    if (scene_atomic_inline(c)) {
                        out.box[0] = at[0] - px;
                        out.box[1] = at[1] - py;
                        relative();
                        continue;
                    }
                    out.box = {at[0] - px, at[1] - py, at[2], at[3]};
                    if (const auto l = lines.find(&c); l != lines.end()) {
                        for (auto line : l->second) {
                            line.rect[0] -= at[0];
                            line.rect[1] -= at[1];
                            out.lines.push_back(std::move(line));
                        }
                    }
                    if (const auto f = fragments.find(&c); f != fragments.end()) {
                        for (auto fragment : f->second) {
                            fragment.rect[0] -= at[0];
                            fragment.rect[1] -= at[1];
                            out.fragments.push_back(fragment);
                        }
                    }
                    relative();
                    self(self, c, at[0], at[1]);
                }
            };
            assign(assign, root, 0, 0);
        }


        // A text field's lines (SceneLine::start, SceneLine::stops): where each starts in the node's text, and its caret
        // stops, from its text shaped as the node's style shapes it. A field without text has one line of none at the
        // start of its content box (centered in it, an input's), as tall as its font's line box, for its caret.
        void field_lines(const SceneNode& root) {
            auto& out = laid_out(root);
            if (out.lines.empty() && root.text.empty()) {
                const auto& s = root.style;
                const auto& box = out.box;
                const float base = box[2];
                const float left = edge_length(s, PropertyId::BorderLeftWidth, base) +
                                   edge_length(s, PropertyId::PaddingLeft, base);
                const float right = edge_length(s, PropertyId::BorderRightWidth, base) +
                                    edge_length(s, PropertyId::PaddingRight, base);
                const float top =
                    edge_length(s, PropertyId::BorderTopWidth, base) + edge_length(s, PropertyId::PaddingTop, base);
                const float bottom = edge_length(s, PropertyId::BorderBottomWidth, base) +
                                     edge_length(s, PropertyId::PaddingBottom, base);
                const auto strut = text->strut(scene_font(s));
                const float height = strut.above + strut.below;
                const float width = std::max(0.f, box[2] - left - right);
                const auto align = s.text(PropertyId::TextAlign, "left");
                const float x = left + (align == "center" ? width / 2 : align == "right" || align == "end" ? width : 0);
                const float y = root.field->state->multiline
                                    ? top
                                    : top + (std::max(0.f, box[3] - top - bottom) - height) / 2;
                SceneLine line;
                line.rect = {x, y, 0, height};
                line.baseline = strut.above;
                line.stops = {{0, 0.f}};
                // The field's baseline is its line's, as with text.
                context_baselines[&root] = y + strut.above;
                out.lines.push_back(std::move(line));
                return;
            }
            std::size_t from = 0;
            for (auto& line : out.lines) {
                const auto at = root.text.find(line.text, from);
                line.start = std::uint32_t(at == std::string::npos ? from : at);
                if (at != std::string::npos) {
                    from = at + line.text.size();
                }
                const auto run = text->shape(line.text, root.style);
                line.stops = scene_field_stops(*run, line.text, line.start);
            }
        }

        // The adapter's text system for a run's inline contexts: each context's content (kept for its root's storage,
        // else its own), its lines at each width asked (memoized), its min-content width and whether it wraps. f gets
        // the measurer and the lines of a context at a width.
        template <class F>
        decltype(auto) with_text(LayoutRun& run, F&& f) {
            const auto atomic = [&](const SceneNode& node, float available) {
                return atomic_extent(node, available);
            };
            // A context's content and lines: those kept for its root's storage, else its own.
            const auto content = [&](std::size_t i) -> InlineContext& {
                auto& c = run.contexts.at(i);
                if (!c.state) {
                    auto* state = &c.own;
                    if (c.handle) {
                        auto& kept = kept_contexts[c.handle.get()];
                        kept.root = c.handle;
                        state = &kept.context;
                    }
                    if (!state->content) {
                        state->content = std::make_unique<SceneInlineContent>(*text, *c.root);
                    }
                    c.state = state;
                }
                return *c.state;
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
                auto& c = content(i);
                if (!c.min_content) {
                    c.min_content = c.content->min_content(atomic);
                }
                return *c.min_content;
            };
            const auto wraps = [&](std::size_t i) {
                return content(i).content->breakable();
            };
            return f(TextMeasurer{lines, min_content, wraps}, at);
        }

        // The min-content and max-content widths of a node's border box, laid out with `style` (a table cell's, its
        // width auto): percentages in its content resolve against nothing.
        IntrinsicWidths intrinsic_widths(const SceneNode& node, const SceneStyle& style) {
            LayoutRun run;
            run.top_style = &style;
            collect_boxes(node, LayoutBox::root, run, std::nullopt);
            return with_text(run, [&](const TextMeasurer& measurer, const auto&) {
                return layout_intrinsic_widths(run.boxes, measurer);
            });
        }

        // Lays out the subtree of `top` and writes the boxes, lines and fragments of its nodes; options size `top`.
        // `scene_root`: top is the scene's root, the containing block of fixed boxes without a transformed ancestor.
        // `top_style`: the values top is laid out with, instead of its own (a table cell at its column's width).
        void layout_run(const SceneNode& top, const LayoutOptions& options, bool scene_root = false,
                        const SceneStyle* top_style = nullptr) {
            LayoutRun run;
            run.top_style = top_style;
            collect_boxes(top, LayoutBox::root, run, scene_root ? std::optional<std::size_t>(0) : std::nullopt);
            with_text(run, [&](const TextMeasurer& measurer, const auto& at) {
                place_run(run, layout_boxes(run.boxes, measurer, options), at);
            });
        }

        // Writes a laid-out run's boxes, and places its contexts' lines at their final widths.
        template <class At>
        void place_run(LayoutRun& run, const std::vector<BoxGeometry>& geometry, const At& at) {
            for (std::size_t i = 0; i < run.boxes.size(); ++i) {
                if (const auto* n = run.targets[i]) {
                    const auto& g = geometry[i];
                    auto& out = laid_out(*n);
                    out.box = {g.left, g.top, g.width, g.height};
                    out.shift = {};
                }
            }
            // Fixed boxes in their parent's coordinates: no box between them and their containing block has a
            // transform.
            for (const auto& [i, parent] : run.fixed) {
                auto& box = laid_out(*run.targets[i]).box;
                box[0] = geometry[i].x - geometry[parent].x;
                box[1] = geometry[i].y - geometry[parent].y;
            }
            constexpr float unbounded = std::numeric_limits<float>::infinity();
            for (auto& [i, c] : run.contexts) {
                const auto& root = *c.root;
                std::array<float, 2> origin{};
                float available{};
                float shift_x{};
                float shift_y{};
                if (c.anonymous) {
                    origin = {geometry[i].left, geometry[i].top};
                    available = geometry[i].width;
                } else {
                    const auto& s = root.style;
                    const auto& root_box = box_of(root);
                    const float base = root_box[2];
                    const float left = edge_length(s, PropertyId::BorderLeftWidth, base) +
                                       edge_length(s, PropertyId::PaddingLeft, base);
                    const float right = edge_length(s, PropertyId::BorderRightWidth, base) +
                                        edge_length(s, PropertyId::PaddingRight, base);
                    const float over =
                        edge_length(s, PropertyId::BorderTopWidth, base) + edge_length(s, PropertyId::PaddingTop, base);
                    const float bottom = edge_length(s, PropertyId::BorderBottomWidth, base) +
                                         edge_length(s, PropertyId::PaddingBottom, base);
                    origin = {left, over};
                    available = std::max(0.f, root_box[2] - left - right);
                    const auto d = s.text(PropertyId::Display, "block");
                    if (d == "flex" || d == "inline-flex") {
                        // The text is an anonymous flex item: as wide as its widest line up to the content box,
                        // placed by justify-content and align-items.
                        available = std::min(available, at(i, unbounded).width);
                        const float free_x = std::max(0.f, root_box[2] - left - right) - available;
                        const float free_y = std::max(0.f, root_box[3] - over - bottom) - at(i, available).height;
                        const auto justify = s.text(PropertyId::JustifyContent, "flex-start");
                        const auto align = s.text(PropertyId::AlignItems, "stretch");
                        shift_x = justify == "center"                         ? free_x / 2
                                  : justify == "flex-end" || justify == "end" ? free_x
                                                                              : 0;
                        shift_y = align == "center" ? free_y / 2 : align == "flex-end" || align == "end" ? free_y : 0;
                    }
                    // A text field's text scrolls (dom/text_field.hpp): an input's left, a textarea's up, no further
                    // than its text and the caret after it reach.
                    if (root.field) {
                        const auto& state = *root.field->state;
                        const float content_w = std::max(0.f, root_box[2] - left - right);
                        const float content_h = std::max(0.f, root_box[3] - over - bottom);
                        const float reach_x =
                            at(i, state.multiline ? available : unbounded).width + constants::scene::field_caret_px;
                        shift_x -= std::clamp(state.scroll_x, 0.f, std::max(0.f, reach_x - content_w));
                        shift_y -= std::clamp(state.scroll_y, 0.f,
                                              std::max(0.f, at(i, available).height - content_h));
                    }
                }
                const auto& layout = at(i, available);
                origin[0] += shift_x;
                origin[1] += shift_y;
                if (layout.lines) {
                    context_baselines[&root] = origin[1] + layout.last_baseline;
                    context_first_baselines[&root] = origin[1] + layout.baseline;
                }
                place(root, layout, origin, available);
            }
            for (std::size_t i = 0; i < run.boxes.size(); ++i) {
                if (const auto* n = run.targets[i]) {
                    if (n->field) {
                        field_lines(*n);
                    }
                    scroll_content(*n);
                }
            }
        }

        // A scroll container just laid out (its children's boxes where layout put them): the size of its content, from
        // its padding box's start edges to the far edges of its children's margin boxes and its end padding (CSS
        // Overflow 3's scrollable overflow, without descendants that overflow their own boxes), and the scroll
        // position its element asks for, clamped to that content, by which its children's boxes move (from where layout
        // put them: a box layout did not place again moves by the difference). Its own lines stay.
        void scroll_content(const SceneNode& n) {
            auto& out = laid_out(n);
            const auto overflow = n.style.text(PropertyId::Overflow, "visible");
            if (overflow == "visible" || overflow == "clip" || n.space == constants::svg_namespace_uri) {
                out.scrolled = {};
                return;
            }
            const auto& s = n.style;
            const auto box = out.box;
            const float base = box[2];
            const float left = edge_length(s, PropertyId::BorderLeftWidth, base);
            const float top = edge_length(s, PropertyId::BorderTopWidth, base);
            const float client_w = std::max(0.f, box[2] - left - edge_length(s, PropertyId::BorderRightWidth, base));
            const float client_h = std::max(0.f, box[3] - top - edge_length(s, PropertyId::BorderBottomWidth, base));
            float right = client_w;
            float bottom = client_h;
            const float end_x = edge_length(s, PropertyId::PaddingRight, base);
            const float end_y = edge_length(s, PropertyId::PaddingBottom, base);
            for (const auto& c : n.children) {
                if (c.style.text(PropertyId::Display, c.tag == "#text" ? "inline" : "block") == "none") {
                    continue;
                }
                const auto& placed = laid_out(c);
                const bool is_text = c.tag == "#text";
                const float margin_x =
                    is_text ? 0.f : std::max(0.f, edge_length(c.style, PropertyId::MarginRight, base));
                const float margin_y =
                    is_text ? 0.f : std::max(0.f, edge_length(c.style, PropertyId::MarginBottom, base));
                right = std::max(right, placed.box[0] + placed.shift[0] + placed.box[2] + margin_x - left + end_x);
                bottom = std::max(bottom, placed.box[1] + placed.shift[1] + placed.box[3] + margin_y - top + end_y);
            }
            for (const auto& line : out.lines) {
                right = std::max(right, line.rect[0] + line.rect[2] - left + end_x);
                bottom = std::max(bottom, line.rect[1] + line.rect[3] - top + end_y);
            }
            const float x = std::clamp(n.scroll[0], 0.f, std::max(0.f, right - client_w));
            const float y = std::clamp(n.scroll[1], 0.f, std::max(0.f, bottom - client_h));
            out.scrolled = {x, y, right, bottom};
            for (const auto& c : n.children) {
                auto& placed = laid_out(c);
                if (placed.shift[0] != x || placed.shift[1] != y) {
                    placed.box[0] += placed.shift[0] - x;
                    placed.box[1] += placed.shift[1] - y;
                    placed.shift = {x, y};
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
                scene_scroll_geometry(n, e);
                out.push_back(e);
            }
            if (n.space == constants::svg_namespace_uri) {
                return;
            }
            for (const auto& c : n.children) {
                list_elements(c, ax, ay, out);
            }
        }

        // Lays out the scene's root in the viewport: parent-local boxes, lines and fragments on the scene nodes,
        // written where they changed, and element geometry in tree order.
        void layout_frame(SceneNode& root, float w, float h, std::vector<ElementGeometry>& elements) {
            atomic_memo.clear();
            context_baselines.clear();
            context_first_baselines.clear();
            laid.clear();
            LayoutOptions options;
            options.viewport = std::array{w, h};
            try {
                layout_run(root, options, true);
                if (const auto it = laid.find(&root); it != laid.end()) {
                    root.box = it->second.box;
                    root.lines = std::move(it->second.lines);
                    root.fragments = std::move(it->second.fragments);
                    root.scrolled = it->second.scrolled;
                    root.scroll_shift = it->second.shift;
                }
                for (std::size_t k = 0; k < root.children.size(); ++k) {
                    const auto child = std::as_const(root.children).handle(k);
                    if (auto laid_child = apply_layout(child); laid_child != child) {
                        root.children.set(k, std::move(laid_child));
                    }
                }
            } catch (...) {
                laid.clear();
                throw;
            }
            laid.clear();
            context_baselines.clear();
            context_first_baselines.clear();
            std::vector<std::array<float, 4>> clips;
            check_fixed_clips(root, {0, 0}, clips, {0, 0, w, h});
            atomic_memo.clear();
            sweep_contexts();
            elements.clear();
            if (element_geometry) {
                list_elements(root, 0, 0, elements);
            }
        }

        // A box laid out apart from the rest of its frame, as a full layout lays it out, given its parent's box
        // (layout_boundary): a shown HTML absolute box with an inset on each axis (no static position)...
        static bool apart(const SceneNode& n) {
            const auto& s = n.style;
            const auto set = [&](PropertyId id) {
                return s.text(id, "auto") != "auto";
            };
            return (n.space.empty() || n.space == constants::xhtml_namespace_uri) &&
                   s.text(PropertyId::Position, "static") == "absolute" &&
                   s.text(PropertyId::Display, "inline") != "none" &&
                   (set(PropertyId::Left) || set(PropertyId::Right)) &&
                   (set(PropertyId::Top) || set(PropertyId::Bottom));
        }

        // ... in a parent that is its containing block (the root, or a shown relative or absolute HTML box laid out
        // as a box of boxes), whose padding and borders are lengths of their own (no percentage of its own parent).
        static bool holds_apart(const SceneNode& parent, bool root) {
            const auto& s = parent.style;
            const auto position = s.text(PropertyId::Position, "static");
            if (!(parent.space.empty() || parent.space == constants::xhtml_namespace_uri) ||
                s.text(PropertyId::Display, "inline") == "none" || !parent.text.empty() ||
                (!root && position != "relative" && position != "absolute") || scene_inline_children(parent)) {
                return false;
            }
            for (const auto id : {PropertyId::PaddingTop, PropertyId::PaddingRight, PropertyId::PaddingBottom,
                                  PropertyId::PaddingLeft, PropertyId::BorderTopWidth, PropertyId::BorderRightWidth,
                                  PropertyId::BorderBottomWidth, PropertyId::BorderLeftWidth}) {
                const auto v = s.text(id, "0");
                if (v.find('%') != std::string_view::npos || v.find('(') != std::string_view::npos) {
                    return false;
                }
            }
            return true;
        }

        // Whether a subtree holds a fixed box, which a layout of its own cannot place.
        static bool holds_fixed(const SceneNode& n) {
            if (n.style.text(PropertyId::Position, "static") == "fixed" &&
                n.style.text(PropertyId::Display, "inline") != "none") {
                return true;
            }
            return std::any_of(n.children.begin(), n.children.end(), [](const SceneNode& c) {
                return holds_fixed(c);
            });
        }

        // Lays out b, a box laid out apart (apart) in its containing block cb (holds_apart), as a full layout lays
        // it out: the only child of a box standing for cb, of cb's laid out border box and its edges. b's subtree
        // takes its output in `laid`.
        void layout_boundary(const SceneNode& cb, const Handle& b) {
            SceneNode container;
            container.identity = cb.identity;
            container.tag = cb.tag;
            container.space = cb.space;
            container.style = cb.style;
            container.style.set(PropertyId::BoxSizing, "border-box");
            for (const auto id :
                 {PropertyId::MinWidth, PropertyId::MaxWidth, PropertyId::MinHeight, PropertyId::MaxHeight}) {
                container.style.erase(id);
            }
            container.box = cb.box;
            container.children.push_back(b);
            LayoutOptions options;
            options.viewport = std::array{cb.box[2], cb.box[3]};
            try {
                layout_run(container, options);
            } catch (...) {
                laid.erase(&container);
                throw;
            }
            laid.erase(&container);
            scroll_content(cb);
        }

        // Lays out the frame being built again around its layout points alone: for each point, the nearest box at
        // or above it below the root laid out apart (one that was laid out apart in the last frame too, for the
        // point itself, whose own values changed), the subtrees of those boxes outside one another. False, with
        // nothing done, where the whole frame is to be laid out.
        bool layout_part(SceneNode& root, std::vector<ElementGeometry>& elements) {
            if (layout_points.empty()) {
                return false;
            }
            std::vector<std::vector<std::uint32_t>> parts;
            std::vector<const SceneNode*> chain;
            for (const auto& point : layout_points) {
                chain.assign(1, &root);
                for (const auto k : point.path) {
                    if (k >= chain.back()->children.size()) {
                        return false;
                    }
                    chain.push_back(&chain.back()->children[k]);
                }
                std::size_t at = chain.size() - 1;
                for (; at >= 1; --at) {
                    if (apart(*chain[at]) && holds_apart(*chain[at - 1], at == 1) &&
                        (at + 1 < chain.size() || (point.old && apart(*point.old)))) {
                        break;
                    }
                }
                if (at == 0) {
                    return false;
                }
                for (std::size_t j = 0; j < at; ++j) {
                    if (chain[j]->style.text(PropertyId::Display, "inline") == "none") {
                        return false;
                    }
                }
                parts.emplace_back(point.path.begin(), point.path.begin() + std::ptrdiff_t(at));
            }
            // A part inside another is laid out with it.
            std::sort(parts.begin(), parts.end());
            std::vector<std::vector<std::uint32_t>> outer;
            for (auto& part : parts) {
                if (!outer.empty() && part.size() >= outer.back().size() &&
                    std::equal(outer.back().begin(), outer.back().end(), part.begin())) {
                    continue;
                }
                outer.push_back(std::move(part));
            }
            const auto node_at = [&](const std::vector<std::uint32_t>& path, std::size_t depth) -> const SceneNode& {
                const SceneNode* n = &root;
                for (std::size_t j = 0; j < depth; ++j) {
                    n = &n->children[path[j]];
                }
                return *n;
            };
            for (const auto& part : outer) {
                if (holds_fixed(node_at(part, part.size()))) {
                    return false;
                }
            }
            atomic_memo.clear();
            context_baselines.clear();
            context_first_baselines.clear();
            laid.clear();
            try {
                for (const auto& part : outer) {
                    const auto& cb = node_at(part, part.size() - 1);
                    layout_boundary(cb, cb.children.handle(part.back()));
                }
                // Written back along the parts' paths: each part, and where its parent is a scroll container, the
                // parent's scroll position and content and its other children's shifts (scroll_content).
                for (const auto& part : outer) {
                    const auto& cb = node_at(part, part.size() - 1);
                    const auto before = cb.children.handle(part.back());
                    auto laid_part = apply_layout(before);
                    const auto own = laid.find(&cb);
                    const bool scrolled = own != laid.end() && own->second.scrolled != cb.scrolled;
                    std::vector<std::pair<std::size_t, Handle>> moved;
                    for (std::size_t k = 0; own != laid.end() && k < cb.children.size(); ++k) {
                        if (k == part.back()) {
                            continue;
                        }
                        const auto child = cb.children.handle(k);
                        if (auto laid_child = apply_layout(child); laid_child != child) {
                            moved.emplace_back(k, std::move(laid_child));
                        }
                    }
                    if (laid_part == before && !scrolled && moved.empty()) {
                        continue;
                    }
                    SceneNode* n = &root;
                    for (std::size_t j = 0; j + 1 < part.size(); ++j) {
                        n = &n->children.edit(part[j]);
                    }
                    if (laid_part != before) {
                        n->children.set(part.back(), std::move(laid_part));
                    }
                    if (scrolled) {
                        n->scrolled = own->second.scrolled;
                    }
                    for (auto& [k, child] : moved) {
                        n->children.set(k, std::move(child));
                    }
                }
            } catch (...) {
                laid.clear();
                throw;
            }
            laid.clear();
            context_baselines.clear();
            context_first_baselines.clear();
            // The fixed boxes and the clips between them and their containing blocks lie outside the parts, as they
            // were (check_fixed_clips held for them).
            atomic_memo.clear();
            sweep_contexts();
            elements.clear();
            if (element_geometry) {
                list_elements(root, 0, 0, elements);
            }
            return true;
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
                                      PropertyId::BorderImageOutset,
                                      PropertyId::BorderImageRepeat,
                                      PropertyId::BorderImageSlice,
                                      PropertyId::BorderImageSource,
                                      PropertyId::BorderImageWidth,
                                      PropertyId::BorderRadius,
                                      PropertyId::BorderTopLeftRadius,
                                      PropertyId::BorderTopRightRadius,
                                      PropertyId::BorderBottomRightRadius,
                                      PropertyId::BorderBottomLeftRadius,
                                      PropertyId::BoxShadow,
                                      PropertyId::TextShadow,
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
                                      PropertyId::Visibility,
                                      PropertyId::OutlineColor,
                                      PropertyId::OutlineOffset,
                                      PropertyId::OutlineStyle,
                                      PropertyId::OutlineWidth,
                                      PropertyId::TextDecorationColor,
                                      PropertyId::TextDecorationLine,
                                      PropertyId::TextDecorationStyle,
                                      PropertyId::TextDecorationThickness,
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

    } // namespace

    SceneSolver::Work SceneSolver::work() const {
        impl_->owner.require(Diag::SceneOwner);
        return impl_->work;
    }

    SceneSolver::SceneSolver(std::string s, std::string k, SceneText& text, bool incremental)
        : SceneSolver(std::move(s), std::move(k), text, incremental, false) {
    }

    SceneSolver::SceneSolver(std::string s, std::string k, SceneText& text, bool incremental, bool forgiving)
        : impl_(std::make_unique<Impl>()) {
        impl_->sheet = std::move(s);
        impl_->incremental = incremental;
        impl_->forgiving = forgiving;
        impl_->compiled = std::make_unique<CompiledStyle>(impl_->sheet, scene_style_profile(forgiving));
        if (forgiving) {
            impl_->keyframes = parse_keyframes(k + "\n" + impl_->compiled->keyframes(), &impl_->warnings);
        } else {
            impl_->keyframes = parse_keyframes(std::move(k));
        }
        impl_->text = &text;
    }

    const std::vector<FontFaceRule>& SceneSolver::font_faces() const {
        return impl_->compiled->font_faces();
    }

    void SceneSolver::set_text(SceneText& text) {
        impl_->owner.require(Diag::SceneOwner);
        impl_->text = &text;
    }

    std::vector<std::string> SceneSolver::take_warnings() {
        auto& i = *impl_;
        i.owner.require(Diag::SceneOwner);
        auto out = std::move(i.warnings);
        i.warnings.clear();
        auto style = i.compiled->take_warnings();
        out.insert(out.end(), std::make_move_iterator(style.begin()), std::make_move_iterator(style.end()));
        return out;
    }

    SceneSolver::~SceneSolver() = default;

    void SceneSolver::set_profiling(bool enabled) {
        impl_->owner.require(Diag::SceneOwner);
        impl_->profiling = enabled;
    }

    void SceneSolver::set_element_geometry(bool on) {
        impl_->owner.require(Diag::SceneOwner);
        impl_->element_geometry = on;
    }

    void SceneSolver::set_effects(const SceneEffects* effects) {
        impl_->owner.require(Diag::SceneOwner);
        impl_->effects = effects;
    }

    void SceneSolver::set_images(SceneImages* images) {
        impl_->owner.require(Diag::SceneOwner);
        impl_->images = images;
    }

    void SceneSolver::set_particles(const SceneParticles* particles) {
        impl_->owner.require(Diag::SceneOwner);
        impl_->particles = particles;
    }

    void SceneSolver::emit_particles(NodeIdentity element, std::string_view name, const SceneBurst& burst) {
        auto& i = *impl_;
        i.owner.require(Diag::SceneOwner);
        if (!i.particles) {
            fail("weft.emitParticles: " + std::string(name) + " without registered particles");
        }
        // An unknown name throws.
        i.particles->resolve(name, {});
        if (burst.count < 1 || burst.count > constants::max_particles_per_emission || !std::isfinite(burst.duration) ||
            burst.duration < 0 ||
            (burst.point && (!std::isfinite((*burst.point)[0]) || !std::isfinite((*burst.point)[1])))) {
            fail("weft.emitParticles: a count of 1 to " + std::to_string(constants::max_particles_per_emission) +
                 ", a duration of 0 or more and a finite point");
        }
        if (i.pending_count >= constants::scene::max_pending_particle_bursts) {
            fail("weft.emitParticles: more than " + std::to_string(constants::scene::max_pending_particle_bursts) +
                 " bursts before a frame");
        }
        i.pending[identity(element)].push_back({std::string(name), burst});
        ++i.pending_count;
    }

    void SceneSolver::reset() {
        impl_->owner.require(Diag::SceneOwner);
        impl_->style_memo.clear();
        impl_->states.clear();
        impl_->pending.clear();
        impl_->pending_count = 0;
        impl_->flights.clear();
        impl_->hidden = false;
        impl_->interpolation = {};
        impl_->paint_cache = {};
        impl_->style_input = {};
        impl_->cascade.reset();
        impl_->computed.reset();
        impl_->previous_dom.reset();
        impl_->retained.reset();
        impl_->retained_elements.clear();
        impl_->kept_contexts.clear();
        impl_->built = impl_->expanded = 0;
        impl_->active.clear();
        impl_->next_active.clear();
        impl_->marked.clear();
        impl_->seen.clear();
        impl_->removed.clear();
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

        struct Reading {
            const DomSnapshot*& document;

            ~Reading() {
                document = nullptr;
            }
        } reading{i.document};

        i.document = &d;
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
        i.hidden = false;
        i.frame_particles = 0;
        i.flights.clear();
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
        if (i.incremental) {
            i.update_incremental(d, f, w, h, time, viewport_moved, [&] {
                return clock();
            });
            return;
        }
        i.computed = compute_style(d, i.sheet, scene_style_profile(i.forgiving), w, h);
        ++i.work.style_evaluations;
        const auto build_start = clock();
        i.work.style_ms += ms(profile_start, build_start);
        i.build_into(f.root, d, *i.computed, nullptr);
        // Bursts on elements the frame did not show are dropped.
        i.pending.clear();
        i.pending_count = 0;
        if (i.frame_particles > constants::scene::max_particles_per_frame) {
            fail("particles: " + std::to_string(i.frame_particles) + " in a frame, more than " +
                 std::to_string(constants::scene::max_particles_per_frame));
        }
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
        i.work.geometry_changed = i.work.paint_changed = true;
        i.layout_frame(f.root, w, h, f.elements);
        ++i.work.layout_evaluations;
        if (!i.flights.empty()) {
            i.place_flights(f.root);
        }
        i.previous_width = w;
        i.previous_height = h;
        i.revision = next_revision.fetch_add(1, std::memory_order_relaxed);
        f.revision = i.revision;
        i.work.layout_ms += ms(layout_start, clock());
    }
} // namespace weft
