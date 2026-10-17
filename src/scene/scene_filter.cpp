#include "scene/scene_filter.hpp"

#include "base/css_scalar.hpp"
#include "base/diagnostic.hpp"
#include "constants/common.hpp"
#include "constants/scene.hpp"
#include "scene/scene.hpp"
#include "scene/scene_values.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <iterator>
#include <memory>
#include <numbers>
#include <string>
#include <unordered_map>
#include <variant>

namespace weft {
    namespace {
        using Matrix = paint::ColorMatrix;

        // a, then b: b times a, both 4 x 5 maps of straight RGBA with their offsets in the fifth column.
        Matrix then(const Matrix& a, const Matrix& b) {
            Matrix out{};
            for (unsigned r = 0; r < 4; ++r) {
                for (unsigned c = 0; c < 5; ++c) {
                    float v = c == 4 ? b[r * 5 + 4] : 0;
                    for (unsigned k = 0; k < 4; ++k) {
                        v += b[r * 5 + k] * a[k * 5 + c];
                    }
                    out[r * 5 + c] = v;
                }
            }
            return out;
        }

        // The matrix that maps r, g and b by m (row-major 3 x 3) plus offset, and keeps alpha.
        Matrix rgb(const std::array<float, 9>& m, const std::array<float, 3>& offset = {}) {
            Matrix out = paint::identity_color_matrix;
            for (unsigned r = 0; r < 3; ++r) {
                for (unsigned c = 0; c < 3; ++c) {
                    out[r * 5 + c] = m[r * 3 + c];
                }
                out[r * 5 + 4] = offset[r];
            }
            return out;
        }

        std::optional<float> number(std::string_view text) {
            const auto n = css::parse_number<float>(std::string(text),
                                                    static_cast<float>(constants::scene::max_numeric_magnitude));
            return n ? std::optional<float>(*n) : std::nullopt;
        }

        // The saturate matrix of amount a (Filter Effects 1, saturate() and feColorMatrix's saturate).
        Matrix saturation(float a) {
            return rgb({.213f + .787f * a, .715f - .715f * a, .072f - .072f * a, .213f - .213f * a, .715f + .285f * a,
                        .072f - .072f * a, .213f - .213f * a, .715f - .715f * a, .072f + .928f * a});
        }

        // The hue rotation of an angle in degrees (hue-rotate() and feColorMatrix's hueRotate).
        Matrix hue_rotation(double degrees) {
            const float c = float(std::cos(degrees * std::numbers::pi / 180));
            const float s = float(std::sin(degrees * std::numbers::pi / 180));
            return rgb({.213f + c * .787f - s * .213f, .715f - c * .715f - s * .715f, .072f - c * .072f + s * .928f,
                        .213f - c * .213f + s * .143f, .715f + c * .285f + s * .140f, .072f - c * .072f - s * .283f,
                        .213f - c * .213f - s * .787f, .715f - c * .715f + s * .715f, .072f + c * .928f + s * .072f});
        }

        constexpr std::string_view color_functions[]{"brightness", "contrast", "grayscale", "sepia",
                                                     "saturate",   "invert",   "opacity",   "hue-rotate"};

        bool color_function(std::string_view name) {
            return std::find(std::begin(color_functions), std::end(color_functions), name) != std::end(color_functions);
        }

        // A color function's matrix: its amount a number or a percentage, 1 when left out, at least 0; hue-rotate()'s
        // an angle. The reason it refuses the argument otherwise.
        std::variant<Matrix, std::string> color_matrix(std::string_view name, std::string_view argument) {
            std::optional<float> amount = 1.f;
            if (!argument.empty() && name != "hue-rotate") {
                amount = argument.ends_with('%') ? number(argument.substr(0, argument.size() - 1)) : number(argument);
                if (amount && argument.ends_with('%')) {
                    *amount /= 100;
                }
                if (!amount || !(*amount >= 0)) {
                    return std::string(name) + "() takes a number or a percentage of 0 or more";
                }
            }
            const float a = *amount;
            const float capped = std::min(a, 1.f);
            Matrix m = paint::identity_color_matrix;
            if (name == "brightness") {
                m = rgb({a, 0, 0, 0, a, 0, 0, 0, a});
            } else if (name == "contrast") {
                const float b = .5f - .5f * a;
                m = rgb({a, 0, 0, 0, a, 0, 0, 0, a}, {b, b, b});
            } else if (name == "grayscale") {
                const float k = 1 - capped;
                m = rgb({.2126f + .7874f * k, .7152f - .7152f * k, .0722f - .0722f * k, .2126f - .2126f * k,
                         .7152f + .2848f * k, .0722f - .0722f * k, .2126f - .2126f * k, .7152f - .7152f * k,
                         .0722f + .9278f * k});
            } else if (name == "sepia") {
                const float k = 1 - capped;
                m = rgb({.393f + .607f * k, .769f - .769f * k, .189f - .189f * k, .349f - .349f * k, .686f + .314f * k,
                         .168f - .168f * k, .272f - .272f * k, .534f - .534f * k, .131f + .869f * k});
            } else if (name == "saturate") {
                m = saturation(a);
            } else if (name == "invert") {
                m = rgb({1 - 2 * capped, 0, 0, 0, 1 - 2 * capped, 0, 0, 0, 1 - 2 * capped}, {capped, capped, capped});
            } else if (name == "opacity") {
                m[3 * 5 + 3] = capped;
            } else if (name == "hue-rotate") {
                // An angle; 0 may go without its unit.
                double degrees{};
                const std::pair<const char*, double> units[]{
                    {"deg", 1}, {"grad", .9}, {"rad", 180 / std::numbers::pi}, {"turn", 360}};
                bool parsed = argument.empty();
                for (const auto& [unit, factor] : units) {
                    if (!parsed && argument.ends_with(unit)) {
                        const auto n = number(argument.substr(0, argument.size() - std::string_view(unit).size()));
                        if (!n) {
                            return std::string("hue-rotate() takes an angle");
                        }
                        degrees = *n * factor;
                        parsed = true;
                    }
                }
                if (!parsed) {
                    const auto n = number(argument);
                    if (!n || *n != 0) {
                        return std::string("hue-rotate() takes an angle");
                    }
                }
                m = hue_rotation(degrees);
            }
            return m;
        }

        // A blur's length in px; 0 may go without its unit.
        std::optional<float> blur_length(std::string_view argument) {
            const auto digits = argument.ends_with("px") ? argument.substr(0, argument.size() - 2) : argument;
            const auto px = argument.empty() ? std::optional<float>(0.f) : number(digits);
            if (!px || (!argument.ends_with("px") && *px != 0)) {
                return std::nullopt;
            }
            return px;
        }

        // The functions of a filter value: name and argument of each, trimmed, in order; none when the value is not a
        // run of functions (a name, then an argument in balanced parentheses, quotes protecting what they hold).
        std::optional<std::vector<std::pair<std::string_view, std::string_view>>> functions(std::string_view text) {
            std::vector<std::pair<std::string_view, std::string_view>> out;
            std::size_t at{};
            while (at < text.size()) {
                if (std::isspace(static_cast<unsigned char>(text[at]))) {
                    ++at;
                    continue;
                }
                const auto open = text.find('(', at);
                if (open == std::string_view::npos) {
                    return std::nullopt;
                }
                const auto name = css::trim(text.substr(at, open - at));
                if (name.empty() || std::any_of(name.begin(), name.end(), [](char c) {
                        return std::isspace(static_cast<unsigned char>(c)) || c == ')';
                    })) {
                    return std::nullopt;
                }
                int depth{};
                char quote{};
                std::size_t close = std::string_view::npos;
                for (auto p = open; p < text.size(); ++p) {
                    const char c = text[p];
                    if (quote) {
                        quote = c == quote ? 0 : quote;
                    } else if (c == '\'' || c == '"') {
                        quote = c;
                    } else if (c == '(') {
                        ++depth;
                    } else if (c == ')' && --depth == 0) {
                        close = p;
                        break;
                    }
                }
                if (close == std::string_view::npos) {
                    return std::nullopt;
                }
                out.emplace_back(text.substr(at, open - at), text.substr(open + 1, close - open - 1));
                at = close + 1;
            }
            for (auto& [name, argument] : out) {
                name = std::string_view(name).substr(0, name.find_last_not_of(" \t\r\n") + 1);
                while (!argument.empty() && std::isspace(static_cast<unsigned char>(argument.front()))) {
                    argument.remove_prefix(1);
                }
                while (!argument.empty() && std::isspace(static_cast<unsigned char>(argument.back()))) {
                    argument.remove_suffix(1);
                }
            }
            return out;
        }

        // The id a url() names: url(#id), quoted or not; empty for another URL.
        std::string fragment(std::string_view argument) {
            if (argument.size() >= 2 && (argument.front() == '"' || argument.front() == '\'') &&
                argument.back() == argument.front()) {
                argument = argument.substr(1, argument.size() - 2);
            }
            return argument.size() > 1 && argument.front() == '#' ? std::string(argument.substr(1)) : std::string{};
        }

        std::string_view attribute(const AttributeMap* attributes, std::string_view name) {
            if (!attributes) {
                return {};
            }
            const auto found = attributes->find(name);
            return found == attributes->end() ? std::string_view{} : std::string_view(found->second);
        }

        // An SVG filter element's feColorMatrix primitives as matrices; the reason it refuses them otherwise.
        std::variant<std::vector<paint::FilterOp>, std::string> svg_filter(const SceneFilterElement& filter) {
            std::vector<paint::FilterOp> out;
            const auto space = [](std::string_view value) {
                const auto v = css::trim(value);
                if (v.empty() || v == "inherit") {
                    return std::optional<std::optional<bool>>(std::optional<bool>{});
                }
                if (v == "sRGB" || v == "auto") {
                    return std::optional<std::optional<bool>>(false);
                }
                if (v == "linearRGB") {
                    return std::optional<std::optional<bool>>(true);
                }
                return std::optional<std::optional<bool>>{};
            };
            const auto inherited = space(attribute(filter.attributes, "color-interpolation-filters"));
            if (!inherited) {
                return std::string("color-interpolation-filters of the filter element");
            }
            std::string previous;
            for (const auto& child : filter.children) {
                if (child.space != constants::svg_namespace_uri) {
                    return std::string("a filter primitive outside the SVG namespace");
                }
                if (child.tag == "desc" || child.tag == "title" || child.tag == "metadata") {
                    continue;
                }
                if (child.tag != "feColorMatrix") {
                    return std::string(child.tag) + " is not a filter primitive the scene takes";
                }
                const auto in = css::trim(attribute(child.attributes, "in"));
                if (!in.empty() && in != "SourceGraphic" && (previous.empty() || in != previous)) {
                    return std::string("feColorMatrix reads ") + in + ", not SourceGraphic or the primitive before";
                }
                previous = std::string(css::trim(attribute(child.attributes, "result")));
                const auto own = space(attribute(child.attributes, "color-interpolation-filters"));
                if (!own) {
                    return std::string("color-interpolation-filters of feColorMatrix");
                }
                paint::FilterOp op;
                op.kind = paint::FilterOp::Kind::Matrix;
                // linearRGB is the initial value.
                op.linear = own->value_or(inherited->value_or(true));
                const auto type = css::trim(attribute(child.attributes, "type"));
                auto values = std::string(attribute(child.attributes, "values"));
                std::replace(values.begin(), values.end(), ',', ' ');
                std::vector<float> numbers;
                for (const auto& part : css::split(values, ' ', {.quotes = false}).parts) {
                    const auto n = number(part);
                    if (!n) {
                        return std::string("feColorMatrix values of other than numbers");
                    }
                    numbers.push_back(*n);
                }
                if (type.empty() || type == "matrix") {
                    if (!numbers.empty() && numbers.size() != op.matrix.size()) {
                        return std::string("feColorMatrix of type matrix takes 20 values");
                    }
                    std::copy(numbers.begin(), numbers.end(), op.matrix.begin());
                } else if (type == "saturate") {
                    if (numbers.size() > 1 || (numbers.size() == 1 && !(numbers[0] >= 0))) {
                        return std::string("feColorMatrix of type saturate takes one value of 0 or more");
                    }
                    op.matrix = saturation(numbers.empty() ? 1 : numbers[0]);
                } else if (type == "hueRotate") {
                    if (numbers.size() > 1) {
                        return std::string("feColorMatrix of type hueRotate takes one value");
                    }
                    op.matrix = hue_rotation(numbers.empty() ? 0 : numbers[0]);
                } else if (type == "luminanceToAlpha") {
                    op.matrix = Matrix{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, .2125f, .7154f, .0721f, 0, 0};
                } else {
                    return std::string("feColorMatrix of type ") + type;
                }
                out.push_back(op);
            }
            if (out.empty()) {
                // A filter of no primitives draws nothing of the element.
                paint::FilterOp nothing;
                nothing.kind = paint::FilterOp::Kind::Matrix;
                nothing.matrix = {};
                out.push_back(nothing);
            }
            return out;
        }
    } // namespace

    Result<std::optional<SceneBackdropFilter>> scene_backdrop_filter(std::string_view value) {
        const auto text = css::trim(value);
        if (text.empty() || text == "none") {
            return std::optional<SceneBackdropFilter>{};
        }
        const auto refuse = [&](const std::string& why) {
            return make_diagnostic(Diag::SceneProfile, "backdrop-filter " + text + ": " + why);
        };
        SceneBackdropFilter out;
        bool blurred{};
        std::size_t at{};
        while (at < text.size()) {
            if (text[at] == ' ') {
                ++at;
                continue;
            }
            const auto open = text.find('(', at);
            const auto close = open == std::string::npos ? open : text.find(')', open);
            if (close == std::string::npos) {
                return refuse("filter functions only");
            }
            const auto name = css::trim(std::string_view(text).substr(at, open - at));
            const auto argument = css::trim(std::string_view(text).substr(open + 1, close - open - 1));
            at = close + 1;
            if (name == "blur") {
                if (blurred) {
                    return refuse("one blur()");
                }
                blurred = true;
                const auto px = blur_length(argument);
                if (!px || !(*px >= 0) || *px > constants::max_backdrop_blur) {
                    return refuse("blur() takes a length of 0 to " + std::to_string(int(constants::max_backdrop_blur)) +
                                  "px");
                }
                out.blur = *px;
                continue;
            }
            if (!color_function(name)) {
                return refuse(std::string(name) + "() is not a filter function the scene takes");
            }
            const auto m = color_matrix(name, argument);
            if (const auto* why = std::get_if<std::string>(&m)) {
                return refuse(*why);
            }
            auto& into = blurred ? out.post : out.pre;
            into = then(into, std::get<Matrix>(m));
        }
        return std::optional<SceneBackdropFilter>(out);
    }

    SceneFilterResolve scene_filter_resolver(const SceneNode& root) {
        // The elements by id, the first of each in tree order, found once.
        auto index = std::make_shared<std::unordered_map<std::string, const SceneNode*>>();
        auto built = std::make_shared<bool>(false);
        return [&root, index, built](std::string_view id) -> std::optional<SceneFilterElement> {
            if (!*built) {
                const auto add = [&](auto&& self, const SceneNode& n) -> void {
                    if (const auto found = n.attributes.find("id"); found != n.attributes.end()) {
                        index->try_emplace(found->second, &n);
                    }
                    for (const auto& child : n.children) {
                        self(self, child);
                    }
                };
                add(add, root);
                *built = true;
            }
            const auto found = index->find(std::string(id));
            if (found == index->end()) {
                return std::nullopt;
            }
            const auto& n = *found->second;
            SceneFilterElement out{n.tag, n.space, &n.attributes, {}};
            for (const auto& child : n.children) {
                if (child.tag != "#text") {
                    out.children.push_back({child.tag, child.space, &child.attributes});
                }
            }
            return out;
        };
    }

    SceneFilterResolve scene_filter_resolver(const DomSnapshot& root) {
        return [&root](std::string_view id) -> std::optional<SceneFilterElement> {
            const auto find = [&](auto&& self, const DomSnapshot& n) -> const DomSnapshot* {
                if (n.type == constants::element_node_type) {
                    if (const auto found = n.attributes.find("id");
                        found != n.attributes.end() && found->second == id) {
                        return &n;
                    }
                }
                for (const auto& child : n.children) {
                    if (const auto* hit = self(self, child)) {
                        return hit;
                    }
                }
                return nullptr;
            };
            const auto* n = find(find, root);
            if (!n) {
                return std::nullopt;
            }
            SceneFilterElement out{n->local_name, n->namespace_uri, &n->attributes, {}};
            for (const auto& child : n->children) {
                if (child.type == constants::element_node_type) {
                    out.children.push_back({child.local_name, child.namespace_uri, &child.attributes});
                }
            }
            return out;
        };
    }

    Result<std::vector<paint::FilterOp>> scene_filter(std::string_view value, const StyleValues& style,
                                                      const SceneFilterResolve& resolve) {
        const auto text = css::trim(value);
        std::vector<paint::FilterOp> out;
        if (text.empty() || text == "none") {
            return out;
        }
        const auto refuse = [&](const std::string& why) {
            return make_diagnostic(Diag::SceneProfile, "filter " + text + ": " + why);
        };
        const auto listed = functions(text);
        if (!listed) {
            return refuse("filter functions only");
        }
        if (listed->size() > constants::max_filter_functions) {
            return refuse("at most " + std::to_string(constants::max_filter_functions) + " functions");
        }
        const auto add = [&](const paint::FilterOp& op) {
            out.push_back(op);
        };
        for (const auto& [name, argument] : *listed) {
            if (name == "blur") {
                const auto px = blur_length(argument);
                if (!px || !(*px >= 0) || *px > constants::max_filter_blur) {
                    return refuse("blur() takes a length of 0 to " + std::to_string(int(constants::max_filter_blur)) +
                                  "px");
                }
                if (*px > 0) {
                    paint::FilterOp op;
                    op.kind = paint::FilterOp::Kind::Blur;
                    op.blur = *px;
                    add(op);
                }
                continue;
            }
            if (name == "drop-shadow") {
                // A color before or after two or three lengths: the offset and the standard deviation.
                auto parts = css::split(argument, ' ', {.quotes = false}).parts;
                const auto length = [](const std::string& t) {
                    return !t.empty() && (std::isdigit(static_cast<unsigned char>(t[0])) || t[0] == '.' ||
                                          t[0] == '-' || t[0] == '+');
                };
                std::string color = "currentcolor";
                if (!parts.empty() && !length(parts.front())) {
                    color = parts.front();
                    parts.erase(parts.begin());
                } else if (!parts.empty() && !length(parts.back())) {
                    color = parts.back();
                    parts.pop_back();
                }
                if (parts.size() < 2 || parts.size() > 3 || !std::all_of(parts.begin(), parts.end(), length) ||
                    std::any_of(parts.begin(), parts.end(), [](const std::string& p) {
                        return p.ends_with('%');
                    })) {
                    return refuse("drop-shadow() takes a color and two or three lengths");
                }
                paint::FilterOp op;
                op.kind = paint::FilterOp::Kind::DropShadow;
                try {
                    const float em =
                        scene_values::len(scene_values::get(style, PropertyId::FontSize, constants::default_font_size));
                    op.color = scene_values::shadow_color(color, style);
                    op.x = scene_values::len(parts[0], em);
                    op.y = scene_values::len(parts[1], em);
                    op.blur = parts.size() > 2 ? scene_values::len(parts[2], em) : 0;
                } catch (const std::exception& e) {
                    return refuse(std::string("drop-shadow(): ") + e.what());
                }
                if (!(op.blur >= 0) || op.blur > constants::max_filter_blur ||
                    !(std::abs(op.x) <= constants::max_filter_offset) ||
                    !(std::abs(op.y) <= constants::max_filter_offset) ||
                    !std::all_of(op.color.begin(), op.color.end(), [](float c) {
                        return std::isfinite(c);
                    })) {
                    return refuse("drop-shadow() takes a blur of 0 to " +
                                  std::to_string(int(constants::max_filter_blur)) + "px and offsets within " +
                                  std::to_string(int(constants::max_filter_offset)) + "px");
                }
                if (op.color[3] > 0) {
                    add(op);
                }
                continue;
            }
            if (name == "url") {
                const auto id = fragment(argument);
                if (id.empty()) {
                    return refuse("url() of other than a filter element of the page (#id)");
                }
                const auto element = resolve ? resolve(id) : std::nullopt;
                if (!element || element->tag != "filter" || element->space != constants::svg_namespace_uri) {
                    // A reference to no filter element: the whole filter is ignored.
                    return std::vector<paint::FilterOp>{};
                }
                const auto matrices = svg_filter(*element);
                if (const auto* why = std::get_if<std::string>(&matrices)) {
                    return refuse("url(#" + id + "): " + *why);
                }
                for (const auto& op : std::get<std::vector<paint::FilterOp>>(matrices)) {
                    if (op.matrix != paint::identity_color_matrix) {
                        add(op);
                    }
                }
                continue;
            }
            if (!color_function(name)) {
                return refuse(std::string(name) + "() is not a filter function the scene takes");
            }
            const auto m = color_matrix(name, argument);
            if (const auto* why = std::get_if<std::string>(&m)) {
                return refuse(*why);
            }
            if (std::get<Matrix>(m) != paint::identity_color_matrix) {
                paint::FilterOp op;
                op.kind = paint::FilterOp::Kind::Matrix;
                op.matrix = std::get<Matrix>(m);
                add(op);
            }
        }
        if (out.size() > constants::max_filter_functions) {
            return refuse("at most " + std::to_string(constants::max_filter_functions) + " functions");
        }
        return out;
    }

    bool scene_path_shadowed(const SceneNode& n) {
        if (n.space != constants::svg_namespace_uri || n.tag != "svg" || n.effect || n.background_image ||
            n.border_image || !n.particles.empty()) {
            return false;
        }
        try {
            if (!scene_values::drop_shadow(n.style)) {
                return false;
            }
            const auto none = [](std::string_view v) {
                const auto t = css::trim(v);
                return t.empty() || t == "none";
            };
            if (!none(n.style.text(PropertyId::BackgroundImage, "none")) ||
                !none(n.style.text(PropertyId::BoxShadow, "none")) ||
                scene_values::shadow_color(scene_values::get(n.style, PropertyId::BackgroundColor, "transparent"),
                                           n.style)[3] > 0) {
                return false;
            }
            if (n.paint && n.paint->values[constants::scene::paint_slot_background_color] &&
                n.paint->values[constants::scene::paint_slot_background_color]->color[3] > 0) {
                return false;
            }
            for (const auto* side : {"top", "right", "bottom", "left"}) {
                const auto prefix = std::string("border-") + side;
                // As the display list takes a side without a style: solid.
                const auto style = scene_values::get(n.style, prefix + "-style", "solid");
                if (style != "none" && style != "hidden" &&
                    scene_values::len(scene_values::get(n.style, prefix + "-width", "0")) > 0) {
                    return false;
                }
            }
            // Shapes alone: no text, no group of an opacity below 1.
            const auto shapes = [](auto&& self, const SceneNode& c) -> bool {
                if (c.tag == "text" || scene_values::len(scene_values::get(c.style, PropertyId::Opacity, "1")) < 1) {
                    return false;
                }
                return std::all_of(c.children.begin(), c.children.end(), [&](const SceneNode& child) {
                    return self(self, child);
                });
            };
            return std::all_of(n.children.begin(), n.children.end(), [&](const SceneNode& child) {
                return shapes(shapes, child);
            });
        } catch (const std::exception&) {
            return false;
        }
    }
} // namespace weft
