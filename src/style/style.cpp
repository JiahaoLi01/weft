#include "style/style.hpp"
#include "base/css_scalar.hpp"
#include "base/owner_thread.hpp"
#include "constants/common.hpp"
#include "constants/style.hpp"
#include "dom/selector.hpp"
#include <thread>
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <functional>
#include <set>
#include <stdexcept>

namespace weft {
    namespace {
        [[noreturn]] void fail(const std::string& s) {
            throw_diagnostic(Diag::StyleProfile, s);
        }

        // Style sheets use CSS Syntax whitespace, which includes form feed.
        std::string trim(std::string_view s) {
            return css::trim(s, css::syntax_whitespace);
        }

        std::string lower(std::string s) {
            for (auto& c : s) {
                c = char(std::tolower(static_cast<unsigned char>(c)));
            }
            return s;
        }

        bool ident_char(char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
        }

        bool ident_start(char c) {
            return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '-';
        }

        std::string comments(std::string_view text, bool scene = false) {
            std::string out;
            char quote{};
            for (std::size_t p = 0; p < text.size(); ++p) {
                const char c = text[p];
                if (!scene && (c == '\\' || static_cast<unsigned char>(c) >= constants::style::ascii_end)) {
                    fail("escapes/non-ASCII syntax outside profile");
                }
                if (quote) {
                    out += c;
                    if (c == quote) {
                        quote = 0;
                    }
                } else if (c == '\'' || c == '"') {
                    quote = c;
                    out += c;
                } else if (c == '/' && p + 1 < text.size() && text[p + 1] == '*') {
                    const auto e = text.find("*/", p + 2);
                    if (e == std::string_view::npos) {
                        fail("unterminated comment");
                    }
                    out += ' ';
                    p = e + 1;
                } else {
                    out += c;
                }
            }
            if (quote) {
                fail("unterminated string");
            }
            return out;
        }
        // Tokens retain boundaries through substitution: NUMBER(16) IDENT(px) may
        // never be concatenated into a DIMENSION(16px). Functions own balanced tokens.
        enum class Kind {
            Ident,
            Number,
            Dimension,
            Percentage,
            Hash,
            String,
            Function,
            Delim
        };

        struct Token {
            Kind kind;
            std::string text;
            std::vector<Token> args;
        };

        using Tokens = std::vector<Token>;

        std::size_t token_count(const Tokens& tokens) {
            std::size_t n = tokens.size();
            for (const auto& t : tokens) {
                n += token_count(t.args);
            }
            return n;
        }

        struct Lexer {
            std::string_view text;
            std::size_t p{};
            std::size_t count{};

            Tokens scan(unsigned depth = 0) {
                if (depth > constants::style::max_function_depth) {
                    fail("function depth bound");
                }
                Tokens out;
                while (p < text.size()) {
                    char c = text[p];
                    if (std::isspace(static_cast<unsigned char>(c))) {
                        ++p;
                        continue;
                    }
                    if (c == ')') {
                        if (!depth) {
                            fail("unmatched parenthesis");
                        }
                        ++p;
                        return out;
                    }
                    if (++count > constants::style::max_value_tokens) {
                        fail("token bound");
                    }
                    const auto begin = p;
                    if (c == '\'' || c == '"') {
                        ++p;
                        while (p < text.size() && text[p] != c) {
                            ++p;
                        }
                        if (p == text.size()) {
                            fail("string");
                        }
                        ++p;
                        out.push_back({Kind::String, std::string(text.substr(begin, p - begin)), {}});
                        continue;
                    }
                    auto digit = [&](std::size_t i) {
                        return i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]));
                    };
                    const bool numeric =
                        digit(p) || (c == '.' && digit(p + 1)) ||
                        ((c == '+' || c == '-') &&
                         (digit(p + 1) || (p + 2 < text.size() && text[p + 1] == '.' && digit(p + 2))));
                    if (numeric) {
                        if (c == '+' || c == '-') {
                            ++p;
                        }
                        while (digit(p)) {
                            ++p;
                        }
                        if (p + 1 < text.size() && text[p] == '.' && digit(p + 1)) {
                            ++p;
                            while (digit(p)) {
                                ++p;
                            }
                        }
                        if (p < text.size() && (text[p] == 'e' || text[p] == 'E')) {
                            auto e = p + 1;
                            if (e < text.size() && (text[e] == '+' || text[e] == '-')) {
                                ++e;
                            }
                            if (digit(e)) {
                                p = e;
                                while (digit(p)) {
                                    ++p;
                                }
                            }
                        }
                        Kind kind = Kind::Number;
                        if (p < text.size() && text[p] == '%') {
                            ++p;
                            kind = Kind::Percentage;
                        } else if (p < text.size() && ident_start(text[p])) {
                            while (p < text.size() && ident_char(text[p])) {
                                ++p;
                            }
                            kind = Kind::Dimension;
                        }
                        out.push_back({kind, std::string(text.substr(begin, p - begin)), {}});
                        continue;
                    }
                    if (ident_start(c) && !(c == '-' && (p + 1 == text.size() || !ident_char(text[p + 1])))) {
                        while (p < text.size() && ident_char(text[p])) {
                            ++p;
                        }
                        std::string name(text.substr(begin, p - begin));
                        if (p < text.size() && text[p] == '(') {
                            ++p;
                            out.push_back({Kind::Function, std::move(name), scan(depth + 1)});
                        } else {
                            out.push_back({Kind::Ident, std::move(name), {}});
                        }
                        continue;
                    }
                    if (c == '#') {
                        ++p;
                        while (p < text.size() && ident_char(text[p])) {
                            ++p;
                        }
                        if (p == begin + 1) {
                            fail("hash token");
                        }
                        out.push_back({Kind::Hash, std::string(text.substr(begin, p - begin)), {}});
                        continue;
                    }
                    if (c == '(' || c == '{' || c == '}' || c == '[' || c == ']' || c == ';' || c == '!' || c == '\\') {
                        fail("token syntax outside profile");
                    }
                    ++p;
                    out.push_back({Kind::Delim, std::string(1, c), {}});
                }
                if (depth) {
                    fail("unclosed function");
                }
                return out;
            }
        };

        std::string serialize(const Tokens& tokens) {
            std::string out;
            for (const auto& t : tokens) {
                if (t.text == ",") {
                    out += ',';
                    continue;
                }
                if (!out.empty()) {
                    out += ' ';
                }
                out += t.text;
                if (t.kind == Kind::Function) {
                    out += '(' + serialize(t.args) + ')';
                }
                if (out.size() > constants::style::max_resolved_value_bytes) {
                    fail("resolved value byte bound");
                }
            }
            return out;
        }

        bool custom(const std::string& s) {
            return s.size() > 2 && s.starts_with("--") && std::all_of(s.begin(), s.end(), ident_char);
        }

        struct Var {
            std::string name;
            std::size_t fallback;
        };

        Var variable(const Token& t) {
            const auto comma = std::find_if(t.args.begin(), t.args.end(), [](const auto& a) {
                return a.kind == Kind::Delim && a.text == ",";
            });
            const auto index = std::size_t(comma - t.args.begin());
            if (index != 1 || t.args[0].kind != Kind::Ident || !custom(t.args[0].text)) {
                fail("var() grammar");
            }
            return {t.args[0].text, comma == t.args.end() ? t.args.size() : index + 1};
        }

        bool references(const Tokens& tokens, std::set<std::string>* deps = nullptr) {
            bool found{};
            for (const auto& t : tokens) {
                if (t.kind == Kind::Function) {
                    if (lower(t.text) == "var") {
                        auto v = variable(t);
                        found = true;
                        if (deps) {
                            deps->insert(v.name);
                        }
                    }
                    found = references(t.args, deps) || found;
                }
            }
            return found;
        }

        std::string keyword(const Tokens& t) {
            return t.size() == 1 && t[0].kind == Kind::Ident ? lower(t[0].text) : "";
        }

        bool wide(const std::string& s) {
            return s == "initial" || s == "inherit" || s == "unset";
        }

        // A <line-style> keyword (lowercase).
        bool border_style_keyword(std::string_view s) {
            for (const auto* k :
                 {"none", "hidden", "dotted", "dashed", "solid", "double", "groove", "ridge", "inset", "outset"}) {
                if (s == k) {
                    return true;
                }
            }
            return false;
        }

        struct Declaration {
            std::string key;
            Tokens tokens;
            bool important{};
            bool from_inset{};
            int box_edge{-1};
            int corner{-1};
            int axis{-1};
            bool solid_background{};
            // A part of a scene background shorthand: 0 its color, 1 its image, which take the value or none.
            int background_layer{-1};
            // The registered property of a non-custom key.
            PropertyId id{};
        };

        // Split only at top-level delimiters; quoted/functional commas and semicolons
        // do not break a declaration or selector list.
        std::vector<std::string> split(std::string_view text, char separator) {
            auto result = css::split(text, separator, {.quotes = true, .brackets = true, .trim_parts = false});
            if (result.nesting == css::Nesting::Unmatched) {
                fail("unmatched delimiter");
            }
            if (result.nesting == css::Nesting::Unclosed) {
                fail("unbalanced syntax");
            }
            return std::move(result.parts);
        }

        std::vector<Declaration> declarations(std::string text, const StyleProfile& profile) {
            if (text.size() > constants::style::max_declaration_bytes) {
                fail("declaration byte bound");
            }
            std::vector<Declaration> result;
            for (auto part : split(text, ';')) {
                if (trim(part).empty()) {
                    continue;
                }
                const auto colon = part.find(':');
                if (colon == std::string::npos) {
                    fail("declaration colon");
                }
                auto key = trim(part.substr(0, colon));
                auto v = trim(part.substr(colon + 1));
                if (!custom(key)) {
                    key = lower(key);
                }
                const bool border_side =
                    key == "border-top" || key == "border-right" || key == "border-bottom" || key == "border-left";
                if (!custom(key) && !profile.properties.contains(key) &&
                    !(profile.scene_syntax && (key == "border" || border_side || key == "flex")) &&
                    !(key == "inset" && profile.single_value_inset) &&
                    !(key == "background" && profile.properties.contains("background-color"))) {
                    fail("unsupported property " + key);
                }
                bool important{};
                std::size_t bang = std::string::npos;
                char quote{};
                int depth{};
                for (std::size_t i = 0; i < v.size(); ++i) {
                    const auto c = v[i];
                    if (quote) {
                        if (c == quote) {
                            quote = 0;
                        }
                    } else if (c == '\'' || c == '"') {
                        quote = c;
                    } else if (c == '(') {
                        ++depth;
                    } else if (c == ')') {
                        --depth;
                    } else if (c == '!' && !depth) {
                        bang = i;
                    }
                }
                if (bang != std::string::npos) {
                    if (lower(trim(v.substr(bang + 1))) != "important") {
                        fail("importance syntax");
                    }
                    important = true;
                    v = trim(v.substr(0, bang));
                }
                Lexer lexer{v};
                auto tokens = lexer.scan();
                references(tokens);
                if (tokens.empty() && !custom(key)) {
                    fail("empty declaration " + key);
                }
                if (profile.scene_syntax && (key == "border" || border_side)) {
                    // Width, style and color in any order; what is left out takes its initial value. A style the
                    // scene does not draw stays a style, so the scene diagnoses it.
                    Tokens width{{Kind::Dimension, "0px", {}}};
                    Tokens style{{Kind::Ident, "none", {}}};
                    Tokens color{{Kind::Ident, "currentcolor", {}}};
                    for (const auto& token : tokens) {
                        if (token.kind == Kind::Dimension || token.kind == Kind::Number) {
                            width = {token};
                        } else if (token.kind == Kind::Ident && border_style_keyword(lower(token.text))) {
                            style = {{Kind::Ident, lower(token.text), {}}};
                        } else if (token.kind == Kind::Hash || token.kind == Kind::Function ||
                                   token.kind == Kind::Ident) {
                            color = {token};
                        } else {
                            fail("border shorthand profile");
                        }
                    }
                    for (const auto* side : {"top", "right", "bottom", "left"}) {
                        const auto prefix = std::string("border-") + side;
                        if (border_side && key != prefix) {
                            continue;
                        }
                        result.push_back({prefix + "-width", width, important});
                        result.push_back({prefix + "-style", style, important});
                        result.push_back({prefix + "-color", color, important});
                    }
                } else if (profile.scene_syntax && key == "flex") {
                    if (tokens.size() != 1 || tokens[0].kind != Kind::Number) {
                        fail("flex shorthand profile requires one number");
                    }
                    result.push_back({"flex-grow", tokens, important});
                    result.push_back({"flex-shrink", {{Kind::Number, "1", {}}}, important});
                    result.push_back({"flex-basis", {{Kind::Percentage, "0%", {}}}, important});
                } else if (key == "background" && profile.scene_syntax) {
                    // The color and the image as longhands, so they cascade with the others; which one the value
                    // gives is known once var() is substituted.
                    result.push_back({"background-color", tokens, important});
                    result.back().background_layer = 0;
                    result.push_back({"background-image", std::move(tokens), important});
                    result.back().background_layer = 1;
                } else if (key == "background" && !profile.scene_syntax) {
                    result.push_back({"background-color", std::move(tokens), important});
                    result.back().solid_background = true;
                } else if (key == "inset" && profile.four_value_edges) {
                    // A box shorthand: one to four lengths, top, right, bottom, left.
                    int edge{};
                    for (const auto* side : {"top", "right", "bottom", "left"}) {
                        result.push_back({side, tokens, important, true, edge++});
                    }
                } else if (key == "inset") {
                    for (const auto* side : {"left", "top", "right", "bottom"}) {
                        result.push_back({side, tokens, important, true});
                    }
                } else if (profile.four_value_edges &&
                           (key == "padding" || key == "margin" || key == "border-width" ||
                            (key == "border-style" && profile.properties.contains("border-top-style")))) {
                    int edge{};
                    for (const auto* side : {"top", "right", "bottom", "left"}) {
                        const auto name = key.starts_with("border-")
                                              ? std::string("border-") + side + key.substr(std::string("border").size())
                                              : key + '-' + side;
                        result.push_back({name, tokens, important, false, edge++});
                    }
                } else if (profile.flow_shorthands && key == "border-radius") {
                    int corner{};
                    for (const auto* name : {"border-top-left-radius", "border-top-right-radius",
                                             "border-bottom-right-radius", "border-bottom-left-radius"}) {
                        result.push_back({name, tokens, important, false, -1, corner++});
                    }
                } else if ((profile.flow_shorthands || profile.properties.contains("overflow-x")) &&
                           key == "overflow") {
                    result.push_back({"overflow-x", tokens, important, false, -1, -1, 0});
                    result.push_back({"overflow-y", tokens, important, false, -1, -1, 1});
                } else {
                    result.push_back({std::move(key), std::move(tokens), important});
                }
            }
            for (auto& d : result) {
                if (!custom(d.key)) {
                    const auto id = find_property(d.key);
                    if (!id) {
                        fail("unregistered property " + d.key);
                    }
                    d.id = *id;
                }
            }
            return result;
        }

        // A cascade path element: the snapshot root has no parent, and :root is its html element.
        struct PathElement {
            const std::vector<const DomSnapshot*>* path{};
            std::size_t index{};

            const DomSnapshot& node() const {
                return *(*path)[index];
            }

            std::string_view local_name() const {
                return node().local_name;
            }

            bool html() const {
                return node().namespace_uri == constants::xhtml_namespace_uri;
            }

            const std::string* attribute(std::string_view name) const {
                const auto it = node().attributes.find(name);
                return it == node().attributes.end() ? nullptr : &it->second;
            }

            bool hovered() const {
                return node().hovered;
            }

            bool active() const {
                return node().active;
            }

            bool document_root() const {
                return index == 0 && node().local_name == "html";
            }

            std::optional<PathElement> parent() const {
                return index ? std::optional(PathElement{path, index - 1}) : std::nullopt;
            }
        };

        // One snapshot element on its own, for compound state keys.
        struct LoneElement {
            const DomSnapshot* n{};
            bool root{};

            std::string_view local_name() const {
                return n->local_name;
            }

            bool html() const {
                return n->namespace_uri == constants::xhtml_namespace_uri;
            }

            const std::string* attribute(std::string_view name) const {
                const auto it = n->attributes.find(name);
                return it == n->attributes.end() ? nullptr : &it->second;
            }

            bool hovered() const {
                return n->hovered;
            }

            bool active() const {
                return n->active;
            }

            bool document_root() const {
                return root;
            }

            std::optional<LoneElement> parent() const {
                return std::nullopt;
            }
        };

        struct Rule {
            dom::Selector selector;
            std::vector<Declaration> declarations;

            bool matches(const std::vector<const DomSnapshot*>& path) const {
                return dom::matches(selector, PathElement{&path, path.size() - 1});
            }
        };

        Rule selector(std::string text, std::vector<Declaration> decl, bool scene) {
            const dom::SelectorGrammar grammar{scene, scene, Diag::StyleProfile, Diag::StyleProfile};
            return {dom::parse_selector(text, grammar).value(), std::move(decl)};
        }

        std::vector<Rule> rules(std::string text, const StyleProfile& profile) {
            if (text.size() > constants::max_text_bytes) {
                fail("stylesheet byte bound");
            }
            text = comments(text, profile.scene_syntax);
            std::vector<Rule> out;
            std::size_t p{};
            while (p < text.size()) {
                if (trim(text.substr(p)).empty()) {
                    break;
                }
                const auto b = text.find('{', p);
                if (b == std::string::npos) {
                    fail("stylesheet rule");
                }
                std::size_t e = b + 1;
                char quote{};
                for (; e < text.size(); ++e) {
                    const auto c = text[e];
                    if (quote) {
                        if (c == quote) {
                            quote = 0;
                        }
                    } else if (c == '\'' || c == '"') {
                        quote = c;
                    } else if (c == '}') {
                        break;
                    } else if (c == '{') {
                        fail("at-rule/nested rule outside profile");
                    }
                }
                if (e == text.size()) {
                    fail("unclosed rule");
                }
                auto decl = declarations(text.substr(b + 1, e - b - 1), profile);
                for (auto s : split(text.substr(p, b - p), ',')) {
                    out.push_back(selector(trim(s), decl, profile.scene_syntax));
                }
                if (out.size() > constants::style::max_rules) {
                    fail("rule bound");
                }
                p = e + 1;
            }
            return out;
        }

        float number(const std::string& s) {
            const auto v = css::parse_number<float>(s);
            if (!v) {
                fail("finite number");
            }
            return *v;
        }

        bool length_token(const Token& t) {
            if (t.kind == Kind::Number) {
                return number(t.text) == 0;
            }
            if (t.kind == Kind::Percentage) {
                return true;
            }
            if (t.kind == Kind::Dimension && !lower(t.text).ends_with("px")) {
                fail("unit outside px/percent profile");
            }
            return t.kind == Kind::Dimension && lower(t.text).ends_with("px") &&
                   std::isfinite(number(t.text.substr(0, t.text.size() - 2)));
        }

        bool valid(const Tokens& t, const CssProperty& prop) {
            const auto k = keyword(t);
            if (std::find(prop.keywords.begin(), prop.keywords.end(), k) != prop.keywords.end()) {
                return true;
            }
            if (prop.grammar == CssGrammar::Keyword) {
                return false;
            }
            if (prop.grammar == CssGrammar::Tokens) {
                return !t.empty();
            }
            if (prop.grammar == CssGrammar::Radius) {
                if (t.empty() || t.size() > 2) {
                    return false;
                }
                for (const auto& a : t) {
                    if (!length_token(a)) {
                        return false;
                    }
                    const auto suffix = a.kind == Kind::Dimension ? 2 : a.kind == Kind::Percentage ? 1 : 0;
                    if (number(a.text.substr(0, a.text.size() - suffix)) < 0) {
                        return false;
                    }
                }
                return true;
            }
            if (prop.grammar == CssGrammar::Color) {
                if (t.size() != 1) {
                    return false;
                }
                const auto& a = t[0];
                if (a.kind == Kind::Ident && lower(a.text) != "transparent") {
                    fail("named color outside profile");
                }
                if (a.kind == Kind::Function && lower(a.text) != "rgb" && lower(a.text) != "rgba") {
                    fail("color function outside profile");
                }
                if (a.kind == Kind::Hash && a.text.size() != constants::style::hex_color_length) {
                    fail("hex color form outside #rrggbb profile");
                }
                if (a.kind == Kind::Function) {
                    for (const auto& c : a.args) {
                        if (c.kind != Kind::Number && !(c.kind == Kind::Delim && c.text == ",")) {
                            fail("color components outside legacy numeric profile");
                        }
                    }
                }
                try {
                    parse_css_color(serialize(t));
                    return true;
                } catch (const std::invalid_argument&) {
                    return false;
                }
            }
            if (t.size() != 1) {
                return false;
            }
            if (prop.grammar == CssGrammar::Number) {
                return t[0].kind == Kind::Number;
            }
            if (t[0].kind == Kind::Function && lower(t[0].text) == "calc") {
                const auto& a = t[0].args;
                if (a.size() != 3 || a[1].kind != Kind::Delim || (a[1].text != "+" && a[1].text != "-")) {
                    fail("calc grammar outside single +/- profile");
                }
                return length_token(a[0]) && length_token(a[2]);
            }
            return length_token(t[0]);
        }

        using Variables = std::map<std::string, std::optional<Tokens>>;

        // A profile by property id: whether it inherits, and its parsed initial value.
        struct ProfileIndex {
            std::array<bool, property_count> registered{};
            std::array<bool, property_count> inherited{};
            std::array<CssValue, property_count> initial;

            ProfileIndex() = default;

            explicit ProfileIndex(const StyleProfile& profile) {
                for (const auto& [name, property] : profile.properties) {
                    const auto id = find_property(name);
                    if (!id) {
                        fail("unregistered property " + name);
                    }
                    registered[std::size_t(*id)] = true;
                    inherited[std::size_t(*id)] = property.inherited;
                    initial[std::size_t(*id)] = CssValue(property.initial);
                }
            }
        };

        // SVG presentation attributes the scene profile maps to declarations, in cascade order.
        constexpr std::array<std::string_view, 18> presentation_attributes{
            "fill",           "fill-opacity",      "fill-rule",       "stroke",           "stroke-width",
            "stroke-opacity", "stroke-linecap",    "stroke-linejoin", "stroke-dasharray", "stroke-dashoffset",
            "display",        "pointer-events",    "font-size",       "font-weight",      "font-family",
            "text-anchor",    "dominant-baseline", "opacity"};

        // Appends a decimal length and ':' as memo and selector keys write them.
        void append_length(std::string& key, std::size_t length) {
            char digits[24];
            key.append(digits, std::to_chars(digits, digits + sizeof digits, length).ptr);
            key += ':';
        }

        // Whether text contains needle, which is lower case, ignoring ASCII case.
        bool contains_ascii_lower(std::string_view text, std::string_view needle) {
            return std::search(text.begin(), text.end(), needle.begin(), needle.end(), [](char a, char b) {
                       return std::tolower(static_cast<unsigned char>(a)) == b;
                   }) != text.end();
        }

        struct Engine {
            const StyleProfile& profile;
            const ProfileIndex& profile_index;
            const std::vector<Rule>& ua;
            const std::vector<Rule>& author;
            std::vector<const DomSnapshot*> path;
            std::size_t nodes{};
            std::size_t work{};
            std::size_t bytes{};
            using Rank = std::array<unsigned, constants::style::cascade_rank_size>;

            struct Winners {
                std::map<std::string, Declaration> declarations;
                std::map<std::string, Rank> ranks;
                unsigned order{};
                std::size_t payload{};
                std::size_t used{};
            };

            struct Memo {
                ComputedStyle own;
                Variables variables;
                std::size_t token_work{};
                std::size_t output_bytes{};
                std::size_t payload_bytes{};
                std::size_t used{};
            };

            bool memoize{};
            bool uses_hover{};
            bool uses_active{};
            bool parent_all{};
            std::set<std::string> memo_attributes{"id", "class"};
            std::vector<const dom::Compound*> state_parts;
            std::string selector_path;
            std::vector<std::size_t> selector_lengths;

            struct MemoState {
                std::map<std::string, Memo> entries;
                std::map<std::string, Winners> winners;
                std::size_t bytes{};
                std::size_t winner_bytes{};
                std::size_t clock{};
            };

            MemoState local_memo;
            MemoState* memo_state = &local_memo;
            std::vector<NodeIdentity>* changed_nodes{};

            Engine(const StyleProfile& p, const ProfileIndex& i, const std::vector<Rule>& u, const std::vector<Rule>& a,
                   bool reuse = false, MemoState* state = nullptr, std::vector<NodeIdentity>* changed = nullptr)
                : profile(p), profile_index(i), ua(u), author(a), memoize(reuse) {
                if (state) {
                    memo_state = state;
                }
                changed_nodes = changed;
                for (const auto* list : {&ua, &author}) {
                    for (const auto& rule : *list) {
                        for (const auto& part : rule.selector.parts) {
                            uses_hover |= part.hover;
                            uses_active |= part.active;
                            if (part.hover || part.active) {
                                state_parts.push_back(&part);
                            }
                            for (const auto& attribute : part.attributes) {
                                memo_attributes.insert(attribute.name);
                                memo_attributes.insert(attribute.lower_name);
                            }
                        }
                        for (const auto& d : rule.declarations) {
                            parent_all |= keyword(d.tokens) == "inherit" || references(d.tokens);
                        }
                    }
                }
            }

            std::string selector_key() {
                selector_lengths.push_back(selector_path.size());
                auto add = [&](std::string_view value) {
                    append_length(selector_path, value.size());
                    selector_path += value;
                };
                const auto* n = path.back();
                add(n->local_name);
                add(n->namespace_uri);
                bool h{};
                bool a{};
                for (auto* p : state_parts) {
                    if (dom::matches(*p, LoneElement{n, n == path.front() && n->local_name == "html"}, false)) {
                        h |= p->hover;
                        a |= p->active;
                    }
                }
                if (uses_hover) {
                    selector_path += (h && n->hovered) ? '1' : '0';
                }
                if (uses_active) {
                    selector_path += (a && n->active) ? '1' : '0';
                }
                for (const auto& [k, v] : n->attributes) {
                    if (memo_attributes.contains(k)) {
                        add(k);
                        add(v);
                    }
                }
                selector_path += ';';
                return selector_path;
            }

            std::string memo_key(const ComputedStyle* parent, const Variables& inherited,
                                 const std::string& selectors) {
                std::string key = selectors;
                auto add = [&](std::string_view value) {
                    append_length(key, value.size());
                    key += value;
                };
                const auto& dom = *path.back();
                // Ancestor inline declarations affect descendants through selectors,
                // inherited values and variables, not through unrelated paint values.
                const auto style = dom.attributes.find("style");
                const std::string_view inline_value =
                    style == dom.attributes.end() ? std::string_view{} : std::string_view(style->second);
                add(inline_value);
                bool presentation_parent{};
                if (profile.scene_syntax) {
                    for (const auto k : presentation_attributes) {
                        if (const auto it = dom.attributes.find(k); it != dom.attributes.end()) {
                            add(k);
                            add(it->second);
                            presentation_parent |=
                                contains_ascii_lower(it->second, "inherit") || contains_ascii_lower(it->second, "var(");
                        }
                    }
                }
                const bool all = parent_all || presentation_parent || contains_ascii_lower(inline_value, "inherit") ||
                                 contains_ascii_lower(inline_value, "var(");
                key += '|';
                if (parent) {
                    for (const auto& [id, v] : parent->values) {
                        if (all ||
                            (profile_index.registered[std::size_t(id)] && profile_index.inherited[std::size_t(id)])) {
                            add(property_name(id));
                            add(v.text());
                        }
                    }
                }
                key += '|';
                auto tokens = [&](auto&& self, const Tokens& values) -> void {
                    key += '[';
                    for (const auto& t : values) {
                        append_length(key, std::size_t(t.kind));
                        add(t.text);
                        self(self, t.args);
                    }
                    key += ']';
                };
                for (const auto& [k, v] : inherited) {
                    add(k);
                    key += v ? '1' : '0';
                    if (v) {
                        tokens(tokens, *v);
                    }
                }
                return key;
            }

            void charge(std::size_t count) {
                work += count;
                if (work > constants::style::max_token_work) {
                    fail("style token work bound");
                }
            }

            std::optional<Tokens> substitute(const Tokens& tokens,
                                             const std::function<std::optional<Tokens>(const std::string&)>& lookup) {
                Tokens out;
                bool invalid{};
                for (auto t : tokens) {
                    charge(1);
                    if (t.kind == Kind::Function && lower(t.text) == "var") {
                        const auto v = variable(t);
                        auto r = lookup(v.name);
                        if (!r) {
                            // Missing comma differs from a comma with an empty fallback.
                            const bool comma =
                                t.args.size() > 1 && t.args[1].kind == Kind::Delim && t.args[1].text == ",";
                            if (comma) {
                                r = substitute(Tokens(t.args.begin() + v.fallback, t.args.end()), lookup);
                            }
                        }
                        if (!r) {
                            invalid = true;
                        } else {
                            charge(token_count(*r));
                            out.insert(out.end(), r->begin(), r->end());
                        }
                    } else {
                        if (t.kind == Kind::Function) {
                            auto a = substitute(t.args, lookup);
                            if (!a) {
                                invalid = true;
                            } else {
                                t.args = std::move(*a);
                            }
                        }
                        out.push_back(std::move(t));
                    }
                    if (out.size() > constants::style::max_resolved_tokens) {
                        fail("resolved token bound");
                    }
                }
                serialize(out);
                return invalid ? std::nullopt : std::optional(std::move(out));
            }

            void own_into(ComputedStyle& target, const ComputedStyle& own, NodeIdentity current_identity) {
                if (target.identity == current_identity && target.local_name == own.local_name &&
                    target.values == own.values && target.custom_properties == own.custom_properties &&
                    target.invalid_at_computed_value == own.invalid_at_computed_value &&
                    target.specified_properties == own.specified_properties) {
                    return;
                }
                if (changed_nodes) {
                    changed_nodes->push_back(current_identity);
                }
                target.identity = current_identity;
                target.local_name = own.local_name;
                target.values = own.values;
                target.custom_properties = own.custom_properties;
                target.invalid_at_computed_value = own.invalid_at_computed_value;
                target.specified_properties = own.specified_properties;
            }

            void children_into(const DomSnapshot& dom, ComputedStyle& out, const Variables& variables) {
                // Reuse owner values by identity, retaining no DOM references. Order and
                // generation changes are verified even when the rule memo hits.
                auto old = std::move(out.children);
                out.children.clear();
                out.children.reserve(old.capacity());
                std::size_t hint{};
                for (const auto& child : dom.children) {
                    if (child.type == constants::element_node_type) {
                        std::size_t index = hint++;
                        if (index >= old.size() || old[index].identity != child.identity) {
                            index = 0;
                            while (index < old.size() && old[index].identity != child.identity) {
                                ++index;
                            }
                        }
                        out.children.push_back(index < old.size() ? std::move(old[index]) : ComputedStyle{});
                        compute_into(child, out.children.back(), &out, variables);
                    }
                }
                if (memoize) {
                    selector_path.resize(selector_lengths.back());
                    selector_lengths.pop_back();
                }
                path.pop_back();
            }

            ComputedStyle compute(const DomSnapshot& dom, const ComputedStyle* parent, const Variables& inherited) {
                ComputedStyle out;
                compute_into(dom, out, parent, inherited);
                return out;
            }

            void compute_into(const DomSnapshot& dom, ComputedStyle& destination, const ComputedStyle* parent,
                              const Variables& inherited) {
                if (path.size() >= constants::max_document_depth || ++nodes > constants::max_document_nodes) {
                    fail("node/depth bound");
                }
                if (dom.type != constants::element_node_type ||
                    (dom.namespace_uri != constants::xhtml_namespace_uri &&
                     !(profile.scene_syntax && dom.namespace_uri == constants::svg_namespace_uri))) {
                    fail("HTML/scene SVG element snapshot required");
                }
                path.push_back(&dom);
                const auto work_before = work;
                const auto bytes_before = bytes;
                auto selectors = memoize ? selector_key() : std::string{};
                auto key = memoize ? memo_key(parent, inherited, selectors) : std::string{};
                if (memoize) {
                    if (auto it = memo_state->entries.find(key); it != memo_state->entries.end()) {
                        it->second.used = ++memo_state->clock;
                        const auto variables = it->second.variables;
                        const auto token_work = it->second.token_work;
                        const auto output_bytes = it->second.output_bytes;
                        charge(token_work);
                        bytes += output_bytes;
                        if (bytes > constants::style::max_computed_output_bytes) {
                            fail("computed output byte bound");
                        }
                        own_into(destination, it->second.own, dom.identity);
                        children_into(dom, destination, variables);
                        return;
                    }
                }
                std::map<std::string, Declaration> winning;
                std::map<std::string, Rank> ranks;
                unsigned order{};
                auto apply = [&](const Declaration& d, Rank rank) {
                    rank[constants::style::cascade_rank_order_index] = ++order;
                    if (!ranks.contains(d.key) || ranks[d.key] <= rank) {
                        ranks[d.key] = rank;
                        winning[d.key] = d;
                    }
                };
                auto sheet = [&](const auto& list, unsigned origin) {
                    for (const auto& r : list) {
                        if (r.matches(path)) {
                            for (const auto& d : r.declarations) {
                                const auto& specificity = r.selector.specificity;
                                apply(d, {d.important ? 1u : 0u, d.important && origin == 0 ? 2u : origin, 0,
                                          specificity[0], specificity[1], specificity[2], 0});
                            }
                        }
                    }
                };
                // Stable rule matching is shared by all inline animation samples.
                // Presentation declarations and inline declarations remain evaluated
                // in their original cascade positions and do not enter this cache.
                bool winner_hit{};
                if (memoize) {
                    if (auto it = memo_state->winners.find(selectors); it != memo_state->winners.end()) {
                        it->second.used = ++memo_state->clock;
                        winning = it->second.declarations;
                        ranks = it->second.ranks;
                        order = it->second.order;
                        winner_hit = true;
                    }
                }
                if (!winner_hit) {
                    sheet(ua, 0);
                    sheet(author, 1);
                    if (memoize) {
                        std::size_t payload = selectors.size();
                        for (const auto& [k, d] : winning) {
                            payload += k.size() + serialize(d.tokens).size() + sizeof(Declaration) + sizeof(Rank);
                        }
                        if (payload <= constants::style::memo_max_bytes) {
                            while (!memo_state->winners.empty() &&
                                   (memo_state->winners.size() >= constants::style::memo_max_entries ||
                                    memo_state->winner_bytes + payload > constants::style::memo_max_bytes)) {
                                auto victim = std::min_element(memo_state->winners.begin(), memo_state->winners.end(),
                                                               [](const auto& a, const auto& b) {
                                                                   return a.second.used < b.second.used;
                                                               });
                                memo_state->winner_bytes -= victim->second.payload;
                                memo_state->winners.erase(victim);
                            }
                            memo_state->winner_bytes += payload;
                            memo_state->winners.emplace(selectors,
                                                        Winners{winning, ranks, order, payload, ++memo_state->clock});
                        }
                    }
                }
                if (profile.scene_syntax && dom.namespace_uri == constants::svg_namespace_uri) {
                    for (const auto k : presentation_attributes) {
                        if (const auto it = dom.attributes.find(k); it != dom.attributes.end()) {
                            for (const auto& d : declarations(std::string(k) + ":" + it->second, profile)) {
                                auto rank = Rank{0, 1, 0, 0, 0, 0, 0};
                                if (!ranks.contains(d.key) || ranks[d.key] < rank) {
                                    ranks[d.key] = rank;
                                    winning[d.key] = d;
                                }
                            }
                        }
                    }
                }
                if (dom.attributes.contains("style")) {
                    for (const auto& d :
                         declarations(comments(dom.attributes.at("style"), profile.scene_syntax), profile)) {
                        apply(d, {d.important ? 1u : 0u, 1, 1, 0, 0, 0, 0});
                    }
                }
                // Charge inherited token copies before allocating a child environment.
                // Input dimensions alone do not bound nested/exponential token copying.
                for (const auto& [k, t] : inherited) {
                    if (t) {
                        charge(token_count(*t));
                    }
                }
                Variables variables = inherited;
                std::map<std::string, Tokens> local;
                for (const auto& [k, d] : winning) {
                    if (custom(k)) {
                        const auto word = keyword(d.tokens);
                        if (word == "revert" || word == "revert-layer") {
                            fail("cascade rollback outside profile");
                        }
                        if (word == "initial") {
                            variables[k] = std::nullopt;
                        } else if (word == "inherit" || word == "unset") {
                            if (!variables.contains(k)) {
                                variables[k] = std::nullopt;
                            }
                        } else {
                            local[k] = d.tokens;
                        }
                    }
                }
                std::set<std::string> variable_names;
                for (const auto& [k, t] : variables) {
                    variable_names.insert(k);
                }
                for (const auto& [k, t] : local) {
                    variable_names.insert(k);
                }
                if (variable_names.size() > constants::style::max_custom_properties) {
                    fail("custom property count bound");
                }
                // Current CSS Values substitution contexts: selected fallbacks only.
                // Guard active property evaluation; every participant in a detected
                // cycle is invalid even when a fallback on that path produces tokens.
                std::set<std::string> cycle;
                std::set<std::string> done;
                std::vector<std::string> stack;
                std::function<std::optional<Tokens>(const std::string&)> resolve;
                resolve = [&](const std::string& k) -> std::optional<Tokens> {
                    const auto active = std::find(stack.begin(), stack.end(), k);
                    if (active != stack.end()) {
                        cycle.insert(active, stack.end());
                        return std::nullopt;
                    }
                    if (local.contains(k) && !done.contains(k)) {
                        stack.push_back(k);
                        auto result = substitute(local.at(k), resolve);
                        stack.pop_back();
                        const auto word = result ? keyword(*result) : "";
                        if (word == "initial") {
                            result = std::nullopt;
                        } else if (word == "inherit" || word == "unset") {
                            result = inherited.contains(k) ? inherited.at(k) : std::nullopt;
                        } else if (word == "revert" || word == "revert-layer") {
                            fail("computed cascade rollback outside profile");
                        }
                        variables[k] = cycle.contains(k) ? std::nullopt : std::move(result);
                        done.insert(k);
                    }
                    return variables.contains(k) ? variables.at(k) : std::nullopt;
                };
                for (const auto& [k, t] : local) {
                    resolve(k);
                }
                if (variables.size() > constants::style::max_custom_properties) {
                    fail("custom property count bound");
                }
                ComputedStyle out;
                out.identity = dom.identity;
                out.local_name = dom.local_name;
                for (const auto& [k, d] : winning) {
                    if (!custom(k)) {
                        out.specified_properties.push_back(k);
                    }
                }
                for (const auto& [k, t] : variables) {
                    out.custom_properties[k] = t ? std::optional(serialize(*t)) : std::nullopt;
                }
                if (parent) {
                    for (const auto& [id, value] : parent->values) {
                        if (profile_index.registered[std::size_t(id)] && profile_index.inherited[std::size_t(id)]) {
                            out.values.set(id, value);
                        }
                    }
                }
                for (const auto& [k, d] : winning) {
                    if (!custom(k)) {
                        const auto& prop = profile.properties.at(k);
                        auto tokens = substitute(d.tokens, resolve);
                        if (tokens && d.box_edge >= 0 && !wide(keyword(*tokens))) {
                            if (tokens->empty() || tokens->size() > 4) {
                                fail("box shorthand needs one to four lengths");
                            }
                            static constexpr int edge_indices[4][4] = {
                                {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
                            bool invalid_edge{};
                            for (const auto& t : *tokens) {
                                if (!valid(Tokens{t}, prop)) {
                                    invalid_edge = true;
                                }
                            }
                            if (invalid_edge) {
                                tokens = std::nullopt;
                            } else {
                                const auto selected = (*tokens)[edge_indices[tokens->size() - 1][d.box_edge]];
                                tokens = Tokens{selected};
                            }
                        }
                        if (tokens && d.corner >= 0 && !wide(keyword(*tokens))) {
                            const auto slash = std::find_if(tokens->begin(), tokens->end(), [](const auto& t) {
                                return t.kind == Kind::Delim && t.text == "/";
                            });
                            Tokens x(tokens->begin(), slash);
                            Tokens y = slash == tokens->end() ? x : Tokens(slash + 1, tokens->end());
                            static constexpr int corners[4][4] = {
                                {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
                            bool okay = !x.empty() && !y.empty() && x.size() <= 4 && y.size() <= 4;
                            for (const auto& a : x) {
                                okay = valid(Tokens{a}, prop) && okay;
                            }
                            for (const auto& a : y) {
                                okay = valid(Tokens{a}, prop) && okay;
                            }
                            if (okay) {
                                tokens = Tokens{x[corners[x.size() - 1][d.corner]], y[corners[y.size() - 1][d.corner]]};
                            } else {
                                tokens = std::nullopt;
                            }
                        }
                        if (tokens && d.axis >= 0 && !wide(keyword(*tokens))) {
                            bool okay = !tokens->empty() && tokens->size() <= 2;
                            for (const auto& a : *tokens) {
                                okay = valid(Tokens{a}, prop) && okay;
                            }
                            if (okay) {
                                tokens = Tokens{(*tokens)[tokens->size() == 1 ? 0 : d.axis]};
                            } else {
                                tokens = std::nullopt;
                            }
                        }
                        if (tokens && d.background_layer >= 0 && !wide(keyword(*tokens))) {
                            // A gradient, an effect or a host texture is the image; anything else the color.
                            const bool image = std::any_of(tokens->begin(), tokens->end(), [](const Token& t) {
                                const auto name = lower(t.text);
                                return t.kind == Kind::Function && (name.ends_with("gradient") ||
                                                                    name == "-weft-effect" || name == "-weft-texture");
                            });
                            const bool none = keyword(*tokens) == "none";
                            if (d.background_layer == 0 && (image || none)) {
                                tokens = Tokens{{Kind::Ident, "transparent", {}}};
                            } else if (d.background_layer == 1 && !image) {
                                tokens = Tokens{{Kind::Ident, "none", {}}};
                            }
                        }
                        std::string word = tokens ? keyword(*tokens) : "";
                        bool invalid = !tokens;
                        if (tokens && d.solid_background && !wide(word)) {
                            if (tokens->size() != 1) {
                                fail(
                                    "background supports one solid color only; layers/images/position outside profile");
                            }
                            invalid = !valid(*tokens, prop);
                        }
                        if (tokens && !wide(word) && references(d.tokens)) {
                            if (tokens->size() > 1 && (d.from_inset || k == "margin" || k == "padding" ||
                                                       k == "border" || k == "border-width" || k == "gap")) {
                                fail("multi-value shorthand outside single-value profile");
                            }
                            invalid = !valid(*tokens, prop);
                        }
                        if (invalid) {
                            out.invalid_at_computed_value.push_back(k);
                            word = "unset";
                        }
                        if (word == "inherit" || (word == "unset" && prop.inherited)) {
                            const auto* inherited_value = parent ? parent->values.find(d.id) : nullptr;
                            out.values.set(d.id, inherited_value ? *inherited_value
                                                                 : profile_index.initial[std::size_t(d.id)]);
                        } else if (word == "initial" || word == "unset") {
                            out.values.set(d.id, profile_index.initial[std::size_t(d.id)]);
                        } else {
                            out.values.set(d.id, CssValue(serialize(*tokens)));
                        }
                    }
                }
                for (const auto& [id, v] : out.values) {
                    bytes += property_name(id).size() + v.text().size();
                }
                for (const auto& [k, v] : out.custom_properties) {
                    bytes += k.size() + (v ? v->size() : 0);
                }
                if (bytes > constants::style::max_computed_output_bytes) {
                    fail("computed output byte bound");
                }
                if (memoize) {
                    const auto output_bytes = bytes - bytes_before;
                    std::size_t variable_bytes{};
                    for (const auto& [k, v] : variables) {
                        variable_bytes += k.size() + (v ? serialize(*v).size() : 0);
                    }
                    const auto payload = key.size() + output_bytes + variable_bytes;
                    if (payload <= constants::style::memo_max_bytes) {
                        while (!memo_state->entries.empty() &&
                               (memo_state->entries.size() >= constants::style::memo_max_entries ||
                                memo_state->bytes + payload > constants::style::memo_max_bytes)) {
                            auto victim = std::min_element(memo_state->entries.begin(), memo_state->entries.end(),
                                                           [](const auto& a, const auto& b) {
                                                               return a.second.used < b.second.used;
                                                           });
                            memo_state->bytes -= victim->second.payload_bytes;
                            memo_state->entries.erase(victim);
                        }
                        memo_state->entries.emplace(std::move(key), Memo{out, variables, work - work_before,
                                                                         output_bytes, payload, ++memo_state->clock});
                        memo_state->bytes += payload;
                    }
                }
                own_into(destination, out, dom.identity);
                children_into(dom, destination, variables);
            }
        };
    } // namespace

    ComputedStyle compute_style(const DomSnapshot& root, std::string_view sheet, const StyleProfile& profile) {
        if (profile.properties.size() > constants::style::max_registered_properties) {
            fail("property registry count bound");
        }
        auto ua = rules(profile.ua_stylesheet, profile);
        auto author = rules(std::string(sheet), profile);
        const ProfileIndex index(profile);
        Engine engine{profile, index, ua, author};
        return engine.compute(root, nullptr, {});
    }

    struct StyleMemo::Impl {
        OwnerThread owner;
        Engine::MemoState values;
        std::shared_ptr<const int> binding;
    };

    StyleMemo::StyleMemo() : impl_(std::make_unique<Impl>()) {
    }

    StyleMemo::~StyleMemo() = default;

    void StyleMemo::clear() {
        impl_->owner.require(Diag::StyleMemoOwner);
        impl_->values = {};
        impl_->binding.reset();
    }

    struct CompiledStyle::Impl {
        std::shared_ptr<const int> identity = std::make_shared<const int>(0);
        StyleProfile profile;
        ProfileIndex index;
        std::vector<Rule> ua;
        std::vector<Rule> author;
        std::set<std::string> attributes{"id", "class", "style"};
        std::vector<const dom::Compound*> state_parts;

        Impl(std::string sheet, StyleProfile p) : profile(std::move(p)) {
            if (profile.properties.size() > constants::style::max_registered_properties) {
                fail("property registry count bound");
            }
            index = ProfileIndex(profile);
            ua = rules(profile.ua_stylesheet, profile);
            author = rules(std::move(sheet), profile);
            for (const auto* list : {&ua, &author}) {
                for (const auto& rule : *list) {
                    for (const auto& part : rule.selector.parts) {
                        if (part.hover || part.active) {
                            state_parts.push_back(&part);
                        }
                        for (const auto& attribute : part.attributes) {
                            attributes.insert(attribute.name);
                            attributes.insert(attribute.lower_name);
                        }
                    }
                }
            }
            if (profile.scene_syntax) {
                for (const auto* key :
                     {"fill", "fill-opacity", "fill-rule", "stroke", "stroke-width", "stroke-opacity", "stroke-linecap",
                      "stroke-linejoin", "stroke-dasharray", "stroke-dashoffset", "display", "pointer-events",
                      "font-size", "font-weight", "font-family", "text-anchor", "dominant-baseline", "opacity"}) {
                    attributes.insert(key);
                }
            }
        }
    };

    CompiledStyle::CompiledStyle(std::string sheet, StyleProfile profile)
        : impl_(std::make_unique<Impl>(std::move(sheet), std::move(profile))) {
    }

    CompiledStyle::~CompiledStyle() = default;

    ComputedStyle CompiledStyle::compute(const DomSnapshot& dom) const {
        Engine engine{impl_->profile, impl_->index, impl_->ua, impl_->author, true};
        return engine.compute(dom, nullptr, {});
    }

    ComputedStyle CompiledStyle::compute(const DomSnapshot& dom, StyleMemo& memo) const {
        auto& cache = *memo.impl_;
        cache.owner.require(Diag::StyleMemoOwner);
        if (cache.binding != impl_->identity) {
            cache.values = {};
            cache.binding = impl_->identity;
        }
        Engine engine{impl_->profile, impl_->index, impl_->ua, impl_->author, true, &cache.values};
        return engine.compute(dom, nullptr, {});
    }

    void CompiledStyle::update(const DomSnapshot& dom, ComputedStyle& output, StyleMemo& memo,
                               std::vector<NodeIdentity>* changed) const {
        auto& cache = *memo.impl_;
        cache.owner.require(Diag::StyleMemoOwner);
        if (cache.binding != impl_->identity) {
            cache.values = {};
            cache.binding = impl_->identity;
        }
        if (changed) {
            changed->clear();
        }
        Engine engine{impl_->profile, impl_->index, impl_->ua, impl_->author, true, &cache.values, changed};
        engine.compute_into(dom, output, nullptr, {});
    }

    bool CompiledStyle::refresh_input(const DomSnapshot& dom, StyleInput& input) const {
        using Node = StyleInput::Node;
        const auto& wanted = impl_->attributes;
        // Copies of one node with the same non-zero revision have equal content, so their inputs are equal too.
        const auto unchanged = [](const DomSnapshot& n, const Node& k) {
            return n.revision != 0 && k.revision == n.revision && k.identity == n.identity;
        };
        // The input child read from DOM child `child`, the element child at `index`.
        const auto prior = [](const Node& k, const DomSnapshot& child, std::size_t index) -> const Node* {
            if (index < k.children.size() && k.children[index].identity == child.identity) {
                return &k.children[index];
            }
            for (const auto& c : k.children) {
                if (c.identity == child.identity) {
                    return &c;
                }
            }
            return nullptr;
        };
        // First pass, read only: the bounds of a full read, taking unchanged subtrees' node counts from the input. Inputs
        // match by identity under matching parents, so an unchanged subtree sits at the depth it was read at, which
        // passed the depth bound then.
        std::size_t nodes{};
        const auto count = [&](std::size_t subtree_nodes, unsigned depth) {
            nodes += subtree_nodes;
            if (nodes > std::size_t(constants::max_document_nodes) ||
                depth >= unsigned(constants::max_document_depth)) {
                fail("style input key node/depth bound");
            }
        };
        const auto bounds = [&](auto&& self, const DomSnapshot& n, const Node* k, unsigned depth) -> void {
            if (k && unchanged(n, *k)) {
                count(k->nodes, depth);
                return;
            }
            count(1, depth);
            std::size_t index{};
            for (const auto& child : n.children) {
                if (child.type == constants::element_node_type) {
                    self(self, child, k ? prior(*k, child, index) : nullptr, depth + 1);
                    ++index;
                }
            }
        };
        // hover and active where a state selector can see them.
        const auto state = [&](const DomSnapshot& n, unsigned depth) {
            std::pair<bool, bool> seen{};
            if (n.hovered || n.active) {
                for (const auto* p : impl_->state_parts) {
                    if (dom::matches(*p, LoneElement{&n, depth == 0 && n.local_name == "html"}, false)) {
                        seen.first |= p->hover && n.hovered;
                        seen.second |= p->active && n.active;
                    }
                }
            }
            return seen;
        };
        const auto same_attributes = [&](const DomSnapshot& n, const Node& k) {
            auto it = k.attributes.begin();
            for (const auto& [key, value] : n.attributes) {
                if (wanted.contains(key)) {
                    if (it == k.attributes.end() || it->first != key || it->second != value) {
                        return false;
                    }
                    ++it;
                }
            }
            return it == k.attributes.end();
        };
        // Second pass: brings k up to date with n and returns whether its content changed.
        const auto read = [&](auto&& self, const DomSnapshot& n, Node& k, unsigned depth) -> bool {
            if (unchanged(n, k)) {
                return false;
            }
            bool changed{};
            const auto assign = [&changed](auto& to, const auto& from) {
                if (to != from) {
                    to = from;
                    changed = true;
                }
            };
            assign(k.identity, n.identity);
            assign(k.type, n.type);
            assign(k.local_name, n.local_name);
            assign(k.namespace_uri, n.namespace_uri);
            const auto [hovered, active] = state(n, depth);
            assign(k.hovered, hovered);
            assign(k.active, active);
            if (!same_attributes(n, k)) {
                k.attributes.clear();
                for (const auto& [key, value] : n.attributes) {
                    if (wanted.contains(key)) {
                        k.attributes.emplace(key, value);
                    }
                }
                changed = true;
            }
            // Children keep their inputs by identity; a node new to this parent is read whole.
            std::size_t elements{};
            bool same_children = true;
            for (const auto& child : n.children) {
                if (child.type == constants::element_node_type) {
                    same_children = same_children && elements < k.children.size() &&
                                    k.children[elements].identity == child.identity;
                    ++elements;
                }
            }
            if (!same_children || elements != k.children.size()) {
                std::vector<Node> children;
                children.reserve(elements);
                std::size_t index{};
                for (const auto& child : n.children) {
                    if (child.type == constants::element_node_type) {
                        const auto* found = prior(k, child, index++);
                        if (found) {
                            auto& source = k.children[std::size_t(found - k.children.data())];
                            children.push_back(std::move(source));
                            source.identity = {};
                        } else {
                            children.emplace_back();
                        }
                    }
                }
                k.children = std::move(children);
                changed = true;
            }
            k.nodes = 1;
            std::size_t index{};
            for (const auto& child : n.children) {
                if (child.type == constants::element_node_type) {
                    auto& c = k.children[index++];
                    changed |= self(self, child, c, depth + 1);
                    k.nodes += c.nodes;
                }
            }
            k.revision = n.revision;
            return changed;
        };
        const bool fresh = !input.root_ || input.binding_ != impl_->identity;
        bounds(bounds, dom, fresh ? nullptr : &*input.root_, 0);
        try {
            if (fresh) {
                input.root_.emplace();
                input.binding_ = impl_->identity;
            }
            return read(read, dom, *input.root_, 0) || fresh;
        } catch (...) {
            input.root_.reset();
            input.binding_.reset();
            throw;
        }
    }

    void CompiledStyle::normalize_selector_state(DomSnapshot& dom) const {
        std::size_t nodes{};
        auto visit = [&](auto&& self, DomSnapshot& n, unsigned depth) -> void {
            if (++nodes > constants::max_document_nodes || depth >= constants::max_document_depth) {
                fail("state key node/depth bound");
            }
            bool h{};
            bool a{};
            for (auto* p : impl_->state_parts) {
                if (dom::matches(*p, LoneElement{&n, depth == 0 && n.local_name == "html"}, false)) {
                    h |= p->hover;
                    a |= p->active;
                }
            }
            n.hovered &= h;
            n.active &= a;
            // A normalized copy no longer matches the DOM state its revision names.
            n.revision = 0;
            for (auto& child : n.children) {
                self(self, child, depth + 1);
            }
        };
        visit(visit, dom, 0);
    }

    void validate_style_pair(const DomSnapshot& dom, const ComputedStyle& style) {
        std::size_t nodes{};
        auto visit = [&](auto&& self, const DomSnapshot& n, const ComputedStyle& s, unsigned depth) -> void {
            if (++nodes > constants::max_document_nodes || depth >= constants::max_document_depth) {
                fail("paired snapshot node/depth bound");
            }
            if (n.identity != s.identity || n.local_name != s.local_name) {
                fail("DOM/computed identity mismatch");
            }
            std::size_t i{};
            for (const auto& c : n.children) {
                if (c.type == constants::element_node_type) {
                    if (i >= s.children.size()) {
                        fail("DOM/computed child mismatch");
                    }
                    self(self, c, s.children[i++], depth + 1);
                }
            }
            if (i != s.children.size()) {
                fail("DOM/computed child mismatch");
            }
        };
        visit(visit, dom, style, 0);
    }

    std::array<float, 4> parse_css_color(std::string_view input) {
        const auto s = lower(trim(std::string(input)));
        if (s == "transparent") {
            return {0, 0, 0, 0};
        }
        if (s.size() == constants::style::hex_color_length && s.front() == '#') {
            auto digit = [](char c) {
                if (c >= '0' && c <= '9') {
                    return c - '0';
                }
                if (c >= 'a' && c <= 'f') {
                    return c - 'a' + 10;
                }
                fail("hex color digit");
            };
            return {(digit(s[1]) * 16 + digit(s[2])) / static_cast<float>(constants::color_channel_max),
                    (digit(s[3]) * 16 + digit(s[4])) / static_cast<float>(constants::color_channel_max),
                    (digit(s[5]) * 16 + digit(s[6])) / static_cast<float>(constants::color_channel_max), 1};
        }
        const bool alpha = s.starts_with("rgba(");
        if ((!alpha && !s.starts_with("rgb(")) || !s.ends_with(')')) {
            fail("color outside #rrggbb/transparent/rgb/rgba profile: " + s);
        }
        const auto values = split(s.substr(alpha ? 5 : 4, s.size() - (alpha ? 6 : 5)), ',');
        if (values.size() != (alpha ? 4 : 3)) {
            fail("color component count");
        }
        std::array<float, 4> result{0, 0, 0, 1};
        for (unsigned i = 0; i < values.size(); ++i) {
            result[i] = std::clamp(
                number(trim(values[i])) / (i < 3 ? static_cast<float>(constants::color_channel_max) : 1.f), 0.f, 1.f);
        }
        return result;
    }

    StyleProfile document_style_profile() {
        StyleProfile p;
        p.single_value_inset = true;
        p.four_value_edges = true;
        p.flow_shorthands = true;
        p.ua_stylesheet = "div,p,button,ul,li,section{display:block}i{font-style:italic}b{font-weight:700}";
        p.properties = {
            {"align-content", {"normal", false, CssGrammar::Tokens, {}}},
            {"align-items", {"stretch", false, CssGrammar::Tokens, {}}},
            {"align-self", {"auto", false, CssGrammar::Tokens, {}}},
            {"background-color", {"transparent", false, CssGrammar::Color, {}}},
            {"border", {"0", false, CssGrammar::Length, {}}},
            {"border-bottom-left-radius", {"0", false, CssGrammar::Radius, {}}},
            {"border-bottom-right-radius", {"0", false, CssGrammar::Radius, {}}},
            {"border-bottom-width", {"0", false, CssGrammar::Length, {}}},
            {"border-left-width", {"0", false, CssGrammar::Length, {}}},
            {"border-radius", {"0", false, CssGrammar::Tokens, {}}},
            {"border-right-width", {"0", false, CssGrammar::Length, {}}},
            {"border-top-left-radius", {"0", false, CssGrammar::Radius, {}}},
            {"border-top-right-radius", {"0", false, CssGrammar::Radius, {}}},
            {"border-top-width", {"0", false, CssGrammar::Length, {}}},
            {"border-width", {"0", false, CssGrammar::Length, {}}},
            {"bottom", {"auto", false, CssGrammar::Length, {"auto"}}},
            {"box-sizing", {"content-box", false, CssGrammar::Keyword, {"content-box", "border-box"}}},
            {"color", {constants::default_color, true, CssGrammar::Color, {}}},
            {"column-gap", {"0", false, CssGrammar::Length, {"auto", "normal", "none"}}},
            {"display",
             {"inline",
              false,
              CssGrammar::Keyword,
              {"none", "block", "inline", "inline-block", "flex", "inline-flex", "grid", "inline-grid", "contents",
               "table", "list-item"}}},
            {"flex", {"0 1 auto", false, CssGrammar::Tokens, {}}},
            {"flex-basis", {"auto", false, CssGrammar::Length, {"auto", "normal", "none"}}},
            {"flex-direction", {"row", false, CssGrammar::Tokens, {}}},
            {"flex-grow", {"0", false, CssGrammar::Number, {}}},
            {"flex-shrink", {"1", false, CssGrammar::Number, {}}},
            {"flex-wrap", {"nowrap", false, CssGrammar::Tokens, {}}},
            {"font-family", {"serif", true, CssGrammar::Tokens, {}}},
            {"font-size", {"medium", true, CssGrammar::Length, {"normal", "medium"}}},
            {"font-style", {"normal", true, CssGrammar::Keyword, {"normal", "italic"}}},
            {"font-weight",
             {constants::default_font_weight, true, CssGrammar::Number, {"normal", "bold", "bolder", "lighter"}}},
            {"gap", {"0", false, CssGrammar::Length, {"auto", "normal", "none"}}},
            {"height", {"auto", false, CssGrammar::Length, {"auto"}}},
            {"isolation", {"auto", false, CssGrammar::Keyword, {"auto", "isolate"}}},
            {"justify-content", {"normal", false, CssGrammar::Tokens, {}}},
            {"left", {"auto", false, CssGrammar::Length, {"auto"}}},
            {"line-height", {"normal", true, CssGrammar::Length, {"normal", "medium"}}},
            {"list-style", {"disc", true, CssGrammar::Tokens, {}}},
            {"margin", {"0", false, CssGrammar::Length, {"auto", "normal", "none"}}},
            {"margin-bottom", {"0", false, CssGrammar::Length, {"auto", "normal", "none"}}},
            {"margin-left", {"0", false, CssGrammar::Length, {"auto", "normal", "none"}}},
            {"margin-right", {"0", false, CssGrammar::Length, {"auto", "normal", "none"}}},
            {"margin-top", {"0", false, CssGrammar::Length, {"auto", "normal", "none"}}},
            {"max-height", {"none", false, CssGrammar::Length, {"auto", "normal", "none"}}},
            {"max-width", {"none", false, CssGrammar::Length, {"none"}}},
            {"min-height", {"auto", false, CssGrammar::Length, {"auto", "normal", "none"}}},
            {"min-width", {"auto", false, CssGrammar::Length, {"auto", "normal", "none"}}},
            {"overflow", {"visible", false, CssGrammar::Keyword, {"visible", "hidden", "clip", "auto", "scroll"}}},
            {"overflow-x", {"visible", false, CssGrammar::Keyword, {"visible", "hidden", "auto", "scroll", "clip"}}},
            {"overflow-y", {"visible", false, CssGrammar::Keyword, {"visible", "hidden", "auto", "scroll", "clip"}}},
            {"padding", {"0", false, CssGrammar::Length, {}}},
            {"padding-bottom", {"0", false, CssGrammar::Length, {}}},
            {"padding-left", {"0", false, CssGrammar::Length, {}}},
            {"padding-right", {"0", false, CssGrammar::Length, {}}},
            {"padding-top", {"0", false, CssGrammar::Length, {}}},
            {"pointer-events", {"auto", true, CssGrammar::Keyword, {"auto", "none"}}},
            {"position", {"static", false, CssGrammar::Keyword, {"static", "relative", "absolute", "fixed", "sticky"}}},
            {"right", {"auto", false, CssGrammar::Length, {"auto"}}},
            {"row-gap", {"0", false, CssGrammar::Length, {"auto", "normal", "none"}}},
            {"text-align", {"left", true, CssGrammar::Keyword, {"left", "right", "center", "justify", "start", "end"}}},
            {"top", {"auto", false, CssGrammar::Length, {"auto"}}},
            {"user-select", {"auto", false, CssGrammar::Tokens, {}}},
            {"white-space",
             {"normal",
              true,
              CssGrammar::Keyword,
              {"normal", "nowrap", "pre", "pre-wrap", "pre-line", "break-spaces"}}},
            {"width", {"auto", false, CssGrammar::Length, {"auto"}}},
            {"z-index", {"auto", false, CssGrammar::Number, {"auto"}}},
        };
        return p;
    }
} // namespace weft
