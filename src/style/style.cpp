#include "style/style.hpp"
#include "base/css_scalar.hpp"
#include "style/background_layer.hpp"
#include "style/color.hpp"
#include "base/owner_thread.hpp"
#include "constants/common.hpp"
#include "constants/style.hpp"
#include "dom/selector.hpp"
#include "text/bidi.hpp"
#include <thread>
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <map>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>

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

            friend bool operator==(const Token& a, const Token& b) {
                return a.kind == b.kind && a.text == b.text && a.args == b.args;
            }
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
            // A forgiving profile's values: a parenthesized simple block is a function token without a name
            // (calc((a + b) / 2)), and strings may escape their quotes.
            bool forgiving{};

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
                            if (forgiving && text[p] == '\\' && p + 1 < text.size()) {
                                ++p;
                            }
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
                    if (forgiving && c == '(') {
                        ++p;
                        out.push_back({Kind::Function, std::string(), scan(depth + 1)});
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

        // The longhands of one background layer as the scene expands the shorthand, with the parts of it they take
        // and the initial values they take without one.
        struct BackgroundLonghand {
            const char* name;
            BackgroundPart part;
            const char* initial;
        };

        constexpr std::array<BackgroundLonghand, 8> background_longhands{{
            {"background-color", BackgroundPart::Color, "transparent"},
            {"background-image", BackgroundPart::Image, "none"},
            {"background-position", BackgroundPart::Position, "0% 0%"},
            {"background-size", BackgroundPart::Size, "auto"},
            {"background-repeat", BackgroundPart::Repeat, "repeat"},
            {"background-attachment", BackgroundPart::Attachment, "scroll"},
            {"background-origin", BackgroundPart::Origin, "padding-box"},
            {"background-clip", BackgroundPart::Clip, "border-box"},
        }};

        // The longhands of the scene's transition shorthand, in the order of the parts of a transition
        // (TransitionParts), with the initial values they take for a transition that leaves its part out.
        struct TransitionLonghand {
            const char* name;
            const char* initial;
        };

        constexpr std::array<TransitionLonghand, 4> transition_longhands{{
            {"transition-property", "all"},
            {"transition-duration", "0s"},
            {"transition-timing-function", "ease"},
            {"transition-delay", "0s"},
        }};

        // Each longhand's list of a transition shorthand's value, its items in the order of the transitions; or why
        // the value is not one.
        struct TransitionParts {
            std::array<Tokens, transition_longhands.size()> lists;
            std::string error;
        };

        // A <time>: a dimension in s or ms (no unitless zero).
        bool time_token(const Token& t) {
            if (t.kind != Kind::Dimension) {
                return false;
            }
            auto unit = t.text.size();
            while (unit && std::isalpha(static_cast<unsigned char>(t.text[unit - 1]))) {
                --unit;
            }
            const auto name = lower(t.text.substr(unit));
            return name == "s" || name == "ms";
        }

        bool easing_token(const Token& t) {
            const auto name = lower(t.text);
            if (t.kind == Kind::Function) {
                return name == "cubic-bezier" || name == "steps" || name == "linear";
            }
            return t.kind == Kind::Ident &&
                   (name == "linear" || name == "ease" || name == "ease-in" || name == "ease-out" ||
                    name == "ease-in-out" || name == "step-start" || name == "step-end");
        }

        // A token with its names, units and keywords in lowercase (they are ASCII case-insensitive), strings as
        // they are.
        Token lowered(const Token& t) {
            Token out{t.kind, t.kind == Kind::String ? t.text : lower(t.text), {}};
            for (const auto& a : t.args) {
                out.args.push_back(lowered(a));
            }
            return out;
        }

        // The transition shorthand (CSS Transitions 1 §2.6): transitions separated by commas, each a property name
        // (or none, alone), a duration and then a delay, a timing function and a transition behavior in any order;
        // a part left out takes its longhand's initial value, and the lists have the parts in lowercase. The scene
        // interpolates every property it transitions, so of the behaviors only normal (the initial one) is in its
        // profile.
        TransitionParts transition_parts(const Tokens& tokens) {
            TransitionParts out;
            std::vector<Tokens> transitions(1);
            for (const auto& t : tokens) {
                if (t.kind == Kind::Delim && t.text == ",") {
                    transitions.emplace_back();
                } else {
                    transitions.back().push_back(t);
                }
            }
            for (std::size_t i = 0; i < transitions.size(); ++i) {
                const auto& transition = transitions[i];
                if (transition.empty()) {
                    out.error = "transition shorthand: an empty transition in the list";
                    return out;
                }
                std::optional<Token> property;
                std::optional<Token> easing;
                std::vector<Token> times;
                bool behavior{};
                for (const auto& t : transition) {
                    const auto word = t.kind == Kind::Ident ? lower(t.text) : std::string();
                    if (time_token(t)) {
                        if (times.size() == 2) {
                            out.error =
                                "transition shorthand: more than a duration and a delay in " + serialize(transition);
                            return out;
                        }
                        if (times.empty() && t.text.starts_with('-')) {
                            out.error = "transition shorthand: a negative duration in " + serialize(transition);
                            return out;
                        }
                        times.push_back(lowered(t));
                    } else if (easing_token(t)) {
                        if (easing) {
                            out.error = "transition shorthand: two timing functions in " + serialize(transition);
                            return out;
                        }
                        easing = lowered(t);
                    } else if (word == "normal" || word == "allow-discrete") {
                        if (word == "allow-discrete") {
                            out.error = "transition-behavior: allow-discrete outside the scene profile";
                            return out;
                        }
                        if (behavior) {
                            out.error = "transition shorthand: two transition behaviors in " + serialize(transition);
                            return out;
                        }
                        behavior = true;
                    } else if (t.kind == Kind::Ident && !wide(word) && word != "default") {
                        if (property) {
                            out.error = "transition shorthand: two properties in " + serialize(transition);
                            return out;
                        }
                        if (word == "none" && transitions.size() > 1) {
                            out.error = "transition shorthand: none in a list of transitions";
                            return out;
                        }
                        property = Token{Kind::Ident, word, {}};
                    } else {
                        out.error = "transition shorthand: " + serialize({t}) +
                                    " is no property name, time, timing function or transition behavior";
                        return out;
                    }
                }
                const std::array<std::optional<Token>, transition_longhands.size()> parts{
                    property, times.empty() ? std::nullopt : std::optional(times[0]), easing,
                    times.size() < 2 ? std::nullopt : std::optional(times[1])};
                for (std::size_t part = 0; part < parts.size(); ++part) {
                    auto& list = out.lists[part];
                    if (i) {
                        list.push_back({Kind::Delim, ",", {}});
                    }
                    if (parts[part]) {
                        list.push_back(*parts[part]);
                    } else {
                        const std::string initial = transition_longhands[part].initial;
                        const bool time = std::isdigit(static_cast<unsigned char>(initial[0]));
                        list.push_back({time ? Kind::Dimension : Kind::Ident, initial, {}});
                    }
                }
            }
            return out;
        }

        // The longhands of the mask shorthand (CSS Masking 1 §4.11), with their initial values.
        struct MaskLonghand {
            const char* name;
            const char* initial;
        };

        constexpr std::array<MaskLonghand, 8> mask_longhands{{
            {"mask-image", "none"},
            {"mask-position", "0% 0%"},
            {"mask-size", "auto"},
            {"mask-repeat", "repeat"},
            {"mask-origin", "border-box"},
            {"mask-clip", "border-box"},
            {"mask-composite", "add"},
            {"mask-mode", "match-source"},
        }};

        // Each longhand's list of a mask shorthand's value, its items in the order of the layers; or why the value is
        // not one. A layer: <mask-reference> || <position> [ / <bg-size> ]? || <repeat-style> || <geometry-box> ||
        // [ <geometry-box> | no-clip ] || <compositing-operator> || <masking-mode>, its parts in any order; one box
        // sets the origin and the clip, a second the clip; a part left out takes its longhand's initial value.
        struct MaskParts {
            std::array<Tokens, mask_longhands.size()> lists;
            std::string error;
        };

        MaskParts mask_parts(const Tokens& tokens) {
            MaskParts out;
            std::vector<Tokens> layers(1);
            for (const auto& t : tokens) {
                if (t.kind == Kind::Delim && t.text == ",") {
                    layers.emplace_back();
                } else {
                    layers.back().push_back(t);
                }
            }
            const auto is = [](const std::string& word, std::initializer_list<const char*> set) {
                for (const auto* k : set) {
                    if (word == k) {
                        return true;
                    }
                }
                return false;
            };
            for (std::size_t l = 0; l < layers.size(); ++l) {
                const auto& layer = layers[l];
                if (layer.empty()) {
                    out.error = "mask shorthand: an empty layer";
                    return out;
                }
                std::array<std::optional<Tokens>, mask_longhands.size()> parts;
                const auto set = [&](std::size_t part, Tokens value) {
                    if (parts[part]) {
                        out.error = "mask shorthand: two " + std::string(mask_longhands[part].name) + " values in " +
                                    serialize(layer);
                        return false;
                    }
                    parts[part] = std::move(value);
                    return true;
                };
                for (std::size_t i = 0; i < layer.size();) {
                    const auto& t = layer[i];
                    const auto word = t.kind == Kind::Ident ? lower(t.text) : std::string();
                    const bool positional =
                        t.kind == Kind::Dimension || t.kind == Kind::Percentage || t.kind == Kind::Number ||
                        is(word, {"left", "right", "top", "bottom", "center"}) ||
                        (t.kind == Kind::Function && lower(t.text) == "calc");
                    if (t.kind == Kind::Function && lower(t.text) != "calc") {
                        if (!set(0, {lowered(t)})) {
                            return out;
                        }
                        ++i;
                    } else if (word == "none") {
                        if (!set(0, {{Kind::Ident, "none", {}}})) {
                            return out;
                        }
                        ++i;
                    } else if (positional) {
                        Tokens position;
                        while (i < layer.size() && position.size() < 4) {
                            const auto& p = layer[i];
                            const auto w = p.kind == Kind::Ident ? lower(p.text) : std::string();
                            if (!(p.kind == Kind::Dimension || p.kind == Kind::Percentage || p.kind == Kind::Number ||
                                  is(w, {"left", "right", "top", "bottom", "center"}) ||
                                  (p.kind == Kind::Function && lower(p.text) == "calc"))) {
                                break;
                            }
                            position.push_back(lowered(p));
                            ++i;
                        }
                        if (!set(1, std::move(position))) {
                            return out;
                        }
                        if (i < layer.size() && layer[i].kind == Kind::Delim && layer[i].text == "/") {
                            ++i;
                            Tokens size;
                            while (i < layer.size() && size.size() < 2) {
                                const auto& p = layer[i];
                                const auto w = p.kind == Kind::Ident ? lower(p.text) : std::string();
                                if (!(p.kind == Kind::Dimension || p.kind == Kind::Percentage ||
                                      p.kind == Kind::Number || w == "auto" ||
                                      (size.empty() && (w == "cover" || w == "contain")) ||
                                      (p.kind == Kind::Function && lower(p.text) == "calc"))) {
                                    break;
                                }
                                size.push_back(lowered(p));
                                ++i;
                                if (w == "cover" || w == "contain") {
                                    break;
                                }
                            }
                            if (size.empty() || !set(2, std::move(size))) {
                                if (out.error.empty()) {
                                    out.error = "mask shorthand: no size after / in " + serialize(layer);
                                }
                                return out;
                            }
                        }
                    } else if (is(word, {"repeat-x", "repeat-y"})) {
                        if (!set(3, {{Kind::Ident, word, {}}})) {
                            return out;
                        }
                        ++i;
                    } else if (is(word, {"repeat", "space", "round", "no-repeat"})) {
                        Tokens repeat{{Kind::Ident, word, {}}};
                        ++i;
                        if (i < layer.size() && layer[i].kind == Kind::Ident &&
                            is(lower(layer[i].text), {"repeat", "space", "round", "no-repeat"})) {
                            repeat.push_back({Kind::Ident, lower(layer[i].text), {}});
                            ++i;
                        }
                        if (!set(3, std::move(repeat))) {
                            return out;
                        }
                    } else if (is(word, {"border-box", "padding-box", "content-box", "margin-box", "fill-box",
                                         "stroke-box", "view-box"})) {
                        // The first box is the origin (and the clip, unless a second comes); the second the clip.
                        if (!parts[4]) {
                            parts[4] = Tokens{{Kind::Ident, word, {}}};
                        } else if (!set(5, {{Kind::Ident, word, {}}})) {
                            return out;
                        }
                        ++i;
                    } else if (word == "no-clip") {
                        if (!set(5, {{Kind::Ident, word, {}}})) {
                            return out;
                        }
                        ++i;
                    } else if (is(word, {"add", "subtract", "intersect", "exclude"})) {
                        if (!set(6, {{Kind::Ident, word, {}}})) {
                            return out;
                        }
                        ++i;
                    } else if (is(word, {"alpha", "luminance", "match-source"})) {
                        if (!set(7, {{Kind::Ident, word, {}}})) {
                            return out;
                        }
                        ++i;
                    } else {
                        out.error = "mask shorthand: " + serialize({t}) + " is no part of a mask layer";
                        return out;
                    }
                }
                if (parts[4] && !parts[5]) {
                    parts[5] = parts[4];
                }
                for (std::size_t part = 0; part < parts.size(); ++part) {
                    auto& list = out.lists[part];
                    if (l) {
                        list.push_back({Kind::Delim, ",", {}});
                    }
                    if (parts[part]) {
                        list.insert(list.end(), parts[part]->begin(), parts[part]->end());
                    } else {
                        for (const auto& word : css::split(mask_longhands[part].initial, ' ').parts) {
                            list.push_back(word.ends_with('%') ? Token{Kind::Percentage, word, {}}
                                                               : Token{Kind::Ident, word, {}});
                        }
                    }
                }
            }
            return out;
        }

        // -webkit-mask-composite's legacy operators as mask-composite's.
        Tokens standard_mask_composite(const Tokens& tokens) {
            Tokens out;
            for (const auto& t : tokens) {
                const auto word = t.kind == Kind::Ident ? lower(t.text) : std::string();
                const auto* standard = word == "source-over"   ? "add"
                                       : word == "source-out"  ? "subtract"
                                       : word == "source-in"   ? "intersect"
                                       : word == "xor"         ? "exclude"
                                       : word == "add" || word == "subtract" || word == "intersect" ||
                                               word == "exclude"
                                           ? word.c_str()
                                           : nullptr;
                if (t.kind == Kind::Ident && !wide(word) && !standard) {
                    fail("-webkit-mask-composite " + t.text);
                }
                out.push_back(standard ? Token{Kind::Ident, standard, {}} : t);
            }
            return out;
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
            // A longhand of a scene background shorthand (background_longhands), which takes its part of the value
            // or its initial value.
            int background_layer{-1};
            // A longhand of a scene transition shorthand (transition_longhands), which takes its list of the value.
            int transition_part{-1};
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

        // What a forgiving sheet and its cascades dropped: each text once, at most max_style_warnings.
        struct Warnings {
            std::vector<std::string> list;
            std::set<std::string> seen;

            void add(std::string text) {
                if (list.size() >= std::size_t(constants::style::max_style_warnings) || seen.contains(text)) {
                    return;
                }
                seen.insert(text);
                list.push_back(std::move(text));
            }
        };

        // Text cut to a length for a warning.
        std::string excerpt(std::string_view text, std::size_t length = 96) {
            auto t = trim(text);
            if (t.size() > length) {
                t = t.substr(0, length) + "...";
            }
            return t;
        }

        // A logical property's physical longhands in a horizontal, left-to-right writing mode (CSS Logical
        // Properties 1): their names and which value of a two-value shorthand each takes (-1: the whole value).
        const std::vector<std::pair<std::string, int>>* logical_longhands(const std::string& key) {
            static const auto table = [] {
                std::map<std::string, std::vector<std::pair<std::string, int>>> t;
                for (const std::string box : {"margin", "padding"}) {
                    t[box + "-inline"] = {{box + "-left", 0}, {box + "-right", 1}};
                    t[box + "-block"] = {{box + "-top", 0}, {box + "-bottom", 1}};
                    t[box + "-inline-start"] = {{box + "-left", -1}};
                    t[box + "-inline-end"] = {{box + "-right", -1}};
                    t[box + "-block-start"] = {{box + "-top", -1}};
                    t[box + "-block-end"] = {{box + "-bottom", -1}};
                }
                t["inset-inline"] = {{"left", 0}, {"right", 1}};
                t["inset-block"] = {{"top", 0}, {"bottom", 1}};
                t["inset-inline-start"] = {{"left", -1}};
                t["inset-inline-end"] = {{"right", -1}};
                t["inset-block-start"] = {{"top", -1}};
                t["inset-block-end"] = {{"bottom", -1}};
                for (const std::string part : {"width", "style", "color"}) {
                    t["border-inline-" + part] = {{"border-left-" + part, 0}, {"border-right-" + part, 1}};
                    t["border-block-" + part] = {{"border-top-" + part, 0}, {"border-bottom-" + part, 1}};
                    t["border-inline-start-" + part] = {{"border-left-" + part, -1}};
                    t["border-inline-end-" + part] = {{"border-right-" + part, -1}};
                    t["border-block-start-" + part] = {{"border-top-" + part, -1}};
                    t["border-block-end-" + part] = {{"border-bottom-" + part, -1}};
                }
                t["border-inline"] = {{"border-left", -1}, {"border-right", -1}};
                t["border-block"] = {{"border-top", -1}, {"border-bottom", -1}};
                t["border-inline-start"] = {{"border-left", -1}};
                t["border-inline-end"] = {{"border-right", -1}};
                t["border-block-start"] = {{"border-top", -1}};
                t["border-block-end"] = {{"border-bottom", -1}};
                t["border-start-start-radius"] = {{"border-top-left-radius", -1}};
                t["border-start-end-radius"] = {{"border-top-right-radius", -1}};
                t["border-end-start-radius"] = {{"border-bottom-left-radius", -1}};
                t["border-end-end-radius"] = {{"border-bottom-right-radius", -1}};
                t["inline-size"] = {{"width", -1}};
                t["block-size"] = {{"height", -1}};
                t["min-inline-size"] = {{"min-width", -1}};
                t["min-block-size"] = {{"min-height", -1}};
                t["max-inline-size"] = {{"max-width", -1}};
                t["max-block-size"] = {{"max-height", -1}};
                t["overflow-inline"] = {{"overflow-x", -1}};
                t["overflow-block"] = {{"overflow-y", -1}};
                return t;
            }();
            const auto it = table.find(key);
            return it == table.end() ? nullptr : &it->second;
        }

        // The flex shorthand (CSS Flexbox 1 §7.2) as its longhands: none, auto, initial, a CSS-wide keyword, or
        // <grow> [<shrink>]? || <basis>.
        void flex_shorthand(const Tokens& tokens, bool important, std::vector<Declaration>& out) {
            Tokens grow{{Kind::Number, "1", {}}};
            Tokens shrink{{Kind::Number, "1", {}}};
            Tokens basis{{Kind::Percentage, "0%", {}}};
            const auto word = keyword(tokens);
            if (references(tokens)) {
                fail("flex shorthand with var()");
            }
            if (wide(word)) {
                grow = shrink = basis = tokens;
            } else if (word == "none") {
                grow = shrink = {{Kind::Number, "0", {}}};
                basis = {{Kind::Ident, "auto", {}}};
            } else if (word == "auto") {
                basis = {{Kind::Ident, "auto", {}}};
            } else if (word == "initial") {
                grow = {{Kind::Number, "0", {}}};
                basis = {{Kind::Ident, "auto", {}}};
            } else {
                std::vector<Token> numbers;
                std::optional<Token> length;
                for (const auto& t : tokens) {
                    if (t.kind == Kind::Number && numbers.size() < 2 && !length) {
                        numbers.push_back(t);
                    } else if (!length &&
                               (t.kind == Kind::Dimension || t.kind == Kind::Percentage || t.kind == Kind::Number ||
                                t.kind == Kind::Function ||
                                (t.kind == Kind::Ident && (lower(t.text) == "auto" || lower(t.text) == "content")))) {
                        length = t;
                    } else if (t.kind == Kind::Number && length && numbers.size() < 2 && numbers.size() >= 1) {
                        numbers.push_back(t);
                    } else {
                        fail("flex shorthand: " + serialize(tokens));
                    }
                }
                if (numbers.empty() && !length) {
                    fail("flex shorthand: " + serialize(tokens));
                }
                if (!numbers.empty()) {
                    grow = {numbers[0]};
                }
                if (numbers.size() > 1) {
                    shrink = {numbers[1]};
                }
                if (length) {
                    basis = {*length};
                }
            }
            out.push_back({"flex-grow", grow, important});
            out.push_back({"flex-shrink", shrink, important});
            out.push_back({"flex-basis", basis, important});
        }

        // The font shorthand (CSS Fonts 4 §2.7) as the longhands a scene draws: [<style> || <variant> || <weight> ||
        // <stretch>]? <size> [/ <line-height>]? <family>; a CSS-wide keyword sets them all. Variant and stretch are
        // read and left out (the scene draws neither).
        void font_shorthand(const Tokens& tokens, bool important, std::vector<Declaration>& out) {
            static constexpr const char* longhands[]{"font-style", "font-weight", "font-size", "line-height",
                                                     "font-family"};
            if (wide(keyword(tokens))) {
                for (const auto* name : longhands) {
                    out.push_back({name, tokens, important});
                }
                return;
            }
            if (references(tokens)) {
                fail("font shorthand with var()");
            }
            Tokens style{{Kind::Ident, "normal", {}}};
            Tokens weight{{Kind::Ident, "normal", {}}};
            Tokens line{{Kind::Ident, "normal", {}}};
            std::size_t i{};
            for (; i < tokens.size(); ++i) {
                const auto& t = tokens[i];
                const auto w = lower(t.text);
                if (t.kind == Kind::Ident &&
                    (w == "normal" || w == "small-caps" || w.ends_with("condensed") || w.ends_with("expanded"))) {
                    continue;
                }
                if (t.kind == Kind::Ident && (w == "italic" || w == "oblique")) {
                    style = {t};
                } else if ((t.kind == Kind::Ident && (w == "bold" || w == "bolder" || w == "lighter")) ||
                           t.kind == Kind::Number) {
                    weight = {t};
                } else {
                    break;
                }
            }
            if (i >= tokens.size()) {
                fail("font shorthand without a size");
            }
            Tokens size{tokens[i++]};
            if (i < tokens.size() && tokens[i].kind == Kind::Delim && tokens[i].text == "/") {
                if (++i >= tokens.size()) {
                    fail("font shorthand line height");
                }
                line = {tokens[i++]};
            }
            Tokens family(tokens.begin() + std::ptrdiff_t(i), tokens.end());
            if (family.empty()) {
                fail("font shorthand without a family");
            }
            out.push_back({"font-style", style, important});
            out.push_back({"font-weight", weight, important});
            out.push_back({"font-size", size, important});
            out.push_back({"line-height", line, important});
            out.push_back({"font-family", family, important});
        }

        bool grid_placement_shorthand(std::string_view key) {
            return key == "grid-row" || key == "grid-column" || key == "grid-area";
        }

        // The longhands of grid-row, grid-column or grid-area (CSS Grid 1 §8.4): lines separated by "/", grid-area's
        // the row start, column start, row end and column end. An omitted line is the line before it in the same axis
        // when that is a custom name, else auto.
        void grid_placement(const std::string& key, const Tokens& tokens, bool important,
                            std::vector<Declaration>& out) {
            if (references(tokens)) {
                fail(key + " shorthand with var() outside profile");
            }
            const std::vector<std::string> names =
                key == "grid-row"      ? std::vector<std::string>{"grid-row-start", "grid-row-end"}
                : key == "grid-column" ? std::vector<std::string>{"grid-column-start", "grid-column-end"}
                                       : std::vector<std::string>{"grid-row-start", "grid-column-start", "grid-row-end",
                                                                  "grid-column-end"};
            if (wide(keyword(tokens))) {
                for (const auto& name : names) {
                    out.push_back({name, tokens, important});
                }
                return;
            }
            std::vector<Tokens> lines(1);
            for (const auto& t : tokens) {
                if (t.kind == Kind::Delim && t.text == "/") {
                    lines.emplace_back();
                } else {
                    lines.back().push_back(t);
                }
            }
            if (lines.size() > names.size() || std::any_of(lines.begin(), lines.end(), [](const Tokens& l) {
                    return l.empty();
                })) {
                fail(key + " shorthand");
            }
            const auto named = [](const Tokens& l) {
                const auto word = keyword(l);
                return !word.empty() && word != "auto" && word != "span";
            };
            while (lines.size() < names.size()) {
                // Two lines: the end takes the start's name. grid-area: the column start and row end take the row
                // start's, the column end the column start's.
                const auto from = names.size() == 4 && lines.size() == 3 ? 1 : 0;
                const auto line = lines[std::size_t(from)];
                lines.push_back(named(line) ? line : Tokens{{Kind::Ident, "auto", {}}});
            }
            for (std::size_t i = 0; i < names.size(); ++i) {
                out.push_back({names[i], lines[i], important});
            }
        }

        void declaration(std::string_view part, const StyleProfile& profile, std::vector<Declaration>& result);

        std::vector<Declaration> declarations(std::string text, const StyleProfile& profile,
                                              Warnings* warnings = nullptr) {
            if (text.size() >
                (profile.forgiving ? constants::max_text_bytes : constants::style::max_declaration_bytes)) {
                fail("declaration byte bound");
            }
            std::vector<Declaration> result;
            for (auto part : split(text, ';')) {
                if (trim(part).empty()) {
                    continue;
                }
                if (!profile.forgiving) {
                    declaration(part, profile, result);
                    continue;
                }
                // CSS drops a declaration it cannot parse, and keeps the others (CSS Syntax 3 §5.4.6).
                std::vector<Declaration> one;
                try {
                    declaration(part, profile, one);
                    for (auto& d : one) {
                        if (!custom(d.key)) {
                            const auto id = find_property(d.key);
                            if (!id) {
                                fail("unregistered property " + d.key);
                            }
                            d.id = *id;
                        }
                    }
                } catch (const std::invalid_argument& e) {
                    if (warnings) {
                        warnings->add(std::string(e.what()) + " (" + excerpt(part) + ")");
                    }
                    continue;
                }
                result.insert(result.end(), std::make_move_iterator(one.begin()), std::make_move_iterator(one.end()));
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

        void declaration(std::string_view part, const StyleProfile& profile, std::vector<Declaration>& result) {
            {
                const auto colon = part.find(':');
                if (colon == std::string::npos) {
                    fail("declaration colon");
                }
                auto key = trim(part.substr(0, colon));
                auto v = trim(part.substr(colon + 1));
                if (!custom(key)) {
                    key = lower(key);
                }
                // word-wrap is a legacy name of overflow-wrap (CSS Text 3 §5.5).
                if (key == "word-wrap" && profile.properties.contains("overflow-wrap")) {
                    key = "overflow-wrap";
                }
                if (profile.forgiving && !custom(key)) {
                    if (std::find(profile.ignored.begin(), profile.ignored.end(), key) != profile.ignored.end() ||
                        std::any_of(profile.ignored_prefixes.begin(), profile.ignored_prefixes.end(),
                                    [&](const std::string& prefix) {
                                        return key.starts_with(prefix);
                                    })) {
                        return;
                    }
                    if (const auto* physical = logical_longhands(key)) {
                        for (const auto& [name, index] : *physical) {
                            const auto before = result.size();
                            declaration(name + ":" + v, profile, result);
                            if (index >= 0) {
                                for (auto i = before; i < result.size(); ++i) {
                                    result[i].axis = index;
                                }
                            }
                        }
                        return;
                    }
                }
                const bool border_side =
                    key == "border-top" || key == "border-right" || key == "border-bottom" || key == "border-left";
                if (!custom(key) && !profile.properties.contains(key) &&
                    !(profile.scene_syntax &&
                      (key == "border" || border_side || key == "flex" || key == "transition" ||
                       (key == "outline" && profile.properties.contains("outline-style")) ||
                       (key == "text-decoration" && profile.properties.contains("text-decoration-line")) ||
                       ((key == "mask" || key == "-webkit-mask" ||
                         (key.starts_with("-webkit-mask-") &&
                          profile.properties.contains(key.substr(std::string_view("-webkit-").size())))) &&
                        profile.properties.contains("mask-image")))) &&
                    !(profile.forgiving && (key == "font" || key == "place-content" || key == "place-self")) &&
                    !(grid_placement_shorthand(key) && profile.properties.contains("grid-row-start")) &&
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
                lexer.forgiving = profile.forgiving;
                auto tokens = lexer.scan();
                references(tokens);
                if (tokens.empty() && !custom(key)) {
                    fail("empty declaration " + key);
                }
                if (profile.forgiving && key == "flex") {
                    flex_shorthand(tokens, important, result);
                } else if (profile.forgiving && key == "font") {
                    font_shorthand(tokens, important, result);
                } else if (profile.forgiving && key == "gap") {
                    // Rows, then columns; one value is both. Never `gap` beside its longhands (the layout adapter
                    // takes one form).
                    result.push_back({"row-gap", tokens, important, false, -1, -1, 0});
                    result.push_back({"column-gap", tokens, important, false, -1, -1, 1});
                } else if (grid_placement_shorthand(key) && profile.properties.contains("grid-row-start")) {
                    grid_placement(key, tokens, important, result);
                } else if (profile.forgiving &&
                           (key == "place-content" || key == "place-self" || key == "place-items")) {
                    const auto suffix = key.substr(std::string("place").size());
                    result.push_back({"align" + suffix, tokens, important, false, -1, -1, 0});
                    if (profile.properties.contains("justify" + suffix)) {
                        result.push_back({"justify" + suffix, tokens, important, false, -1, -1, 1});
                    }
                } else if (profile.forgiving && key == "border-radius") {
                    int corner{};
                    for (const auto* name : {"border-top-left-radius", "border-top-right-radius",
                                             "border-bottom-right-radius", "border-bottom-left-radius"}) {
                        result.push_back({name, tokens, important, false, -1, corner++});
                    }
                } else if (profile.scene_syntax && (key == "border" || border_side)) {
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
                } else if (profile.scene_syntax && key == "outline" && profile.properties.contains("outline-style")) {
                    // Width, style and color in any order, as border's; auto is a style too. What is left out takes its
                    // initial value.
                    Tokens width{{Kind::Ident, "medium", {}}};
                    Tokens style{{Kind::Ident, "none", {}}};
                    Tokens color{{Kind::Ident, "currentcolor", {}}};
                    for (const auto& token : tokens) {
                        const auto word = lower(token.text);
                        if (token.kind == Kind::Dimension || token.kind == Kind::Number ||
                            (token.kind == Kind::Ident && (word == "thin" || word == "medium" || word == "thick"))) {
                            width = {token};
                        } else if (token.kind == Kind::Ident && (border_style_keyword(word) || word == "auto")) {
                            style = {{Kind::Ident, word, {}}};
                        } else if (token.kind == Kind::Hash || token.kind == Kind::Function ||
                                   token.kind == Kind::Ident) {
                            color = {token};
                        } else {
                            fail("outline shorthand profile");
                        }
                    }
                    result.push_back({"outline-width", width, important});
                    result.push_back({"outline-style", style, important});
                    result.push_back({"outline-color", color, important});
                } else if (profile.scene_syntax && key == "text-decoration" &&
                           profile.properties.contains("text-decoration-line")) {
                    // Lines, style, color and thickness in any order (CSS Text Decoration 4 §2.6); a CSS-wide keyword
                    // is every longhand's. What is left out takes its initial value.
                    Tokens line{{Kind::Ident, "none", {}}};
                    Tokens style{{Kind::Ident, "solid", {}}};
                    Tokens color{{Kind::Ident, "currentcolor", {}}};
                    Tokens thickness{{Kind::Ident, "auto", {}}};
                    if (wide(keyword(tokens))) {
                        line = style = color = thickness = tokens;
                    } else {
                        Tokens lines;
                        for (const auto& token : tokens) {
                            const auto word = lower(token.text);
                            if (token.kind == Kind::Ident &&
                                (word == "underline" || word == "overline" || word == "line-through" ||
                                 word == "none" || word == "blink")) {
                                lines.push_back(token);
                            } else if (token.kind == Kind::Ident &&
                                       (word == "solid" || word == "double" || word == "dotted" || word == "dashed" ||
                                        word == "wavy")) {
                                style = {{Kind::Ident, word, {}}};
                            } else if (token.kind == Kind::Dimension || token.kind == Kind::Percentage ||
                                       (token.kind == Kind::Ident && (word == "auto" || word == "from-font"))) {
                                thickness = {token};
                            } else if (token.kind == Kind::Hash || token.kind == Kind::Function ||
                                       token.kind == Kind::Ident) {
                                color = {token};
                            } else {
                                fail("text-decoration shorthand profile");
                            }
                        }
                        if (!lines.empty()) {
                            line = std::move(lines);
                        }
                    }
                    result.push_back({"text-decoration-line", line, important});
                    result.push_back({"text-decoration-style", style, important});
                    result.push_back({"text-decoration-color", color, important});
                    result.push_back({"text-decoration-thickness", thickness, important});
                } else if (profile.scene_syntax && (key == "mask" || key == "-webkit-mask") &&
                           profile.properties.contains("mask-image")) {
                    // Every longhand its list, a layer a comma apart; a CSS-wide keyword is every longhand's.
                    if (references(tokens)) {
                        fail("mask shorthand with var()");
                    }
                    if (wide(keyword(tokens))) {
                        for (const auto& longhand : mask_longhands) {
                            result.push_back({longhand.name, tokens, important});
                        }
                    } else {
                        auto parts = mask_parts(tokens);
                        if (!parts.error.empty()) {
                            fail(parts.error);
                        }
                        for (std::size_t part = 0; part < mask_longhands.size(); ++part) {
                            result.push_back({mask_longhands[part].name, std::move(parts.lists[part]), important});
                        }
                    }
                } else if (profile.scene_syntax && key.starts_with("-webkit-mask-") &&
                           profile.properties.contains("mask-image")) {
                    // Chrome's names of the mask longhands; -webkit-mask-composite takes the legacy operators.
                    auto name = key.substr(std::string_view("-webkit-").size());
                    if (name == "mask-composite") {
                        tokens = standard_mask_composite(tokens);
                    }
                    result.push_back({std::move(name), std::move(tokens), important});
                } else if (profile.scene_syntax && key == "flex") {
                    if (tokens.size() != 1 || tokens[0].kind != Kind::Number) {
                        fail("flex shorthand profile requires one number");
                    }
                    result.push_back({"flex-grow", tokens, important});
                    result.push_back({"flex-shrink", {{Kind::Number, "1", {}}}, important});
                    result.push_back({"flex-basis", {{Kind::Percentage, "0%", {}}}, important});
                } else if (key == "background" && profile.scene_syntax) {
                    // Every longhand of the layer, so they cascade with the others; which part of the value each
                    // takes is known once var() is substituted.
                    for (std::size_t layer = 0; layer < background_longhands.size(); ++layer) {
                        result.push_back({background_longhands[layer].name, tokens, important});
                        result.back().background_layer = int(layer);
                    }
                } else if (key == "transition" && profile.scene_syntax) {
                    // Every longhand, each taking its list once var() is substituted; a value without var() that is
                    // no transition list is the stylesheet's error.
                    if (!references(tokens) && !wide(keyword(tokens))) {
                        if (const auto parts = transition_parts(tokens); !parts.error.empty()) {
                            fail(parts.error);
                        }
                    }
                    for (std::size_t longhand = 0; longhand < transition_longhands.size(); ++longhand) {
                        result.push_back({transition_longhands[longhand].name, tokens, important});
                        result.back().transition_part = int(longhand);
                    }
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
                            (key == "border-style" && profile.properties.contains("border-top-style")) ||
                            (key == "border-color" && profile.properties.contains("border-top-color")))) {
                    // A box shorthand of one to four values, top, right, bottom, left, as its longhands: each cascades
                    // with the same side's longhand of the other rules (a class's border-color beats the side colors
                    // a less specific `border: 0 solid` sets).
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
        }

        // Whether a snapshot node has children that are content (:empty does not match it): an element, or text.
        bool has_content(const DomSnapshot& n) {
            for (const auto& c : n.children) {
                if (c.type == constants::element_node_type ||
                    (c.type == constants::text_node_type && !c.data.empty())) {
                    return true;
                }
            }
            return false;
        }

        // A cascade path element: the snapshot root has no parent, and :root is its html element. A sibling of the
        // path's element at `index` stands in for it where sibling combinators and structural pseudo-classes look.
        struct PathElement {
            const std::vector<const DomSnapshot*>* path{};
            std::size_t index{};
            const DomSnapshot* sibling{};

            const DomSnapshot& node() const {
                return sibling ? *sibling : *(*path)[index];
            }

            bool focused() const {
                return node().focused;
            }

            bool focus_visible() const {
                return node().focus_visible;
            }

            bool focus_within() const {
                return node().focus_within;
            }

            bool has_content() const {
                return weft::has_content(node());
            }

            const TextFieldState* field() const {
                return node().field.get();
            }

            // The element sibling before (step -1) or after (step 1) this one; none for the scope root, whose
            // siblings are outside the snapshot.
            std::optional<PathElement> step(int direction) const {
                if (!index) {
                    return std::nullopt;
                }
                const auto& siblings = (*path)[index - 1]->children;
                const DomSnapshot* self = &node();
                std::size_t at = siblings.size();
                for (std::size_t i = 0; i < siblings.size(); ++i) {
                    if (&siblings[i] == self) {
                        at = i;
                        break;
                    }
                }
                if (at == siblings.size()) {
                    return std::nullopt;
                }
                for (auto i = std::ptrdiff_t(at) + direction; i >= 0 && i < std::ptrdiff_t(siblings.size());
                     i += direction) {
                    if (siblings[std::size_t(i)].type == constants::element_node_type) {
                        return PathElement{path, index, &siblings[std::size_t(i)]};
                    }
                }
                return std::nullopt;
            }

            std::optional<PathElement> previous() const {
                return step(-1);
            }

            std::optional<PathElement> next() const {
                return step(1);
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
                return !sibling && index == 0 && node().local_name == "html";
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

            std::optional<LoneElement> previous() const {
                return std::nullopt;
            }

            std::optional<LoneElement> next() const {
                return std::nullopt;
            }

            bool has_content() const {
                return weft::has_content(*n);
            }

            const TextFieldState* field() const {
                return n->field.get();
            }

            bool focused() const {
                return n->focused;
            }

            bool focus_visible() const {
                return n->focus_visible;
            }

            bool focus_within() const {
                return n->focus_within;
            }
        };

        // A compound with :hover or :active as an element alone is matched to it: its positions among siblings and
        // its :not(), :is() and :where(), which the element alone cannot answer, taken to hold.
        dom::Compound lone_state_part(const dom::Compound& part) {
            dom::Compound lone = part;
            lone.nth.clear();
            lone.alternatives.clear();
            return lone;
        }

        // The hover and active of n that a state compound (a lone_state_part) could see; a state no compound can
        // match n by is no input of the cascade.
        std::pair<bool, bool> seen_state(const std::vector<dom::Compound>& parts, const DomSnapshot& n, bool root) {
            std::pair<bool, bool> seen{};
            if (n.hovered || n.active) {
                for (const auto& p : parts) {
                    if (dom::matches(p, LoneElement{&n, root}, false)) {
                        seen.first |= p.hover && n.hovered;
                        seen.second |= p.active && n.active;
                    }
                }
            }
            return seen;
        }

        // A @media condition (Media Queries 4) as a forgiving profile evaluates it: the device is a screen, color, no
        // preference for reduced motion or contrast and a light color scheme; width, height, aspect ratio and
        // orientation are the viewport's, pointer, any-pointer, hover and any-hover the input devices', which the
        // cascade is given.
        struct MediaCondition {
            enum class Kind : std::uint8_t {
                Constant,
                All,
                Any,
                Not,
                Size,
                Device
            };
            enum class Feature : std::uint8_t {
                Width,
                Height,
                Aspect
            };

            Kind kind{};
            bool value{};
            std::vector<MediaCondition> parts;
            Feature feature{};
            // '<', '>', '=', 'l' (<=) or 'g' (>=): the feature's value compared with the amount.
            char op{};
            double amount{};
            // A device feature: its name (pointer, any-pointer, hover, any-hover) and value (empty: boolean).
            std::string device_feature;
            std::string device_value;

            bool holds(float width, float height, const InputDevices& devices = {}) const {
                switch (kind) {
                    case Kind::Constant:
                        return value;
                    case Kind::All:
                        return std::all_of(parts.begin(), parts.end(), [&](const MediaCondition& c) {
                            return c.holds(width, height, devices);
                        });
                    case Kind::Any:
                        return std::any_of(parts.begin(), parts.end(), [&](const MediaCondition& c) {
                            return c.holds(width, height, devices);
                        });
                    case Kind::Not:
                        return !parts[0].holds(width, height, devices);
                    case Kind::Device: {
                        // The primary device's (at most one) or every available one's.
                        const unsigned set = device_feature.starts_with("any-") ? devices.available : devices.primary;
                        if (device_feature.ends_with("hover")) {
                            const bool hovers = set & InputDevices::mouse;
                            return device_value.empty() || device_value == "hover" ? hovers
                                   : device_value == "none"                        ? !hovers
                                                                                   : false;
                        }
                        return device_value.empty()       ? set != 0
                               : device_value == "fine"   ? (set & InputDevices::mouse) != 0
                               : device_value == "coarse" ? (set & InputDevices::touch) != 0
                               : device_value == "none"   ? set == 0
                                                          : false;
                    }
                    case Kind::Size: {
                        const double v = feature == Feature::Width    ? width
                                         : feature == Feature::Height ? height
                                         : height > 0                 ? double(width) / height
                                                                      : 0;
                        switch (op) {
                            case '<':
                                return v < amount;
                            case '>':
                                return v > amount;
                            case 'l':
                                return v <= amount;
                            case 'g':
                                return v >= amount;
                            default:
                                return v == amount;
                        }
                    }
                }
                return false;
            }

            // Whether the outcome depends on the viewport or the input devices.
            bool sized() const {
                return kind == Kind::Size || kind == Kind::Device ||
                       std::any_of(parts.begin(), parts.end(), [](const MediaCondition& c) {
                           return c.sized();
                       });
            }

            static MediaCondition constant(bool v) {
                MediaCondition c;
                c.value = v;
                return c;
            }
        };

        // The pseudo-element a node of a cascade path stands for: none for an element, ::before or ::after for the
        // stand-in Engine::generate computes a generated box's style with (its local name "::before" or "::after").
        dom::PseudoElement generated_kind(const DomSnapshot& n) {
            if (n.local_name.size() < 2 || n.local_name[0] != ':') {
                return dom::PseudoElement::None;
            }
            return n.local_name == "::before"        ? dom::PseudoElement::Before
                   : n.local_name == "::after"       ? dom::PseudoElement::After
                   : n.local_name == "::placeholder" ? dom::PseudoElement::Placeholder
                   : n.local_name == "::selection"   ? dom::PseudoElement::Selection
                                                     : dom::PseudoElement::None;
        }

        // What :read-write and :placeholder-shown read of an element: whether it is a text field the user may edit,
        // and whether its placeholder shows.
        std::pair<bool, bool> field_flags(const DomSnapshot& n) {
            const auto* f = n.field.get();
            return {f && f->editable, f && f->value.empty() && !f->placeholder.empty()};
        }

        struct Rule {
            dom::Selector selector;
            std::vector<Declaration> declarations;
            // The rule's cascade layer (CSS Cascade 5): its rank among the sheet's layers, earlier lower; rules in no
            // layer rank above every layer.
            unsigned layer = std::numeric_limits<unsigned>::max();
            // The @media conditions the rule holds under; none: always.
            std::shared_ptr<const MediaCondition> media;

            bool matches(const std::vector<const DomSnapshot*>& path) const {
                // A generated box (Engine::generate) matches the rules whose subject is that pseudo-element of its
                // element, the element below it on the path; an element matches no such rule.
                if (const auto kind = generated_kind(*path.back()); kind != dom::PseudoElement::None) {
                    return path.size() >= 2 && selector.pseudo() == kind &&
                           dom::matches_originating(selector, PathElement{&path, path.size() - 2});
                }
                return dom::matches(selector, PathElement{&path, path.size() - 1});
            }
        };

        // A registered custom property (@property, CSS Properties and Values API 1): whether it inherits, and its
        // initial value (none: the guaranteed-invalid value).
        struct Registered {
            bool inherits = true;
            std::optional<Tokens> initial;
        };

        // A sheet as a forgiving profile parses it.
        struct Sheet {
            std::vector<Rule> rules;
            std::string keyframes;
            std::vector<FontFaceRule> font_faces;
            std::map<std::string, Registered> registered;
            // Some rule holds under a @media condition on the viewport.
            bool sized_media{};
            Warnings warnings;
        };

        // The index of [begin, end) of a top-level token: skips a string or a balanced (), [] or {} group.
        std::size_t skip_group(std::string_view text, std::size_t p) {
            const char open = text[p];
            if (open == '"' || open == '\'') {
                for (++p; p < text.size(); ++p) {
                    if (text[p] == '\\') {
                        ++p;
                    } else if (text[p] == open) {
                        return p + 1;
                    }
                }
                return std::string_view::npos;
            }
            const char close = open == '(' ? ')' : open == '[' ? ']' : '}';
            int depth = 1;
            char quote{};
            for (++p; p < text.size(); ++p) {
                const char c = text[p];
                if (quote) {
                    if (c == '\\') {
                        ++p;
                    } else if (c == quote) {
                        quote = 0;
                    }
                } else if (c == '"' || c == '\'') {
                    quote = c;
                } else if (c == '\\') {
                    ++p;
                } else if (c == open) {
                    ++depth;
                } else if (c == close && --depth == 0) {
                    return p + 1;
                }
            }
            return std::string_view::npos;
        }

        // Where a prelude ends: the first '{' or ';' outside strings and groups (npos: neither).
        std::size_t prelude_end(std::string_view text, std::size_t p) {
            while (p < text.size()) {
                const char c = text[p];
                if (c == '{' || c == ';') {
                    return p;
                }
                if (c == '"' || c == '\'' || c == '(' || c == '[') {
                    p = skip_group(text, p);
                    if (p == std::string_view::npos) {
                        return p;
                    }
                    continue;
                }
                if (c == '\\') {
                    ++p;
                }
                ++p;
            }
            return std::string_view::npos;
        }

        // The words and parenthesized groups of a condition (@media, @supports), lowercase words, groups as written.
        std::vector<std::string> condition_parts(std::string_view text) {
            std::vector<std::string> out;
            std::size_t p{};
            while (p < text.size()) {
                const char c = text[p];
                if (std::isspace(static_cast<unsigned char>(c))) {
                    ++p;
                } else if (c == '(') {
                    const auto end = skip_group(text, p);
                    if (end == std::string_view::npos) {
                        fail("unclosed condition group");
                    }
                    out.emplace_back(text.substr(p, end - p));
                    p = end;
                } else if (c == ',') {
                    out.emplace_back(",");
                    ++p;
                } else {
                    const auto start = p;
                    while (p < text.size() && !std::isspace(static_cast<unsigned char>(text[p])) && text[p] != '(' &&
                           text[p] != ',') {
                        ++p;
                    }
                    if (p < text.size() && text[p] == '(') {
                        // A function (selector(), font-tech()...): the name with its group.
                        const auto end = skip_group(text, p);
                        if (end == std::string_view::npos) {
                            fail("unclosed condition function");
                        }
                        p = end;
                    }
                    out.push_back(lower(std::string(text.substr(start, p - start))));
                }
            }
            return out;
        }

        // A length of a media feature in CSS px (px, em and rem of the default 16px font size), or a plain number.
        std::optional<double> media_amount(std::string_view text) {
            const auto t = lower(trim(text));
            if (const auto slash = t.find('/'); slash != std::string::npos) {
                const auto a = css::parse_number<double>(trim(t.substr(0, slash)));
                const auto b = css::parse_number<double>(trim(t.substr(slash + 1)));
                return a && b && *b != 0 ? std::optional(*a / *b) : std::nullopt;
            }
            for (const auto& [unit, scale] : {std::pair<const char*, double>{"rem", constants::default_font_size_px},
                                              {"em", constants::default_font_size_px},
                                              {"px", 1.0}}) {
                if (t.ends_with(unit)) {
                    const auto n = css::parse_number<double>(t.substr(0, t.size() - std::string_view(unit).size()));
                    return n ? std::optional(*n * scale) : std::nullopt;
                }
            }
            const auto n = css::parse_number<double>(t);
            return n ? std::optional(*n) : std::nullopt;
        }

        // A media feature in its parentheses' inside: plain (name: value), boolean (name) or a range (width >= 40rem,
        // 400px < width < 700px).
        MediaCondition media_feature(std::string_view inside, Warnings& warnings) {
            using M = MediaCondition;
            const auto text = trim(inside);
            const auto sized = [](std::string_view name) -> std::optional<M::Feature> {
                if (name == "width" || name == "device-width") {
                    return M::Feature::Width;
                }
                if (name == "height" || name == "device-height") {
                    return M::Feature::Height;
                }
                if (name == "aspect-ratio" || name == "device-aspect-ratio") {
                    return M::Feature::Aspect;
                }
                return std::nullopt;
            };
            const auto size = [](M::Feature feature, char op, double amount) {
                M c;
                c.kind = M::Kind::Size;
                c.feature = feature;
                c.op = op;
                c.amount = amount;
                return c;
            };
            // Range syntax: split at the comparison operators.
            const auto op_at = text.find_first_of("<>=");
            if (op_at != std::string::npos && text.find(':') == std::string::npos) {
                std::vector<std::string> pieces;
                std::vector<char> ops;
                std::size_t start{};
                for (std::size_t i = 0; i < text.size(); ++i) {
                    if (text[i] == '<' || text[i] == '>' || text[i] == '=') {
                        pieces.push_back(lower(trim(text.substr(start, i - start))));
                        char op = text[i];
                        if (i + 1 < text.size() && text[i + 1] == '=' && op != '=') {
                            op = op == '<' ? 'l' : 'g';
                            ++i;
                        }
                        ops.push_back(op);
                        start = i + 1;
                    }
                }
                pieces.push_back(lower(trim(text.substr(start))));
                const auto flip = [](char op) {
                    return op == '<' ? '>' : op == '>' ? '<' : op == 'l' ? 'g' : op == 'g' ? 'l' : op;
                };
                if (pieces.size() == 2) {
                    if (const auto f = sized(pieces[0])) {
                        if (const auto a = media_amount(pieces[1])) {
                            return size(*f, ops[0], *a);
                        }
                    } else if (const auto f2 = sized(pieces[1])) {
                        if (const auto a = media_amount(pieces[0])) {
                            return size(*f2, flip(ops[0]), *a);
                        }
                    }
                } else if (pieces.size() == 3) {
                    if (const auto f = sized(pieces[1])) {
                        const auto a = media_amount(pieces[0]);
                        const auto b = media_amount(pieces[2]);
                        if (a && b) {
                            M all;
                            all.kind = M::Kind::All;
                            all.parts.push_back(size(*f, flip(ops[0]), *a));
                            all.parts.push_back(size(*f, ops[1], *b));
                            return all;
                        }
                    }
                }
                warnings.add("@media: a range outside the profile (" + excerpt(text) + ")");
                return M::constant(false);
            }
            const auto colon = text.find(':');
            const auto name = lower(trim(text.substr(0, colon)));
            const auto value = colon == std::string::npos ? std::string() : lower(trim(text.substr(colon + 1)));
            for (const auto& [prefix, op] : {std::pair<const char*, char>{"min-", 'g'}, {"max-", 'l'}}) {
                if (name.starts_with(prefix)) {
                    if (const auto f = sized(name.substr(std::string_view(prefix).size()))) {
                        if (const auto a = media_amount(value)) {
                            return size(*f, op, *a);
                        }
                    }
                }
            }
            if (const auto f = sized(name); f && colon != std::string::npos) {
                if (const auto a = media_amount(value)) {
                    return size(*f, '=', *a);
                }
            }
            if (name == "orientation") {
                // portrait: height >= width.
                M c;
                c.kind = M::Kind::Size;
                c.feature = M::Feature::Aspect;
                c.op = value == "portrait" ? 'l' : '>';
                c.amount = 1;
                return c;
            }
            // The input devices: answered as the cascade is given them.
            if (name == "hover" || name == "any-hover" || name == "pointer" || name == "any-pointer") {
                M c;
                c.kind = M::Kind::Device;
                c.device_feature = name;
                c.device_value = value;
                return c;
            }
            // The fixed device: what holds.
            static const std::map<std::string, std::vector<std::string>> device{
                {"prefers-reduced-motion", {"no-preference"}},
                {"prefers-reduced-transparency", {"no-preference"}},
                {"prefers-reduced-data", {"no-preference"}},
                {"prefers-contrast", {"no-preference"}},
                {"prefers-color-scheme", {"light"}},
                {"forced-colors", {"none"}},
                {"inverted-colors", {"none"}},
                {"color", {""}},
                {"color-gamut", {"srgb"}},
                {"dynamic-range", {"standard"}},
                {"scripting", {"enabled", ""}},
                {"update", {"fast", ""}},
                {"display-mode", {"browser"}},
                {"grid", {"0"}},
                {"monochrome", {"0"}},
            };
            if (const auto it = device.find(name); it != device.end()) {
                const auto& holds = it->second;
                return M::constant(std::find(holds.begin(), holds.end(), value) != holds.end());
            }
            warnings.add("@media: the feature " + name + " is outside the profile (taken as false)");
            return M::constant(false);
        }

        MediaCondition media_condition(const std::vector<std::string>& parts, std::size_t begin, std::size_t end,
                                       Warnings& warnings);

        // A <media-in-parens>: a nested condition or a feature.
        MediaCondition media_in_parens(const std::string& group, Warnings& warnings) {
            const auto inside = std::string_view(group).substr(1, group.size() - 2);
            const auto t = trim(inside);
            if (!t.empty() && (t[0] == '(' || lower(t).starts_with("not ") || lower(t).starts_with("not("))) {
                const auto parts = condition_parts(t);
                return media_condition(parts, 0, parts.size(), warnings);
            }
            if (lower(t).find(" and ") != std::string::npos || lower(t).find(" or ") != std::string::npos) {
                const auto parts = condition_parts(t);
                return media_condition(parts, 0, parts.size(), warnings);
            }
            return media_feature(inside, warnings);
        }

        // not X | X [and X]* | X [or X]* over parts [begin, end).
        MediaCondition media_condition(const std::vector<std::string>& parts, std::size_t begin, std::size_t end,
                                       Warnings& warnings) {
            using M = MediaCondition;
            if (begin >= end) {
                return M::constant(false);
            }
            if (parts[begin] == "not") {
                M c;
                c.kind = M::Kind::Not;
                c.parts.push_back(media_condition(parts, begin + 1, end, warnings));
                return c;
            }
            M combined;
            combined.kind = M::Kind::All;
            for (std::size_t i = begin; i < end; ++i) {
                const auto& part = parts[i];
                if (part == "and") {
                    continue;
                }
                if (part == "or") {
                    combined.kind = M::Kind::Any;
                    continue;
                }
                if (part.starts_with("(")) {
                    combined.parts.push_back(media_in_parens(part, warnings));
                } else {
                    warnings.add("@media: " + part + " is outside the profile (taken as false)");
                    combined.parts.push_back(M::constant(false));
                }
            }
            return combined.parts.size() == 1 ? combined.parts[0] : combined;
        }

        // A media query list: queries separated by commas, each [not | only]? <type> [and <condition>]? or a
        // condition.
        MediaCondition media_query_list(std::string_view text, Warnings& warnings) {
            using M = MediaCondition;
            const auto parts = condition_parts(text);
            M any;
            any.kind = M::Kind::Any;
            std::size_t begin{};
            for (std::size_t i = 0; i <= parts.size(); ++i) {
                if (i < parts.size() && parts[i] != ",") {
                    continue;
                }
                std::size_t b = begin;
                bool negate{};
                if (b < i && (parts[b] == "only")) {
                    ++b;
                }
                if (b < i && parts[b] == "not" && b + 1 < i && !parts[b + 1].starts_with("(")) {
                    negate = true;
                    ++b;
                }
                M query;
                if (b < i && !parts[b].starts_with("(") && parts[b] != "not") {
                    const auto& type = parts[b];
                    const bool screen = type == "all" || type == "screen";
                    if (!screen && type != "print" && type != "speech" && type != "tv" && type != "projection" &&
                        type != "handheld" && type != "braille" && type != "embossed" && type != "aural" &&
                        type != "tty") {
                        warnings.add("@media: the media type " + type + " is unknown (taken as false)");
                    }
                    query = M::constant(screen);
                    if (b + 1 < i && parts[b + 1] == "and") {
                        M all;
                        all.kind = M::Kind::All;
                        all.parts.push_back(query);
                        all.parts.push_back(media_condition(parts, b + 2, i, warnings));
                        query = all;
                    }
                } else {
                    query = media_condition(parts, b, i, warnings);
                }
                if (negate) {
                    M n;
                    n.kind = M::Kind::Not;
                    n.parts.push_back(query);
                    query = n;
                }
                any.parts.push_back(std::move(query));
                begin = i + 1;
            }
            if (any.parts.empty()) {
                return M::constant(true);
            }
            return any.parts.size() == 1 ? any.parts[0] : any;
        }

        // The cascade layers of a sheet: a tree of names whose ranks follow CSS Cascade 5 (a layer's sublayers in
        // the order they first appear, then the layer itself; rules in no layer last).
        struct LayerTree {
            struct Node {
                std::string name;
                std::vector<std::size_t> children;
                unsigned rank{};
            };

            std::vector<Node> nodes{Node{}};
            unsigned anonymous{};

            std::size_t child(std::size_t parent, const std::string& name) {
                for (const auto c : nodes[parent].children) {
                    if (nodes[c].name == name) {
                        return c;
                    }
                }
                if (nodes.size() >= std::size_t(constants::style::max_forgiving_layers)) {
                    fail("cascade layer bound");
                }
                nodes.push_back({name, {}, 0});
                nodes[parent].children.push_back(nodes.size() - 1);
                return nodes.size() - 1;
            }

            // A dotted name (a.b) under parent; empty: a new anonymous layer.
            std::size_t path(std::size_t parent, std::string_view dotted) {
                if (trim(dotted).empty()) {
                    return child(parent, "\x01" + std::to_string(++anonymous));
                }
                std::size_t at = parent;
                for (const auto& part : css::split(trim(dotted), '.').parts) {
                    at = child(at, trim(part));
                }
                return at;
            }

            void rank() {
                unsigned next{};
                const auto visit = [&](auto&& self, std::size_t n) -> void {
                    for (const auto c : nodes[n].children) {
                        self(self, c);
                    }
                    nodes[n].rank = n == 0 ? std::numeric_limits<unsigned>::max() : next++;
                };
                visit(visit, 0);
            }
        };

        bool supports(std::string_view condition, const StyleProfile& profile);

        // A forgiving profile's sheet, as CSS Syntax 3 parses a style sheet: qualified rules and at-rules, nested as
        // CSS Nesting nests them; what it cannot take is dropped with a warning.
        struct SheetParser {
            const StyleProfile& profile;
            Sheet& out;
            LayerTree layers;
            std::vector<std::size_t> rule_layers;
            const dom::SelectorGrammar grammar{true, true, Diag::StyleProfile, Diag::StyleProfile, true};

            void warn(std::string text) {
                out.warnings.add(std::move(text));
            }

            static std::shared_ptr<const MediaCondition> both(const std::shared_ptr<const MediaCondition>& outer,
                                                              MediaCondition inner) {
                if (!outer) {
                    return std::make_shared<const MediaCondition>(std::move(inner));
                }
                MediaCondition all;
                all.kind = MediaCondition::Kind::All;
                all.parts.push_back(*outer);
                all.parts.push_back(std::move(inner));
                return std::make_shared<const MediaCondition>(std::move(all));
            }

            // A nested rule's selector list against its parent's (CSS Nesting 1): & is the parent, a relative
            // selector or one without & is a descendant of it.
            static std::string nest(std::string_view parent, std::string_view child) {
                const auto items = css::split(child, ',', {.quotes = true, .brackets = true, .trim_parts = true}).parts;
                const std::string is = ":is(" + std::string(parent) + ")";
                std::string out;
                for (const auto& raw : items) {
                    auto item = trim(raw);
                    std::string resolved;
                    if (item.find('&') != std::string::npos) {
                        for (const char c : item) {
                            resolved += c == '&' ? is : std::string(1, c);
                        }
                    } else {
                        resolved = is + " " + item;
                    }
                    out += (out.empty() ? "" : ", ") + resolved;
                }
                return out;
            }

            void block(std::string_view text, std::size_t layer, const std::shared_ptr<const MediaCondition>& media,
                       unsigned depth, const std::string* parent) {
                if (depth > unsigned(constants::style::max_at_rule_depth)) {
                    warn("rules nested deeper than the profile takes were dropped");
                    return;
                }
                std::size_t p{};
                // The declarations directly in a nested at-rule's block belong to the parent rule.
                std::string declarations_text;
                while (p < text.size()) {
                    const char c = text[p];
                    if (std::isspace(static_cast<unsigned char>(c)) || c == ';') {
                        ++p;
                        continue;
                    }
                    if (c == '}') {
                        warn("a stray } in the style sheet");
                        ++p;
                        continue;
                    }
                    const auto end = prelude_end(text, p);
                    if (end == std::string_view::npos) {
                        if (parent) {
                            declarations_text += std::string(text.substr(p)) + ";";
                        } else {
                            warn("a rule without a block (" + excerpt(text.substr(p)) + ")");
                        }
                        break;
                    }
                    if (text[end] == ';') {
                        if (c == '@') {
                            statement(text.substr(p + 1, end - p - 1), layer);
                        } else if (parent) {
                            declarations_text += std::string(text.substr(p, end - p)) + ";";
                        } else {
                            warn("a declaration outside a rule (" + excerpt(text.substr(p, end - p)) + ")");
                        }
                        p = end + 1;
                        continue;
                    }
                    const auto close = skip_group(text, end);
                    if (close == std::string_view::npos) {
                        warn("an unclosed block (" + excerpt(text.substr(p, end - p)) + ")");
                        break;
                    }
                    const auto prelude = text.substr(p, end - p);
                    const auto body = text.substr(end + 1, close - end - 2);
                    if (c == '@') {
                        at_rule(prelude.substr(1), body, layer, media, depth, parent);
                    } else {
                        style_rule(parent ? nest(*parent, prelude) : std::string(trim(prelude)), body, layer, media,
                                   depth);
                    }
                    p = close;
                }
                if (parent && !trim(declarations_text).empty()) {
                    add_rules(*parent, declarations_text, layer, media);
                }
            }

            void add_rules(const std::string& selectors, const std::string& body, std::size_t layer,
                           const std::shared_ptr<const MediaCondition>& media) {
                std::vector<dom::Selector> list;
                for (const auto& item :
                     css::split(selectors, ',', {.quotes = true, .brackets = true, .trim_parts = true}).parts) {
                    auto s = dom::parse_selector(trim(item), grammar);
                    if (!s) {
                        // An invalid selector invalidates its rule (Selectors 4 §3.1).
                        warn("a rule dropped for its selector " + excerpt(item) + ": " + s.error().message);
                        return;
                    }
                    list.push_back(std::move(*s));
                }
                auto decl = declarations(body, profile, &out.warnings);
                if (decl.empty()) {
                    return;
                }
                for (auto& s : list) {
                    if (s.pseudo() == dom::PseudoElement::Other) {
                        continue;
                    }
                    if (out.rules.size() >= std::size_t(constants::style::max_forgiving_rules)) {
                        fail("rule bound");
                    }
                    out.rules.push_back({std::move(s), decl});
                    out.rules.back().media = media;
                    rule_layers.push_back(layer);
                }
            }

            void style_rule(const std::string& selectors, std::string_view body, std::size_t layer,
                            const std::shared_ptr<const MediaCondition>& media, unsigned depth) {
                // Declarations first; nested rules and at-rules (CSS Nesting) after, as rules of their own.
                std::string declarations_text;
                std::vector<std::pair<std::string_view, std::string_view>> nested;
                std::size_t p{};
                while (p < body.size()) {
                    const char c = body[p];
                    if (std::isspace(static_cast<unsigned char>(c)) || c == ';') {
                        ++p;
                        continue;
                    }
                    const auto end = prelude_end(body, p);
                    if (end == std::string_view::npos) {
                        declarations_text += std::string(body.substr(p));
                        break;
                    }
                    if (body[end] == ';') {
                        declarations_text += std::string(body.substr(p, end - p + 1));
                        p = end + 1;
                        continue;
                    }
                    const auto close = skip_group(body, end);
                    if (close == std::string_view::npos) {
                        warn("an unclosed nested block (" + excerpt(body.substr(p, end - p)) + ")");
                        break;
                    }
                    nested.emplace_back(body.substr(p, end - p), body.substr(end + 1, close - end - 2));
                    p = close;
                }
                add_rules(selectors, declarations_text, layer, media);
                for (const auto& [prelude, inner] : nested) {
                    if (!prelude.empty() && prelude[0] == '@') {
                        at_rule(prelude.substr(1), inner, layer, media, depth + 1, &selectors);
                    } else {
                        style_rule(nest(selectors, prelude), inner, layer, media, depth + 1);
                    }
                }
            }

            // An at-rule ending in ';'.
            void statement(std::string_view text, std::size_t layer) {
                std::size_t n{};
                while (n < text.size() && (ident_char(text[n]))) {
                    ++n;
                }
                const auto name = lower(std::string(text.substr(0, n)));
                const auto prelude = trim(text.substr(n));
                if (name == "layer") {
                    for (const auto& item : css::split(prelude, ',').parts) {
                        layers.path(layer, trim(item));
                    }
                } else if (name != "charset") {
                    warn("@" + name + " is outside the profile (one sheet a page)");
                }
            }

            void at_rule(std::string_view text, std::string_view body, std::size_t layer,
                         const std::shared_ptr<const MediaCondition>& media, unsigned depth,
                         const std::string* parent) {
                std::size_t n{};
                while (n < text.size() && ident_char(text[n])) {
                    ++n;
                }
                const auto name = lower(std::string(text.substr(0, n)));
                const auto prelude = trim(text.substr(n));
                if (name == "layer") {
                    block(body, layers.path(layer, prelude), media, depth + 1, parent);
                } else if (name == "media") {
                    auto condition = media_query_list(prelude, out.warnings);
                    if (condition.kind == MediaCondition::Kind::Constant && !condition.value) {
                        return;
                    }
                    const bool sized = condition.sized();
                    out.sized_media |= sized;
                    const auto inner =
                        condition.kind == MediaCondition::Kind::Constant ? media : both(media, std::move(condition));
                    block(body, layer, inner, depth + 1, parent);
                } else if (name == "supports") {
                    bool holds{};
                    try {
                        holds = supports(prelude, profile);
                    } catch (const std::invalid_argument&) {
                        holds = false;
                    }
                    if (holds) {
                        block(body, layer, media, depth + 1, parent);
                    }
                } else if (name == "keyframes") {
                    out.keyframes += "@keyframes " + prelude + "{" + std::string(body) + "}\n";
                } else if (name == "-webkit-keyframes" || name == "-moz-keyframes") {
                    // The unprefixed rule says the same.
                } else if (name == "property") {
                    property(prelude, body);
                } else if (name == "font-face") {
                    font_face(body);
                } else if (name == "container" || name == "scope" || name == "starting-style") {
                    warn("@" + name + " is outside the profile; its rules were dropped");
                } else {
                    warn("@" + name + " is outside the profile");
                }
            }

            // @font-face { font-family: ...; src: ...; font-weight: ...; font-style: ...; unicode-range: ... }: a family
            // the page defines (its sources load with the page, View::load). font-display changes nothing (the page's
            // fonts are there before its first frame); other descriptors are dropped with a warning, and a rule without
            // a family or a source is dropped.
            void font_face(std::string_view body) {
                FontFaceRule rule;
                const auto unquoted = [](std::string_view raw) {
                    auto v = trim(raw);
                    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) {
                        v = v.substr(1, v.size() - 2);
                    }
                    return v;
                };
                // A function's argument: url(x), url("x"), format("truetype"), local(Name).
                const auto argument = [&](std::string_view raw, std::string_view name) -> std::optional<std::string> {
                    const auto call = trim(raw);
                    if (call.size() < name.size() + 2 || lower(call.substr(0, name.size())) != name ||
                        call[name.size()] != '(' || call.back() != ')') {
                        return std::nullopt;
                    }
                    return unquoted(std::string_view(call).substr(name.size() + 1, call.size() - name.size() - 2));
                };
                for (const auto& part : css::split(body, ';', {.quotes = true, .brackets = true}).parts) {
                    const auto colon = part.find(':');
                    if (colon == std::string::npos) {
                        continue;
                    }
                    const auto key = lower(trim(part.substr(0, colon)));
                    const auto value = trim(part.substr(colon + 1));
                    if (key == "font-family") {
                        // One family: a string, or identifiers with single spaces between them.
                        if (value.find(',') != std::string::npos) {
                            warn("@font-face font-family " + excerpt(value) + ": one family");
                            return;
                        }
                        std::string family;
                        for (const auto& word : css::split(unquoted(value), ' ').parts) {
                            family += (family.empty() ? "" : " ") + std::string(trim(word));
                        }
                        rule.family = std::move(family);
                    } else if (key == "src") {
                        for (const auto& item : css::split(value, ',', {.quotes = true}).parts) {
                            FontFaceRule::Source source;
                            auto words = css::split(item, ' ', {.quotes = true}).parts;
                            if (words.empty()) {
                                continue;
                            }
                            if (auto local = argument(words[0], "local")) {
                                source.local = std::move(*local);
                            } else if (auto url = argument(words[0], "url")) {
                                source.url = std::move(*url);
                                for (std::size_t k = 1; k < words.size(); ++k) {
                                    if (auto format = argument(words[k], "format")) {
                                        for (const auto& f : css::split(*format, ',', {.quotes = true}).parts) {
                                            source.formats.push_back(lower(unquoted(f)));
                                        }
                                    } else if (!argument(words[k], "tech")) {
                                        warn("@font-face src " + excerpt(item) + ": not a source");
                                    }
                                }
                            } else {
                                warn("@font-face src " + excerpt(item) + ": not a source");
                                continue;
                            }
                            rule.sources.push_back(std::move(source));
                        }
                    } else if (key == "font-weight") {
                        const auto v = lower(std::string(value));
                        if (v == "normal") {
                            rule.weight = 400;
                        } else if (v == "bold") {
                            rule.weight = 700;
                        } else if (const auto n = css::parse_number<double>(v); n && *n >= 1 && *n <= 1000) {
                            rule.weight = int(std::lround(*n));
                        } else {
                            warn("@font-face font-weight " + excerpt(value) + " is outside the profile (one weight)");
                        }
                    } else if (key == "font-style") {
                        const auto v = lower(std::string(value));
                        if (v == "italic" || v == "oblique") {
                            rule.italic = true;
                        } else if (v != "normal") {
                            warn("@font-face font-style " + excerpt(value) + " is outside the profile");
                        }
                    } else if (key == "unicode-range") {
                        rule.unicode_range = value;
                    } else if (key != "font-display" &&
                               !(key == "font-stretch" && lower(std::string(value)) == "normal")) {
                        warn("@font-face " + key + " is outside the profile");
                    }
                }
                if (rule.family.empty() || rule.sources.empty()) {
                    warn("@font-face without a font-family and a src was dropped");
                    return;
                }
                out.font_faces.push_back(std::move(rule));
            }

            // @property --name { syntax: ...; inherits: ...; initial-value: ... }
            void property(std::string_view prelude, std::string_view body) {
                const auto name = trim(prelude);
                if (!custom(name)) {
                    warn("@property " + excerpt(name) + ": not a custom property name");
                    return;
                }
                Registered r;
                std::optional<bool> inherits;
                std::string syntax;
                for (const auto& part : css::split(body, ';', {.quotes = true, .brackets = true}).parts) {
                    const auto colon = part.find(':');
                    if (colon == std::string::npos) {
                        continue;
                    }
                    const auto key = lower(trim(part.substr(0, colon)));
                    const auto value = trim(part.substr(colon + 1));
                    if (key == "inherits") {
                        inherits = lower(value) == "true";
                    } else if (key == "syntax") {
                        syntax = value;
                    } else if (key == "initial-value") {
                        Lexer lexer{value};
                        lexer.forgiving = true;
                        r.initial = lexer.scan();
                    }
                }
                if (!inherits || syntax.empty()) {
                    warn("@property " + name + " without syntax and inherits was dropped");
                    return;
                }
                r.inherits = *inherits;
                if (out.registered.size() >= std::size_t(constants::style::max_registered_custom_properties)) {
                    warn("@property " + name + ": more registered properties than the profile takes");
                    return;
                }
                out.registered[name] = std::move(r);
            }
        };

        Sheet parse_sheet(std::string text, const StyleProfile& profile) {
            if (text.size() > std::size_t(constants::style::max_forgiving_stylesheet_bytes)) {
                fail("stylesheet byte bound");
            }
            Sheet sheet;
            text = comments(text, true);
            SheetParser parser{profile, sheet};
            parser.block(text, 0, nullptr, 0, nullptr);
            parser.layers.rank();
            for (std::size_t i = 0; i < sheet.rules.size(); ++i) {
                sheet.rules[i].layer = parser.layers.nodes[parser.rule_layers[i]].rank;
            }
            return sheet;
        }

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
                // An at-rule without nested rules (@font-face, @page) is no selector either: at-rules are a forgiving
                // sheet's.
                if (const auto prelude = trim(text.substr(p, b - p)); !prelude.empty() && prelude[0] == '@') {
                    fail("at-rule/nested rule outside profile");
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

        // ---- Colors and math functions as a forgiving profile computes them (CSS Color 4 and 5, CSS Values 4). ----

        // Properties whose values hold <color>s.
        bool color_bearing(std::string_view key) {
            return key == "color" || key.ends_with("-color") || key == "background" || key == "background-image" ||
                   key == "box-shadow" || key == "text-shadow" || key == "fill" || key == "stroke" || key == "filter" ||
                   key == "border-image-source" || key == "outline" || key == "text-decoration";
        }

        // A number of a token: a plain number, or a percentage of `percent` (the value 100% stands for).
        std::optional<double> amount(const Token& t, double percent) {
            if (t.kind == Kind::Number) {
                const auto n = css::parse_number<double>(t.text);
                return n ? std::optional(*n) : std::nullopt;
            }
            if (t.kind == Kind::Percentage) {
                const auto n = css::parse_number<double>(t.text.substr(0, t.text.size() - 1));
                return n ? std::optional(*n / 100 * percent) : std::nullopt;
            }
            if (t.kind == Kind::Ident && lower(t.text) == "none") {
                return 0.0;
            }
            return std::nullopt;
        }

        // A <hue> in degrees: a number or an angle.
        std::optional<double> hue_amount(const Token& t) {
            if (t.kind == Kind::Number || (t.kind == Kind::Ident && lower(t.text) == "none")) {
                return amount(t, 1);
            }
            if (t.kind != Kind::Dimension) {
                return std::nullopt;
            }
            const auto text = lower(t.text);
            for (const auto& [unit, scale] : {std::pair<const char*, double>{"deg", 1.0},
                                              {"grad", 0.9},
                                              {"rad", 180 / 3.14159265358979323846},
                                              {"turn", 360.0}}) {
                if (text.ends_with(unit)) {
                    const auto n =
                        css::parse_number<double>(text.substr(0, text.size() - std::string_view(unit).size()));
                    return n ? std::optional(*n * scale) : std::nullopt;
                }
            }
            return std::nullopt;
        }

        // A color function's components and alpha: legacy (separated by commas) or modern (by spaces, / alpha).
        struct ColorArguments {
            std::vector<Token> components;
            std::optional<Token> alpha;
            bool legacy{};
        };

        std::optional<ColorArguments> color_arguments(const Tokens& args) {
            ColorArguments out;
            out.legacy = std::any_of(args.begin(), args.end(), [](const Token& t) {
                return t.kind == Kind::Delim && t.text == ",";
            });
            bool slash{};
            for (std::size_t i = 0; i < args.size(); ++i) {
                const auto& t = args[i];
                if (t.kind == Kind::Delim && t.text == ",") {
                    if (!out.legacy || i == 0 || i + 1 == args.size()) {
                        return std::nullopt;
                    }
                    continue;
                }
                if (t.kind == Kind::Delim && t.text == "/") {
                    if (out.legacy || slash) {
                        return std::nullopt;
                    }
                    slash = true;
                    continue;
                }
                if (slash) {
                    if (out.alpha) {
                        return std::nullopt;
                    }
                    out.alpha = t;
                } else {
                    out.components.push_back(t);
                }
            }
            if (out.legacy && out.components.size() == 4) {
                out.alpha = out.components.back();
                out.components.pop_back();
            }
            if (out.components.size() != 3) {
                return std::nullopt;
            }
            return out;
        }

        std::optional<color::Rgba> color_value(const Token& t, const std::optional<color::Rgba>& current);

        // A color function (rgb(), hsl(), hwb(), lab(), lch(), oklab(), oklch(), color()) or color-mix().
        std::optional<color::Rgba> color_function(const Token& t, const std::optional<color::Rgba>& current) {
            const auto name = lower(t.text);
            if (name == "color-mix") {
                // color-mix(in <space> [<hue> hue]?, <color> <percentage>?, <color> <percentage>?)
                std::vector<Tokens> items(1);
                for (const auto& a : t.args) {
                    if (a.kind == Kind::Delim && a.text == ",") {
                        items.emplace_back();
                    } else {
                        items.back().push_back(a);
                    }
                }
                if (items.size() != 3 || items[0].size() < 2 || items[0][0].kind != Kind::Ident ||
                    lower(items[0][0].text) != "in" || items[0][1].kind != Kind::Ident) {
                    return std::nullopt;
                }
                const auto space = color::space(lower(items[0][1].text));
                if (!space) {
                    return std::nullopt;
                }
                auto hue = color::HueInterpolation::Shorter;
                if (items[0].size() == 4 && lower(items[0][3].text) == "hue") {
                    const auto h = lower(items[0][2].text);
                    hue = h == "longer"       ? color::HueInterpolation::Longer
                          : h == "increasing" ? color::HueInterpolation::Increasing
                          : h == "decreasing" ? color::HueInterpolation::Decreasing
                                              : color::HueInterpolation::Shorter;
                } else if (items[0].size() != 2) {
                    return std::nullopt;
                }
                std::array<color::Rgba, 2> colors{};
                std::array<std::optional<double>, 2> weights;
                for (std::size_t i = 0; i < 2; ++i) {
                    const auto& item = items[i + 1];
                    std::optional<color::Rgba> c;
                    for (const auto& part : item) {
                        if (part.kind == Kind::Percentage && !weights[i]) {
                            weights[i] = amount(part, 1);
                        } else if (!c) {
                            c = color_value(part, current);
                            if (!c) {
                                return std::nullopt;
                            }
                        } else {
                            return std::nullopt;
                        }
                    }
                    if (!c) {
                        return std::nullopt;
                    }
                    colors[i] = *c;
                }
                double a = weights[0].value_or(weights[1] ? 1 - *weights[1] : 0.5);
                double b = weights[1].value_or(weights[0] ? 1 - *weights[0] : 0.5);
                if (a < 0 || b < 0 || a > 1 || b > 1) {
                    return std::nullopt;
                }
                const double sum = a + b;
                if (sum <= 0) {
                    return std::nullopt;
                }
                auto mixed = color::mix(*space, hue, colors[0], a / sum, colors[1], b / sum);
                if (sum < 1) {
                    mixed[3] *= sum;
                }
                return mixed;
            }
            if (std::any_of(t.args.begin(), t.args.end(), [](const Token& a) {
                    return a.kind == Kind::Ident && lower(a.text) == "from";
                })) {
                // Relative color syntax is outside the profile.
                return std::nullopt;
            }
            if (name == "color") {
                if (t.args.empty() || t.args[0].kind != Kind::Ident) {
                    return std::nullopt;
                }
                const auto space = color::space(lower(t.args[0].text));
                if (!space || color::polar(*space) || *space == color::Space::Lab || *space == color::Space::Oklab) {
                    return std::nullopt;
                }
                const auto args = color_arguments(Tokens(t.args.begin() + 1, t.args.end()));
                if (!args || args->legacy) {
                    return std::nullopt;
                }
                std::array<double, 3> c{};
                for (std::size_t i = 0; i < 3; ++i) {
                    const auto v = amount(args->components[i], 1);
                    if (!v) {
                        return std::nullopt;
                    }
                    c[i] = *v;
                }
                const auto alpha = args->alpha ? amount(*args->alpha, 1) : std::optional(1.0);
                if (!alpha) {
                    return std::nullopt;
                }
                const auto rgb = color::to_srgb(*space, c);
                return color::Rgba{rgb[0], rgb[1], rgb[2], std::clamp(*alpha, 0.0, 1.0)};
            }
            const auto args = color_arguments(t.args);
            if (!args) {
                return std::nullopt;
            }
            const auto alpha = args->alpha ? amount(*args->alpha, 1) : std::optional(1.0);
            if (!alpha) {
                return std::nullopt;
            }
            const auto& c = args->components;
            std::array<double, 3> v{};
            std::optional<color::Space> space;
            if (name == "rgb" || name == "rgba") {
                for (std::size_t i = 0; i < 3; ++i) {
                    const auto x = amount(c[i], 255);
                    if (!x) {
                        return std::nullopt;
                    }
                    v[i] = *x / 255;
                }
                space = color::Space::Srgb;
            } else if (name == "hsl" || name == "hsla" || name == "hwb") {
                const auto h = hue_amount(c[0]);
                const auto x = amount(c[1], 100);
                const auto y = amount(c[2], 100);
                if (!h || !x || !y) {
                    return std::nullopt;
                }
                v = {*h, *x, *y};
                space = name == "hwb" ? color::Space::Hwb : color::Space::Hsl;
            } else if (name == "lab" || name == "oklab") {
                const bool ok = name == "oklab";
                const auto l = amount(c[0], ok ? 1 : 100);
                const auto a = amount(c[1], ok ? 0.4 : 125);
                const auto b = amount(c[2], ok ? 0.4 : 125);
                if (!l || !a || !b) {
                    return std::nullopt;
                }
                v = {*l, *a, *b};
                space = ok ? color::Space::Oklab : color::Space::Lab;
            } else if (name == "lch" || name == "oklch") {
                const bool ok = name == "oklch";
                const auto l = amount(c[0], ok ? 1 : 100);
                const auto ch = amount(c[1], ok ? 0.4 : 150);
                const auto h = hue_amount(c[2]);
                if (!l || !ch || !h) {
                    return std::nullopt;
                }
                v = {*l, std::max(*ch, 0.0), *h};
                space = ok ? color::Space::Oklch : color::Space::Lch;
            } else {
                return std::nullopt;
            }
            const auto rgb = color::to_srgb(*space, v);
            return color::Rgba{rgb[0], rgb[1], rgb[2], std::clamp(*alpha, 0.0, 1.0)};
        }

        // A <color> token: a hex color, a named color, transparent, currentcolor (the given current color) or a color
        // function; none for any other token.
        std::optional<color::Rgba> color_value(const Token& t, const std::optional<color::Rgba>& current) {
            if (t.kind == Kind::Hash) {
                const auto hex = lower(t.text.substr(1));
                if (hex.size() != 3 && hex.size() != 4 && hex.size() != 6 && hex.size() != 8) {
                    return std::nullopt;
                }
                const auto digit = [](char c) -> int {
                    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
                };
                std::array<int, 8> d{};
                for (std::size_t i = 0; i < hex.size(); ++i) {
                    if ((d[i] = digit(hex[i])) < 0) {
                        return std::nullopt;
                    }
                }
                color::Rgba c{0, 0, 0, 1};
                if (hex.size() <= 4) {
                    for (std::size_t i = 0; i < hex.size(); ++i) {
                        c[i] = (d[i] * 17) / 255.0;
                    }
                } else {
                    for (std::size_t i = 0; i < hex.size() / 2; ++i) {
                        c[i] = (d[2 * i] * 16 + d[2 * i + 1]) / 255.0;
                    }
                }
                return c;
            }
            if (t.kind == Kind::Ident) {
                const auto name = lower(t.text);
                if (name == "currentcolor") {
                    return current;
                }
                return color::named(name);
            }
            if (t.kind == Kind::Function) {
                return color_function(t, current);
            }
            return std::nullopt;
        }

        bool color_function_name(std::string_view name) {
            for (const auto* n :
                 {"rgb", "rgba", "hsl", "hsla", "hwb", "lab", "lch", "oklab", "oklch", "color", "color-mix"}) {
                if (name == n) {
                    return true;
                }
            }
            return false;
        }

        // A color as the scene draws it: #rrggbb when opaque, rgba() of 8-bit channels otherwise.
        Token color_token(color::Rgba c) {
            c = color::clip(c);
            const auto channel = [](double v) {
                return int(std::lround(v * 255));
            };
            if (c[3] >= 1) {
                char text[8];
                std::snprintf(text, sizeof text, "#%02x%02x%02x", channel(c[0]), channel(c[1]), channel(c[2]));
                return {Kind::Hash, text, {}};
            }
            Token out{Kind::Function, "rgba", {}};
            for (int i = 0; i < 3; ++i) {
                out.args.push_back({Kind::Number, std::to_string(channel(c[std::size_t(i)])), {}});
                out.args.push_back({Kind::Delim, ",", {}});
            }
            char alpha[32];
            std::snprintf(alpha, sizeof alpha, "%.4g", c[3]);
            out.args.push_back({Kind::Number, alpha, {}});
            return out;
        }

        // The colors of a value computed to the forms the scene draws, inside gradients and shadows too; a color it
        // cannot read fails (the declaration is invalid at computed-value time). A bare currentcolor stays for the
        // scene to resolve as it inherits.
        void normalize_colors(Tokens& tokens, const std::optional<color::Rgba>& current) {
            for (auto& t : tokens) {
                if (t.kind == Kind::Hash) {
                    const auto c = color_value(t, current);
                    if (!c) {
                        fail("color " + t.text);
                    }
                    t = color_token(*c);
                } else if (t.kind == Kind::Ident) {
                    const auto name = lower(t.text);
                    if (name != "currentcolor" && name != "transparent") {
                        if (const auto c = color::named(name)) {
                            t = color_token(*c);
                        }
                    }
                } else if (t.kind == Kind::Function) {
                    const auto name = lower(t.text);
                    // A URL holds no colors: url(#id) names an element (a filter's SVG filter).
                    if (name == "url") {
                        continue;
                    }
                    if (color_function_name(name)) {
                        const auto c = color_value(t, current);
                        if (!c) {
                            fail("color " + serialize({t}) + " outside the profile");
                        }
                        t = color_token(*c);
                        continue;
                    }
                    if (name.ends_with("-gradient")) {
                        // A gradient's interpolation color space ("to right in oklab"): the scene interpolates in
                        // sRGB, the space CSS gradients used before; the clause goes.
                        for (std::size_t i = 0; i < t.args.size(); ++i) {
                            if (t.args[i].kind == Kind::Delim && t.args[i].text == ",") {
                                break;
                            }
                            if (t.args[i].kind == Kind::Ident && lower(t.args[i].text) == "in" &&
                                i + 1 < t.args.size()) {
                                std::size_t end = i + 2;
                                if (end + 1 < t.args.size() && t.args[end + 1].kind == Kind::Ident &&
                                    lower(t.args[end + 1].text) == "hue") {
                                    end += 2;
                                }
                                t.args.erase(t.args.begin() + std::ptrdiff_t(i), t.args.begin() + std::ptrdiff_t(end));
                                // A first argument that was only the clause leaves a leading comma.
                                if (i == 0 && !t.args.empty() && t.args[0].kind == Kind::Delim &&
                                    t.args[0].text == ",") {
                                    t.args.erase(t.args.begin());
                                }
                                break;
                            }
                        }
                    }
                    normalize_colors(t.args, current);
                }
            }
        }

        // Whether every color of a value parses (@supports).
        bool colors_parse(const Tokens& tokens) {
            try {
                Tokens copy = tokens;
                normalize_colors(copy, color::Rgba{0, 0, 0, 1});
                return true;
            } catch (const std::invalid_argument&) {
                return false;
            }
        }

        // ---- calc(), min(), max() and clamp() simplified (CSS Values 4 §10.10). ----

        // A sum of terms by canonical unit: "" numbers, "%" percentages, "px" absolute lengths (rem at the default
        // font size), "deg" angles, "ms" times, and other units as they are (em, the viewport units).
        using Sum = std::map<std::string, double>;

        std::optional<Sum> term(const Token& t);
        std::optional<Sum> sum_of(const Tokens& tokens);

        std::optional<Sum> single(std::string unit, double v) {
            if (!std::isfinite(v)) {
                // infinity clamps to a large finite value (CSS Values 4 §10.9).
                v = v > 0 ? constants::max_style_number_magnitude : -constants::max_style_number_magnitude;
            }
            return Sum{{std::move(unit), v}};
        }

        std::optional<Sum> term(const Token& t) {
            switch (t.kind) {
                case Kind::Number: {
                    const auto n = css::parse_number<double>(t.text);
                    return n ? single("", *n) : std::nullopt;
                }
                case Kind::Percentage: {
                    const auto n = css::parse_number<double>(t.text.substr(0, t.text.size() - 1));
                    return n ? single("%", *n) : std::nullopt;
                }
                case Kind::Dimension: {
                    std::size_t unit = t.text.size();
                    while (unit && std::isalpha(static_cast<unsigned char>(t.text[unit - 1]))) {
                        --unit;
                    }
                    const auto n = css::parse_number<double>(t.text.substr(0, unit));
                    if (!n) {
                        return std::nullopt;
                    }
                    const auto u = lower(t.text.substr(unit));
                    if (u == "px") {
                        return single("px", *n);
                    }
                    if (u == "rem") {
                        return single("px", *n * constants::default_font_size_px);
                    }
                    if (u == "deg" || u == "grad" || u == "rad" || u == "turn") {
                        return single("deg", *hue_amount(t));
                    }
                    if (u == "s" || u == "ms") {
                        return single("ms", u == "s" ? *n * 1000 : *n);
                    }
                    return single(u, *n);
                }
                case Kind::Ident: {
                    const auto w = lower(t.text);
                    if (w == "infinity") {
                        return single("", 1e7);
                    }
                    if (w == "-infinity") {
                        return single("", -1e7);
                    }
                    if (w == "pi") {
                        return single("", 3.14159265358979323846);
                    }
                    if (w == "e") {
                        return single("", 2.71828182845904523536);
                    }
                    return std::nullopt;
                }
                case Kind::Function: {
                    const auto name = lower(t.text);
                    if (name.empty() || name == "calc") {
                        return sum_of(t.args);
                    }
                    if (name == "min" || name == "max" || name == "clamp") {
                        std::vector<Sum> items;
                        Tokens item;
                        for (std::size_t i = 0; i <= t.args.size(); ++i) {
                            if (i == t.args.size() || (t.args[i].kind == Kind::Delim && t.args[i].text == ",")) {
                                auto s = sum_of(item);
                                if (!s || s->size() != 1) {
                                    return std::nullopt;
                                }
                                items.push_back(std::move(*s));
                                item.clear();
                            } else {
                                item.push_back(t.args[i]);
                            }
                        }
                        if (items.empty() || (name == "clamp" && items.size() != 3)) {
                            return std::nullopt;
                        }
                        const auto unit = items[0].begin()->first;
                        for (const auto& s : items) {
                            if (s.begin()->first != unit) {
                                return std::nullopt;
                            }
                        }
                        double v = items[0].begin()->second;
                        if (name == "clamp") {
                            v = std::max(items[0].begin()->second,
                                         std::min(items[1].begin()->second, items[2].begin()->second));
                        } else {
                            for (const auto& s : items) {
                                v = name == "min" ? std::min(v, s.begin()->second) : std::max(v, s.begin()->second);
                            }
                        }
                        return single(unit, v);
                    }
                    return std::nullopt;
                }
                default:
                    return std::nullopt;
            }
        }

        bool unitless(const Sum& s) {
            return s.size() == 1 && s.begin()->first.empty();
        }

        // sum := product (('+' | '-') product)*; product := term (('*' | '/') term)*.
        std::optional<Sum> sum_of(const Tokens& tokens) {
            Sum total;
            std::optional<Sum> product;
            double sign = 1;
            char op = 0;
            const auto add = [&](const Sum& s, double k) {
                for (const auto& [u, v] : s) {
                    total[u] += k * v;
                }
            };
            for (const auto& t : tokens) {
                if (t.kind == Kind::Delim && (t.text == "+" || t.text == "-")) {
                    if (!product || op) {
                        return std::nullopt;
                    }
                    add(*product, sign);
                    product.reset();
                    sign = t.text == "-" ? -1 : 1;
                    continue;
                }
                if (t.kind == Kind::Delim && (t.text == "*" || t.text == "/")) {
                    if (!product || op) {
                        return std::nullopt;
                    }
                    op = t.text[0];
                    continue;
                }
                auto value = term(t);
                if (!value) {
                    return std::nullopt;
                }
                if (!product) {
                    if (op) {
                        return std::nullopt;
                    }
                    product = std::move(*value);
                } else if (op == '*') {
                    if (unitless(*value)) {
                        for (auto& [u, v] : *product) {
                            v *= value->begin()->second;
                        }
                    } else if (unitless(*product)) {
                        const double k = product->begin()->second;
                        product = std::move(*value);
                        for (auto& [u, v] : *product) {
                            v *= k;
                        }
                    } else {
                        return std::nullopt;
                    }
                } else if (op == '/') {
                    if (!unitless(*value) || value->begin()->second == 0) {
                        return std::nullopt;
                    }
                    for (auto& [u, v] : *product) {
                        v /= value->begin()->second;
                    }
                } else {
                    // Two terms without an operator.
                    return std::nullopt;
                }
                op = 0;
            }
            if (!product || op) {
                return std::nullopt;
            }
            add(*product, sign);
            // Terms that cancel out go (but keep one).
            for (auto it = total.begin(); it != total.end() && total.size() > 1;) {
                it = it->second == 0 ? total.erase(it) : std::next(it);
            }
            return total;
        }

        std::string number_text(double v) {
            if (std::abs(v) < 1e-9) {
                v = 0;
            }
            char text[32];
            std::snprintf(text, sizeof text, "%.6g", v);
            return text;
        }

        // A sum as tokens: one term as itself, more as calc(a + b - c) with percentages first.
        Token sum_token(const Sum& s) {
            const auto one = [](const std::string& unit, double v) -> Token {
                const auto n = number_text(v);
                if (unit.empty()) {
                    return {Kind::Number, n, {}};
                }
                if (unit == "%") {
                    return {Kind::Percentage, n + "%", {}};
                }
                return {Kind::Dimension, n + unit, {}};
            };
            if (s.size() == 1) {
                return one(s.begin()->first, s.begin()->second);
            }
            Token out{Kind::Function, "calc", {}};
            std::vector<std::pair<std::string, double>> terms(s.begin(), s.end());
            std::stable_sort(terms.begin(), terms.end(), [](const auto& a, const auto& b) {
                return (a.first == "%") > (b.first == "%");
            });
            for (const auto& [u, v] : terms) {
                if (!out.args.empty()) {
                    out.args.push_back({Kind::Delim, v < 0 ? "-" : "+", {}});
                    out.args.push_back(one(u, std::abs(v)));
                } else {
                    out.args.push_back(one(u, v));
                }
            }
            return out;
        }

        // The math functions of a value simplified, inside other functions too: one that reduces to a single term is
        // that term; one that does not stays a canonical calc() (or as it was).
        void simplify_math(Tokens& tokens) {
            for (auto& t : tokens) {
                if (t.kind != Kind::Function) {
                    continue;
                }
                const auto name = lower(t.text);
                if (name == "calc" || name == "min" || name == "max" || name == "clamp" || name.empty()) {
                    simplify_math(t.args);
                    if (const auto s = name.empty() ? sum_of(t.args) : term(t)) {
                        t = sum_token(*s);
                    }
                } else {
                    simplify_math(t.args);
                }
            }
        }

        // Numbers beyond the range the profiles take clamp to it, as calc(infinity) does (CSS Values 4 §4.2): Tailwind's
        // rounded-full is border-radius: 3.40282e38px.
        void clamp_numbers(Tokens& tokens) {
            for (auto& t : tokens) {
                if (t.kind == Kind::Function) {
                    clamp_numbers(t.args);
                    continue;
                }
                if (t.kind != Kind::Number && t.kind != Kind::Dimension && t.kind != Kind::Percentage) {
                    continue;
                }
                std::size_t unit = t.text.size();
                if (t.kind == Kind::Percentage) {
                    --unit;
                }
                while (t.kind == Kind::Dimension && unit &&
                       std::isalpha(static_cast<unsigned char>(t.text[unit - 1]))) {
                    --unit;
                }
                const auto n = css::parse_number<double>(t.text.substr(0, unit));
                if (n && std::abs(*n) > constants::max_style_number_magnitude) {
                    t.text = number_text(*n > 0 ? constants::max_style_number_magnitude
                                                : -constants::max_style_number_magnitude) +
                             t.text.substr(unit);
                }
            }
        }

        // ---- @supports (CSS Conditional 3) as a forgiving profile answers it. ----

        bool supports_declaration(std::string_view text, const StyleProfile& profile) {
            const auto colon = text.find(':');
            if (colon == std::string_view::npos) {
                return false;
            }
            auto key = trim(text.substr(0, colon));
            if (custom(key)) {
                return true;
            }
            key = lower(key);
            if (std::find(profile.ignored.begin(), profile.ignored.end(), key) != profile.ignored.end()) {
                return false;
            }
            std::vector<Declaration> out;
            try {
                declaration(text, profile, out);
            } catch (const std::invalid_argument&) {
                return false;
            }
            if (out.empty()) {
                return false;
            }
            for (const auto& d : out) {
                if (!profile.properties.contains(d.key)) {
                    return false;
                }
                if (color_bearing(d.key) && !references(d.tokens) && !colors_parse(d.tokens)) {
                    return false;
                }
            }
            return true;
        }

        bool supports_parts(const std::vector<std::string>& parts, std::size_t begin, std::size_t end,
                            const StyleProfile& profile);

        bool supports_group(const std::string& part, const StyleProfile& profile) {
            if (part.starts_with("selector(")) {
                const dom::SelectorGrammar grammar{true, true, Diag::StyleProfile, Diag::StyleProfile, true};
                const auto inside = std::string_view(part).substr(9, part.size() - 10);
                const auto list = dom::parse_selector_list(inside, grammar, 64);
                return bool(list);
            }
            if (!part.starts_with("(")) {
                // font-tech(), font-format() and general enclosed: false.
                return false;
            }
            const auto inside = trim(std::string_view(part).substr(1, part.size() - 2));
            if (!inside.empty() && (inside[0] == '(' || lower(inside).starts_with("not ") ||
                                    lower(inside).starts_with("not(") || lower(inside).starts_with("selector("))) {
                const auto parts = condition_parts(inside);
                return supports_parts(parts, 0, parts.size(), profile);
            }
            return supports_declaration(inside, profile);
        }

        bool supports_parts(const std::vector<std::string>& parts, std::size_t begin, std::size_t end,
                            const StyleProfile& profile) {
            if (begin >= end) {
                return false;
            }
            if (parts[begin] == "not") {
                return begin + 1 < end && !supports_group(parts[begin + 1], profile);
            }
            bool any = false;
            bool all = true;
            bool disjunction = false;
            for (std::size_t i = begin; i < end; ++i) {
                if (parts[i] == "and") {
                    continue;
                }
                if (parts[i] == "or") {
                    disjunction = true;
                    continue;
                }
                const bool holds = supports_group(parts[i], profile);
                any |= holds;
                all &= holds;
            }
            return disjunction ? any : all;
        }

        bool supports(std::string_view condition, const StyleProfile& profile) {
            const auto parts = condition_parts(condition);
            return supports_parts(parts, 0, parts.size(), profile);
        }

        using Variables = std::map<std::string, std::optional<Tokens>>;
    } // namespace

    bool media_matches(std::string_view query, float width, float height, const InputDevices& devices) {
        if (trim(query).empty()) {
            return true;
        }
        Warnings warnings;
        try {
            return media_query_list(query, warnings).holds(width, height, devices);
        } catch (const DiagnosticError&) {
            // A query that does not parse is "not all".
            return false;
        }
    }

    struct StyleEnvironment {
        Variables variables;
    };

    namespace {

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

        // An HTML dimension attribute's value as CSS (HTML's rules for parsing dimension values): its leading number,
        // a percentage when a '%' follows it and px otherwise ("12.5", "50%", "20px" is 20px); none when it does not
        // start with a digit after spaces.
        std::optional<std::string> html_dimension(std::string_view value) {
            std::size_t i = 0;
            while (i < value.size() &&
                   (value[i] == ' ' || value[i] == '\t' || value[i] == '\n' || value[i] == '\f' || value[i] == '\r')) {
                ++i;
            }
            const auto digit = [&](std::size_t at) {
                return at < value.size() && value[at] >= '0' && value[at] <= '9';
            };
            if (!digit(i)) {
                return std::nullopt;
            }
            const auto start = i;
            while (digit(i)) {
                ++i;
            }
            auto end = i;
            if (i < value.size() && value[i] == '.' && digit(i + 1)) {
                ++i;
                while (digit(i)) {
                    ++i;
                }
                end = i;
            }
            std::string number(value.substr(start, end - start));
            return number + (end < value.size() && value[end] == '%' ? "%" : "px");
        }

        // The direction an HTML element's dir=auto (and a bdi's whose dir is not ltr or rtl) gives it, as Chrome's
        // HTMLElement maps it to style: its text's first strong character's (bidi class L, R or AL), skipping the text
        // of descendants that are bdi, script, style or textarea or have a dir attribute; ltr without one. None for
        // another element.
        std::optional<std::string_view> auto_direction(const DomSnapshot& dom) {
            if (dom.type != constants::element_node_type ||
                (!dom.namespace_uri.empty() && dom.namespace_uri != constants::xhtml_namespace_uri)) {
                return std::nullopt;
            }
            const auto it = dom.attributes.find("dir");
            const auto dir = it == dom.attributes.end() ? std::string_view() : std::string_view(it->second);
            if (dir != "auto" && !(dom.local_name == "bdi" && dir != "ltr" && dir != "rtl")) {
                return std::nullopt;
            }
            const auto first = [](auto&& self, const DomSnapshot& node) -> std::optional<bool> {
                for (const auto& c : node.children) {
                    if (c.type == constants::text_node_type) {
                        if (const auto rtl = bidi_first_strong_rtl(c.data)) {
                            return rtl;
                        }
                    } else if (c.type == constants::element_node_type && c.local_name != "bdi" &&
                               c.local_name != "script" && c.local_name != "style" && c.local_name != "textarea" &&
                               !c.attributes.contains("dir")) {
                        if (const auto rtl = self(self, c)) {
                            return rtl;
                        }
                    }
                }
                return std::nullopt;
            };
            return first(first, dom).value_or(false) ? std::string_view("rtl") : std::string_view("ltr");
        }

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

        // Calls f for every compound of a selector, those of its :is(), :where(), :not() and :nth-child(of S) too.
        template <class F>
        void each_compound(const dom::Selector& s, F&& f) {
            for (const auto& part : s.parts) {
                f(part);
                for (const auto& alternative : part.alternatives) {
                    for (const auto& inner : alternative.list) {
                        each_compound(inner, f);
                    }
                }
                for (const auto& n : part.nth) {
                    for (const auto& inner : n.of) {
                        each_compound(inner, f);
                    }
                }
            }
        }

        // An author sheet's rules by the key of their subject compound: its first id, else its first class, else its
        // type; the rest are universal. An element's candidates merge in sheet order.
        struct RuleIndex {
            std::unordered_map<std::string, std::vector<std::uint32_t>> ids;
            std::unordered_map<std::string, std::vector<std::uint32_t>> classes;
            std::unordered_map<std::string, std::vector<std::uint32_t>> tags;
            std::vector<std::uint32_t> universal;

            RuleIndex() = default;

            explicit RuleIndex(const std::vector<Rule>& rules) {
                for (std::uint32_t i = 0; i < rules.size(); ++i) {
                    const auto& subject = rules[i].selector.parts.back();
                    if (!subject.ids.empty()) {
                        ids[subject.ids.front()].push_back(i);
                    } else if (!subject.classes.empty()) {
                        classes[subject.classes.front()].push_back(i);
                    } else if (!subject.tag.empty() && subject.tag != "*") {
                        tags[subject.lower_tag].push_back(i);
                    } else {
                        universal.push_back(i);
                    }
                }
            }

            void candidates(const DomSnapshot& n, std::vector<std::uint32_t>& out) const {
                out.clear();
                const auto take = [&](const std::vector<std::uint32_t>& list) {
                    out.insert(out.end(), list.begin(), list.end());
                };
                take(universal);
                if (const auto id = n.attributes.find("id"); id != n.attributes.end() && !ids.empty()) {
                    if (const auto it = ids.find(id->second); it != ids.end()) {
                        take(it->second);
                    }
                }
                if (const auto list = n.attributes.find("class"); list != n.attributes.end() && !classes.empty()) {
                    const std::string_view text = list->second;
                    std::size_t i{};
                    while (i < text.size()) {
                        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' ||
                                                   text[i] == '\f' || text[i] == '\r')) {
                            ++i;
                        }
                        const auto start = i;
                        while (i < text.size() && !(text[i] == ' ' || text[i] == '\t' || text[i] == '\n' ||
                                                    text[i] == '\f' || text[i] == '\r')) {
                            ++i;
                        }
                        if (i > start) {
                            if (const auto it = classes.find(std::string(text.substr(start, i - start)));
                                it != classes.end()) {
                                take(it->second);
                            }
                        }
                    }
                }
                if (!tags.empty()) {
                    if (const auto it = tags.find(lower(n.local_name)); it != tags.end()) {
                        take(it->second);
                    }
                }
                std::sort(out.begin(), out.end());
                out.erase(std::unique(out.begin(), out.end()), out.end());
            }
        };

        // What a forgiving profile's cascade reads besides the rules: their index, the sheet's registered custom
        // properties, where to warn, and the viewport @media conditions read.
        struct ForgivingContext {
            const RuleIndex* index{};
            const std::map<std::string, Registered>* registered{};
            Warnings* warnings{};
            float width{};
            float height{};
            InputDevices devices{};
        };

        // A computed color value read back (the forms normalize_colors writes, or what a strict profile keeps).
        std::optional<color::Rgba> parse_computed_color(std::string_view text) {
            try {
                Lexer lexer{text};
                lexer.forgiving = true;
                const auto tokens = lexer.scan();
                return tokens.size() == 1 ? color_value(tokens[0], std::nullopt) : std::nullopt;
            } catch (const std::invalid_argument&) {
                return std::nullopt;
            }
        }

        struct Engine {
            const StyleProfile& profile;
            const ProfileIndex& profile_index;
            const std::vector<Rule>& ua;
            const std::vector<Rule>& author;
            const ForgivingContext* forgiving{};
            std::vector<std::uint32_t> candidates;
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
            // What the modern grammar's selectors read: focus (:focus and :focus-visible, and apart :focus-within,
            // which every ancestor of the focused element matches), an element's position among its siblings, its
            // siblings themselves (+, ~, :nth-child(of S)), and whether it is empty.
            bool uses_focus{};
            bool uses_focus_within{};
            bool uses_position{};
            bool uses_siblings{};
            bool uses_empty{};
            // Selectors read text fields (:read-only, :read-write, :placeholder-shown), and rules style a text field's
            // ::placeholder or ::selection.
            bool uses_field{};
            bool placeholder_rules{};
            bool selection_rules{};
            bool parent_all{};
            // The rules whose subject is ::before or ::after and that declare content: only a pseudo-element one of
            // them gives makes a box (Tailwind's preflight gives every element's ::before and ::after a style, but no
            // content).
            std::vector<const Rule*> content_rules;
            // The attributes an element's values depend on beyond its selectors': an img's width and height.
            std::set<std::string> memo_attributes{"id", "class", "width", "height"};
            std::vector<dom::Compound> state_parts;
            std::string selector_path;
            std::vector<std::size_t> selector_lengths;

            // The inline style an element was last restyled to (restyle_inline), and its declarations that win among
            // themselves, by key: its next restyle starts from them.
            struct InlineLast {
                std::string text;
                std::shared_ptr<const std::map<std::string, Declaration>> winners;
            };

            struct IdentityHash {
                std::size_t operator()(const NodeIdentity& n) const {
                    return std::hash<std::uint64_t>{}(n.view_epoch ^ (std::uint64_t(n.slot) << 32 | n.generation));
                }
            };

            struct MemoState {
                std::map<std::string, Memo> entries;
                std::map<std::string, Winners> winners;
                // By element: an animated element's style is parsed once.
                std::unordered_map<NodeIdentity, InlineLast, IdentityHash> inline_last;
                // Each memo's entries by last use (`used`, unique), least recent first: what the bounds evict.
                std::map<std::size_t, std::map<std::string, Memo>::iterator> entry_uses;
                std::map<std::size_t, std::map<std::string, Winners>::iterator> winner_uses;
                std::size_t bytes{};
                std::size_t winner_bytes{};
                std::size_t clock{};
            };

            // A memo entry taken again: its use is the latest.
            template <class Uses, class Iterator>
            static void touch(Uses& uses, Iterator it, std::size_t& clock) {
                uses.erase(it->second.used);
                it->second.used = ++clock;
                uses.emplace(it->second.used, it);
            }

            MemoState local_memo;
            MemoState* memo_state = &local_memo;
            std::vector<NodeIdentity>* changed_nodes{};
            // An update from a previous document: the stamp of the nodes whose own values change.
            std::uint64_t stamp{};

            Engine(const StyleProfile& p, const ProfileIndex& i, const std::vector<Rule>& u, const std::vector<Rule>& a,
                   bool reuse = false, MemoState* state = nullptr, std::vector<NodeIdentity>* changed = nullptr,
                   const ForgivingContext* forgiving_context = nullptr)
                : profile(p), profile_index(i), ua(u), author(a), forgiving(forgiving_context), memoize(reuse) {
                if (state) {
                    memo_state = state;
                }
                changed_nodes = changed;
                for (const auto* list : {&ua, &author}) {
                    for (const auto& rule : *list) {
                        for (const auto relation : rule.selector.relations) {
                            uses_siblings |= relation == '+' || relation == '~';
                        }
                        each_compound(rule.selector, [&](const dom::Compound& part) {
                            uses_hover |= part.hover;
                            uses_active |= part.active;
                            uses_focus |= part.focus || part.focus_visible;
                            uses_focus_within |= part.focus_within;
                            uses_empty |= part.empty;
                            uses_field |= part.read_only || part.read_write || part.placeholder_shown;
                            uses_position |= !part.nth.empty();
                            for (const auto& n : part.nth) {
                                uses_siblings |= !n.of.empty();
                            }
                            for (const auto& alternative : part.alternatives) {
                                for (const auto& inner : alternative.list) {
                                    for (const auto relation : inner.relations) {
                                        uses_siblings |= relation == '+' || relation == '~';
                                    }
                                }
                            }
                            if (part.hover || part.active) {
                                state_parts.push_back(lone_state_part(part));
                            }
                            for (const auto& attribute : part.attributes) {
                                memo_attributes.insert(attribute.name);
                                memo_attributes.insert(attribute.lower_name);
                            }
                            if (part.disabled || part.enabled || part.checked) {
                                memo_attributes.insert("disabled");
                                memo_attributes.insert("checked");
                                memo_attributes.insert("selected");
                            }
                        });
                        for (const auto& d : rule.declarations) {
                            parent_all |= keyword(d.tokens) == "inherit" || references(d.tokens);
                        }
                        const auto pseudo = rule.selector.pseudo();
                        placeholder_rules |= pseudo == dom::PseudoElement::Placeholder;
                        selection_rules |= pseudo == dom::PseudoElement::Selection;
                        if ((pseudo == dom::PseudoElement::Before || pseudo == dom::PseudoElement::After) &&
                            std::any_of(rule.declarations.begin(), rule.declarations.end(), [](const Declaration& d) {
                                return d.key == "content";
                            })) {
                            content_rules.push_back(&rule);
                        }
                    }
                }
            }

            // A cascade-wide bound was passed: the document fails, whatever the profile.
            bool bounds_passed() const {
                return work > std::size_t(constants::style::max_token_work) ||
                       bytes > std::size_t(constants::style::max_computed_output_bytes) ||
                       nodes > std::size_t(constants::max_document_nodes);
            }

            void warn(std::string text) const {
                if (forgiving && forgiving->warnings) {
                    forgiving->warnings->add(std::move(text));
                }
            }

            Warnings* warnings() const {
                return forgiving ? forgiving->warnings : nullptr;
            }

            const Registered* registered(const std::string& name) const {
                if (!forgiving || !forgiving->registered) {
                    return nullptr;
                }
                const auto it = forgiving->registered->find(name);
                return it == forgiving->registered->end() ? nullptr : &it->second;
            }

            // The custom properties a child inherits from variables: all but the registered ones that do not inherit.
            Variables inheritable(const Variables& variables) const {
                if (!forgiving || !forgiving->registered || forgiving->registered->empty()) {
                    return variables;
                }
                Variables out = variables;
                for (auto it = out.begin(); it != out.end();) {
                    const auto* r = registered(it->first);
                    it = r && !r->inherits ? out.erase(it) : std::next(it);
                }
                return out;
            }

            // Whether n, on the path, is the document's root element (:root).
            bool at_root(const DomSnapshot& n) const {
                return !path.empty() && &n == path.front() && n.local_name == "html";
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
                const auto [h, a] = seen_state(state_parts, *n, at_root(*n));
                if (uses_hover) {
                    selector_path += h ? '1' : '0';
                }
                if (uses_active) {
                    selector_path += a ? '1' : '0';
                }
                if (uses_focus) {
                    selector_path += n->focused ? 'F' : 'f';
                    selector_path += n->focus_visible ? 'V' : 'v';
                }
                if (uses_focus_within) {
                    selector_path += n->focus_within ? 'W' : 'w';
                }
                if (uses_empty) {
                    selector_path += has_content(*n) ? 'C' : 'c';
                }
                if (uses_field) {
                    const auto [writable, placeholder] = field_flags(*n);
                    selector_path += writable ? 'R' : 'r';
                    selector_path += placeholder ? 'P' : 'p';
                }
                for (const auto& [k, v] : n->attributes) {
                    if (memo_attributes.contains(k)) {
                        add(k);
                        add(v);
                    }
                }
                // What the structural selectors read: the element's position among its element siblings, of every
                // type and of its own, from each end; and with sibling combinators the siblings themselves.
                if ((uses_position || uses_siblings) && path.size() >= 2) {
                    std::size_t index{};
                    std::size_t count{};
                    std::size_t type_index{};
                    std::size_t type_count{};
                    for (const auto& s : path[path.size() - 2]->children) {
                        if (s.type != constants::element_node_type) {
                            continue;
                        }
                        const bool same_type = s.local_name == n->local_name && s.namespace_uri == n->namespace_uri;
                        if (&s == n) {
                            index = count;
                            type_index = type_count;
                            selector_path += '@';
                        } else if (uses_siblings) {
                            add(s.local_name);
                            selector_path += s.hovered ? 'h' : '-';
                            selector_path += s.active ? 'a' : '-';
                            for (const auto& [k, v] : s.attributes) {
                                if (memo_attributes.contains(k)) {
                                    add(k);
                                    add(v);
                                }
                            }
                        }
                        ++count;
                        type_count += same_type ? 1 : 0;
                    }
                    add(std::to_string(index) + "/" + std::to_string(count) + "/" + std::to_string(type_index) + "/" +
                        std::to_string(type_count));
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
                    // dir=auto's direction comes from the element's text.
                    if (const auto direction = auto_direction(dom)) {
                        add("dir");
                        add(*direction);
                    }
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

            // Whether the own values changed.
            bool own_into(ComputedStyle& target, const ComputedStyle& own, NodeIdentity current_identity) {
                if (target.identity == current_identity && target.local_name == own.local_name &&
                    target.values == own.values && target.custom_properties == own.custom_properties &&
                    target.invalid_at_computed_value == own.invalid_at_computed_value &&
                    target.specified_properties == own.specified_properties) {
                    return false;
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
                target.stamp = stamp;
                return true;
            }

            // Whether two copies of an element hold the same values for its own cascade: its name, attributes and
            // the state selectors can see (root: the copies are of the document's root element).
            bool same_own_input(const DomSnapshot& a, const DomSnapshot& b, bool root) const {
                return a.type == b.type && a.local_name == b.local_name && a.namespace_uri == b.namespace_uri &&
                       a.attributes == b.attributes &&
                       seen_state(state_parts, a, root) == seen_state(state_parts, b, root) &&
                       (!uses_focus || (a.focused == b.focused && a.focus_visible == b.focus_visible)) &&
                       (!uses_focus_within || a.focus_within == b.focus_within) &&
                       (!uses_empty || has_content(a) == has_content(b)) &&
                       (!uses_field || field_flags(a) == field_flags(b)) &&
                       ((!placeholder_rules && !selection_rules) || (a.field != nullptr) == (b.field != nullptr));
            }

            // Whether two copies of an element read alike to the selectors of its descendants: name, the state
            // selectors can see and the attributes they read.
            bool same_selector_input(const DomSnapshot& a, const DomSnapshot& b, bool root) const {
                if (a.local_name != b.local_name || a.namespace_uri != b.namespace_uri ||
                    seen_state(state_parts, a, root) != seen_state(state_parts, b, root) ||
                    (uses_focus && (a.focused != b.focused || a.focus_visible != b.focus_visible)) ||
                    (uses_focus_within && a.focus_within != b.focus_within) ||
                    (uses_field && field_flags(a) != field_flags(b))) {
                    return false;
                }
                const auto read = [&](const AttributeMap& m, AttributeMap::const_iterator it) {
                    while (it != m.end() && !memo_attributes.contains(it->first)) {
                        ++it;
                    }
                    return it;
                };
                auto x = read(a.attributes, a.attributes.begin());
                auto y = read(b.attributes, b.attributes.begin());
                for (;;) {
                    if (x == a.attributes.end() || y == b.attributes.end()) {
                        return x == a.attributes.end() && y == b.attributes.end();
                    }
                    if (*x != *y) {
                        return false;
                    }
                    x = read(a.attributes, std::next(x));
                    y = read(b.attributes, std::next(y));
                }
            }

            // Whether two values of an element hold the same values for its children to inherit.
            bool same_inherited_values(const StyleValues& a, const StyleValues& b) const {
                const auto next = [&](const StyleValues& v, StyleValues::const_iterator it) {
                    while (it != v.end() && !profile_index.inherited[std::size_t(it->first)]) {
                        ++it;
                    }
                    return it;
                };
                for (auto x = next(a, a.begin()), y = next(b, b.begin());;
                     x = next(a, std::next(x)), y = next(b, std::next(y))) {
                    if (x == a.end() || y == b.end()) {
                        return x == a.end() && y == b.end();
                    }
                    if (*x != *y) {
                        return false;
                    }
                }
            }

            // Whether a child of dom may read its parent's values beyond those it inherits: an `inherit`, or a var()
            // that could give one, in a rule or in the child's own declarations (as memo_key keys a child).
            bool children_read_all(const DomSnapshot& dom) const {
                if (parent_all) {
                    return true;
                }
                const auto reads = [](std::string_view text) {
                    return contains_ascii_lower(text, "inherit") || contains_ascii_lower(text, "var(");
                };
                for (const auto& child : dom.children) {
                    if (child.type != constants::element_node_type) {
                        continue;
                    }
                    if (const auto it = child.attributes.find("style");
                        it != child.attributes.end() && reads(it->second)) {
                        return true;
                    }
                    if (profile.scene_syntax) {
                        for (const auto k : presentation_attributes) {
                            if (const auto it = child.attributes.find(k);
                                it != child.attributes.end() && reads(it->second)) {
                                return true;
                            }
                        }
                    }
                }
                return false;
            }

            // Whether a change of dom's own values from target's to own reaches its children (children_read_all);
            // target holding another node's values reaches them.
            bool reaches_children(const DomSnapshot& dom, const ComputedStyle& target, const ComputedStyle& own) const {
                return target.identity != dom.identity || !same_inherited_values(target.values, own.values) ||
                       children_read_all(dom);
            }

            static bool same_declaration(const Declaration& a, const Declaration& b) {
                return a.key == b.key && a.tokens == b.tokens && a.important == b.important &&
                       a.from_inset == b.from_inset && a.box_edge == b.box_edge && a.corner == b.corner &&
                       a.axis == b.axis && a.solid_background == b.solid_background &&
                       a.background_layer == b.background_layer && a.id == b.id;
            }

            // An element whose own input changed in its inline style alone (old, the copy it was last computed from),
            // apart from attributes the cascade does not read (an HTML element's that no selector reads), where the
            // declarations that changed set values no child inherits, without var() or a CSS-wide keyword,
            // as do the declarations that win their properties before and after: only those values are cascaded
            // again, against the rule winners kept for its selectors, and the bounds are charged as a full cascade of
            // it charges them. Whether its own values changed; nullopt, with nothing changed, where it is to be
            // cascaded in full.
            std::optional<bool> restyle_inline(const DomSnapshot& dom, const DomSnapshot& old, ComputedStyle& target,
                                               const std::string& selectors, bool root) {
                if (!memoize || dom.namespace_uri != constants::xhtml_namespace_uri || dom.type != old.type ||
                    dom.local_name != old.local_name || dom.namespace_uri != old.namespace_uri ||
                    !same_selector_input(dom, old, root) || (uses_empty && has_content(dom) != has_content(old))) {
                    return std::nullopt;
                }
                const auto winners = memo_state->winners.find(selectors);
                if (winners == memo_state->winners.end()) {
                    return std::nullopt;
                }
                // The inline declarations that win among themselves, by key: the last important one, else the last.
                using Parse = std::shared_ptr<const std::map<std::string, Declaration>>;
                static const Parse none = std::make_shared<const std::map<std::string, Declaration>>();
                const auto parse = [&](const std::string& text) {
                    auto out = std::make_shared<std::map<std::string, Declaration>>();
                    for (const auto& d : declarations(comments(text, profile.scene_syntax), profile, warnings())) {
                        if (const auto found = out->find(d.key);
                            found == out->end() || !found->second.important || d.important) {
                            (*out)[d.key] = d;
                        }
                    }
                    return Parse(std::move(out));
                };
                const auto then_style = old.attributes.find("style");
                const auto now_style = dom.attributes.find("style");
                if (memo_state->inline_last.size() >= constants::max_document_nodes) {
                    memo_state->inline_last.clear();
                }
                auto& last = memo_state->inline_last[dom.identity];
                const auto then_parse = then_style == old.attributes.end()                ? none
                                        : last.winners && last.text == then_style->second ? last.winners
                                                                                          : parse(then_style->second);
                const auto now_parse = now_style == dom.attributes.end() ? none : parse(now_style->second);
                last = {now_style == dom.attributes.end() ? std::string() : now_style->second, now_parse};
                const auto& then_inline = *then_parse;
                const auto& now_inline = *now_parse;
                const auto& rules = winners->second;
                // What wins a property: the inline declaration unless a rule outranks it (as apply ranks it, after
                // every rule), or the rule's.
                const auto winner = [&](const std::map<std::string, Declaration>& inline_values,
                                        const std::string& key) -> const Declaration* {
                    if (const auto it = inline_values.find(key); it != inline_values.end()) {
                        Rank rank{it->second.important ? 1u : 0u, 1, 1, 0, 0, 0, 0, 0};
                        rank[constants::style::cascade_rank_order_index] = std::numeric_limits<unsigned>::max();
                        if (const auto r = rules.ranks.find(key); r == rules.ranks.end() || r->second <= rank) {
                            return &it->second;
                        }
                    }
                    const auto r = rules.declarations.find(key);
                    return r == rules.declarations.end() ? nullptr : &r->second;
                };
                const auto plain = [](const Declaration& d) {
                    return !references(d.tokens) && !wide(keyword(d.tokens)) && d.box_edge < 0 && d.corner < 0 &&
                           d.axis < 0 && d.background_layer < 0 && !d.solid_background;
                };

                struct Contest {
                    const std::string* key{};
                    PropertyId id{};
                    const Declaration* before{};
                    const Declaration* after{};
                };

                std::vector<Contest> contests;
                const auto contest = [&](const std::string& key, PropertyId id) {
                    if (custom(key) || !profile.properties.contains(key) || profile_index.inherited[std::size_t(id)]) {
                        return false;
                    }
                    const auto* before = winner(then_inline, key);
                    const auto* after = winner(now_inline, key);
                    if ((before && !plain(*before)) || (after && !plain(*after))) {
                        return false;
                    }
                    contests.push_back({&key, id, before, after});
                    return true;
                };
                for (const auto& [k, d] : then_inline) {
                    const auto it = now_inline.find(k);
                    if ((it == now_inline.end() || !same_declaration(d, it->second)) && !contest(k, d.id)) {
                        return std::nullopt;
                    }
                }
                for (const auto& [k, d] : now_inline) {
                    if (!then_inline.contains(k) && !contest(k, d.id)) {
                        return std::nullopt;
                    }
                }
                // The charges: the element's as before, but for the declarations that won the properties that changed,
                // which the winners now substitute as apply's winners are.
                std::size_t removed{};
                for (const auto& c : contests) {
                    removed += c.before ? token_count(c.before->tokens) : 0;
                }
                if (removed > target.own_work) {
                    return std::nullopt;
                }
                if (profile.forgiving) {
                    // A value the forgiving profile drops is the full cascade's to drop (with its warning).
                    for (const auto& c : contests) {
                        if (c.after) {
                            Tokens copy = c.after->tokens;
                            try {
                                normalize(*c.key, copy, target.values, nullptr);
                            } catch (const std::invalid_argument&) {
                                return std::nullopt;
                            }
                        }
                    }
                }
                touch(memo_state->winner_uses, winners, memo_state->clock);
                charge(target.own_work - removed);
                const std::function<std::optional<Tokens>(const std::string&)> no_variables = [](const std::string&) {
                    return std::optional<Tokens>{};
                };
                bool changed{};
                for (const auto& c : contests) {
                    if (c.after) {
                        auto tokens = substitute(c.after->tokens, no_variables);
                        if (profile.forgiving) {
                            normalize(*c.key, *tokens, target.values, nullptr);
                        }
                        auto text = serialize(*tokens);
                        if (const auto* current = target.values.find(c.id); !current || current->text() != text) {
                            target.values.set(c.id, CssValue(std::move(text)));
                            changed = true;
                        }
                    } else if (target.values.erase(c.id)) {
                        changed = true;
                    }
                    if (bool(c.before) != bool(c.after)) {
                        auto& specified = target.specified_properties;
                        const auto at = std::lower_bound(specified.begin(), specified.end(), *c.key);
                        if (c.after) {
                            specified.insert(at, *c.key);
                        } else if (at != specified.end() && *at == *c.key) {
                            specified.erase(at);
                        }
                        changed = true;
                    }
                }
                std::size_t own_bytes{};
                for (const auto& [id, v] : target.values) {
                    own_bytes += property_name(id).size() + v.text().size();
                }
                for (const auto& [k, v] : target.custom_properties) {
                    own_bytes += k.size() + (v ? v->size() : 0);
                }
                bytes += own_bytes;
                if (bytes > constants::style::max_computed_output_bytes) {
                    fail("computed output byte bound");
                }
                if (changed) {
                    if (changed_nodes) {
                        changed_nodes->push_back(dom.identity);
                    }
                    target.stamp = stamp;
                }
                return changed;
            }

            // The element children of dom into out, reusing owner values by identity and retaining no DOM references.
            // Order and generation changes are verified even when the rule memo hits. With old, the copy of dom the
            // children were last computed from, a child whose subtree revision did not move keeps its values without a
            // visit unless `dirty` (dom's values changed) or `selectors` (what selectors read of dom or an ancestor
            // changed): its bounds are charged as recorded.
            void children_into(const DomSnapshot& dom, ComputedStyle& out, const Variables& variables,
                               const DomSnapshot* old = nullptr, bool dirty = true, bool selectors = true) {
                auto previous = std::move(out.children);
                out.children.clear();
                out.children.reserve(previous.capacity());
                // Which children there were, in order: a new list of them is a change of the node's.
                std::vector<NodeIdentity> before;
                before.reserve(previous.size());
                for (const auto& c : previous) {
                    before.push_back(c.identity);
                }
                std::size_t hint{};
                std::size_t old_hint{};
                // Selectors that read positions or siblings match anew when the element children are other ones, or
                // in another order; with sibling combinators, every child after one whose selector input changed.
                bool siblings_moved{};
                if ((uses_position || uses_siblings) && old) {
                    std::vector<NodeIdentity> now;
                    std::vector<NodeIdentity> then;
                    for (const auto& c : dom.children) {
                        if (c.type == constants::element_node_type) {
                            now.push_back(c.identity);
                        }
                    }
                    for (const auto& c : old->children) {
                        if (c.type == constants::element_node_type) {
                            then.push_back(c.identity);
                        }
                    }
                    siblings_moved = now != then;
                }
                bool sibling_changed{};
                for (const auto& child : dom.children) {
                    if (child.type == constants::element_node_type) {
                        std::size_t index = hint++;
                        if (index >= previous.size() || previous[index].identity != child.identity) {
                            index = 0;
                            while (index < previous.size() && previous[index].identity != child.identity) {
                                ++index;
                            }
                        }
                        const DomSnapshot* old_child{};
                        if (old) {
                            const auto& siblings = old->children;
                            std::size_t k = old_hint;
                            if (k >= siblings.size() || siblings[k].identity != child.identity) {
                                k = 0;
                                while (k < siblings.size() && siblings[k].identity != child.identity) {
                                    ++k;
                                }
                            }
                            if (k < siblings.size()) {
                                old_child = &siblings[k];
                                old_hint = k + 1;
                            }
                        }
                        out.children.push_back(index < previous.size() ? std::move(previous[index]) : ComputedStyle{});
                        auto& kept = out.children.back();
                        const bool child_selectors = selectors || siblings_moved || sibling_changed;
                        if (uses_siblings && (!old_child || !same_selector_input(child, *old_child, false))) {
                            sibling_changed = true;
                        }
                        if (!dirty && !child_selectors && old_child && child.revision != 0 &&
                            old_child->revision == child.revision && kept.identity == child.identity &&
                            kept.environment) {
                            keep(kept);
                            continue;
                        }
                        compute_into(child, kept, &out, variables, old_child, dirty, child_selectors);
                    }
                }
                if (out.children.size() != before.size() ||
                    !std::equal(before.begin(), before.end(), out.children.begin(),
                                [](const NodeIdentity& a, const ComputedStyle& b) {
                                    return a == b.identity;
                                })) {
                    out.children_stamp = stamp;
                }
                if (memoize) {
                    selector_path.resize(selector_lengths.back());
                    selector_lengths.pop_back();
                }
                path.pop_back();
            }

            // A subtree kept as it was: what it charged against the bounds when it was computed.
            void keep(const ComputedStyle& subtree) {
                nodes += subtree.subtree_nodes;
                if (nodes > constants::max_document_nodes) {
                    fail("node/depth bound");
                }
                charge(subtree.subtree_work);
                bytes += subtree.subtree_bytes;
                if (bytes > constants::style::max_computed_output_bytes) {
                    fail("computed output byte bound");
                }
            }

            // Makes node's environment the custom properties its children inherit (the parent's own when they are
            // the same); whether they changed.
            static bool inherit(ComputedStyle& node, const Variables& variables, const ComputedStyle* parent) {
                if (node.environment && node.environment->variables == variables) {
                    return false;
                }
                if (parent && parent->environment && parent->environment->variables == variables) {
                    node.environment = parent->environment;
                } else {
                    node.environment = std::make_shared<const StyleEnvironment>(StyleEnvironment{variables});
                }
                return true;
            }

            // Records the bounds a node and its subtree charged, and its subtree's latest stamp.
            void close(ComputedStyle& node, std::size_t nodes_before, std::size_t work_before,
                       std::size_t bytes_before) {
                node.subtree_nodes = nodes - nodes_before;
                node.subtree_work = work - work_before;
                node.subtree_bytes = bytes - bytes_before;
                node.subtree_stamp = std::max(node.stamp, node.children_stamp);
                for (const auto& c : node.children) {
                    node.subtree_stamp = std::max(node.subtree_stamp, c.subtree_stamp);
                }
                for (const auto& g : node.generated) {
                    node.subtree_stamp = std::max(node.subtree_stamp, g.subtree_stamp);
                }
            }

            // The ::before and ::after of the element atop path (CSS Pseudo-Elements 4 §2): each a child of the element
            // computed from the rules whose subject is that pseudo-element of it (it inherits from the element and the
            // custom properties its children inherit, variables), when a rule that declares content gives it; a box
            // only when its content computes to other than normal and none. Each starts from the previous one of its
            // kind, so what did not change keeps its stamp; another list of boxes is a change of the element's
            // children.
            void generate(const DomSnapshot& dom, ComputedStyle& destination, const Variables& variables) {
                auto previous = std::move(destination.generated);
                destination.generated.clear();
                std::vector<NodeIdentity> before;
                for (const auto& g : previous) {
                    before.push_back(g.identity);
                }
                // A generated box generates none (*::after does not give ::after::after).
                if (generated_kind(dom) != dom::PseudoElement::None) {
                    return;
                }
                const PathElement element{&path, path.size() - 1};
                for (const auto box : {GeneratedBox::Before, GeneratedBox::After}) {
                    const auto kind =
                        box == GeneratedBox::Before ? dom::PseudoElement::Before : dom::PseudoElement::After;
                    const bool given = std::any_of(content_rules.begin(), content_rules.end(), [&](const Rule* r) {
                        return r->selector.pseudo() == kind &&
                               !(r->media && (!forgiving || !r->media->holds(forgiving->width, forgiving->height,
                                                                             forgiving->devices))) &&
                               dom::matches_originating(r->selector, element);
                    });
                    if (!given) {
                        continue;
                    }
                    DomSnapshot stand_in;
                    stand_in.identity = generated_identity(dom.identity, box);
                    stand_in.type = constants::element_node_type;
                    stand_in.local_name = box == GeneratedBox::Before ? "::before" : "::after";
                    stand_in.namespace_uri = constants::xhtml_namespace_uri;
                    ComputedStyle style;
                    for (auto& g : previous) {
                        if (g.identity == stand_in.identity) {
                            style = std::move(g);
                            break;
                        }
                    }
                    compute_into(stand_in, style, &destination, variables);
                    const auto* content = style.values.find(PropertyId::Content);
                    const auto text = content ? trim(content->text()) : std::string("normal");
                    if (text == "normal" || text == "none") {
                        continue;
                    }
                    destination.generated.push_back(std::move(style));
                }
                // A text field's ::placeholder and ::selection: the styles its placeholder and selected text take from
                // the rules whose subject they are (local names "::placeholder" and "::selection"), not boxes.
                if (dom.field) {
                    for (const bool selection : {false, true}) {
                        if (!(selection ? selection_rules : placeholder_rules)) {
                            continue;
                        }
                        DomSnapshot stand_in;
                        stand_in.identity = field_part_identity(dom.identity, selection);
                        stand_in.type = constants::element_node_type;
                        stand_in.local_name = selection ? "::selection" : "::placeholder";
                        stand_in.namespace_uri = constants::xhtml_namespace_uri;
                        ComputedStyle style;
                        for (auto& g : previous) {
                            if (g.identity == stand_in.identity) {
                                style = std::move(g);
                                break;
                            }
                        }
                        compute_into(stand_in, style, &destination, variables);
                        destination.generated.push_back(std::move(style));
                    }
                }
                if (destination.generated.size() != before.size() ||
                    !std::equal(before.begin(), before.end(), destination.generated.begin(),
                                [](const NodeIdentity& a, const ComputedStyle& b) {
                                    return a == b.identity;
                                })) {
                    destination.children_stamp = stamp;
                }
            }

            ComputedStyle compute(const DomSnapshot& dom, const ComputedStyle* parent, const Variables& inherited) {
                ComputedStyle out;
                compute_into(dom, out, parent, inherited);
                return out;
            }

            // dom's values into destination, then its children's. With old, the copy of dom destination was last
            // computed from: dom keeps its own values unless `dirty` (its parent's values changed), `selectors`
            // (what selectors read of an ancestor changed) or its own input changed, and its children follow
            // (children_into).
            void compute_into(const DomSnapshot& dom, ComputedStyle& destination, const ComputedStyle* parent,
                              const Variables& inherited, const DomSnapshot* old = nullptr, bool dirty = true,
                              bool selectors_dirty = true) {
                const auto nodes_before = nodes;
                if (path.size() >= constants::max_document_depth || ++nodes > constants::max_document_nodes) {
                    fail("node/depth bound");
                }
                if (dom.type != constants::element_node_type ||
                    (dom.namespace_uri != constants::xhtml_namespace_uri &&
                     !(profile.scene_syntax && dom.namespace_uri == constants::svg_namespace_uri))) {
                    fail("HTML/scene SVG element snapshot required");
                }
                path.push_back(&dom);
                const bool root = at_root(dom);
                const auto work_before = work;
                const auto bytes_before = bytes;
                auto selectors = memoize ? selector_key() : std::string{};
                // Only a descendant changed: the own values stand, as do the custom properties the children inherit.
                if (old && !dirty && !selectors_dirty && destination.identity == dom.identity &&
                    destination.environment && same_own_input(dom, *old, root)) {
                    charge(destination.own_work);
                    bytes += destination.own_bytes;
                    if (bytes > constants::style::max_computed_output_bytes) {
                        fail("computed output byte bound");
                    }
                    const auto environment = destination.environment;
                    for (const auto& g : destination.generated) {
                        keep(g);
                    }
                    children_into(dom, destination, environment->variables, old, false, false);
                    close(destination, nodes_before, work_before, bytes_before);
                    return;
                }
                // Only the inline style changed, in values the children do not inherit: those values alone.
                if (old && !dirty && !selectors_dirty && destination.identity == dom.identity &&
                    destination.environment) {
                    if (const auto restyled = restyle_inline(dom, *old, destination, selectors, root)) {
                        destination.own_work = work - work_before;
                        destination.own_bytes = bytes - bytes_before;
                        const auto environment = destination.environment;
                        generate(dom, destination, environment->variables);
                        children_into(dom, destination, environment->variables, old,
                                      *restyled && children_read_all(dom), false);
                        close(destination, nodes_before, work_before, bytes_before);
                        return;
                    }
                }
                // What the children's selectors read of dom changed, so may what they match.
                const bool child_selectors = selectors_dirty || !old || !same_selector_input(dom, *old, root);
                auto key = memoize ? memo_key(parent, inherited, selectors) : std::string{};
                if (memoize) {
                    if (auto it = memo_state->entries.find(key); it != memo_state->entries.end()) {
                        touch(memo_state->entry_uses, it, memo_state->clock);
                        const auto variables = it->second.variables;
                        const auto token_work = it->second.token_work;
                        const auto output_bytes = it->second.output_bytes;
                        charge(token_work);
                        bytes += output_bytes;
                        if (bytes > constants::style::max_computed_output_bytes) {
                            fail("computed output byte bound");
                        }
                        const bool reaches = reaches_children(dom, destination, it->second.own);
                        const bool changed = own_into(destination, it->second.own, dom.identity);
                        destination.own_work = token_work;
                        destination.own_bytes = output_bytes;
                        const bool inherited_changed = inherit(destination, variables, parent);
                        generate(dom, destination, variables);
                        children_into(dom, destination, variables, old, (changed && reaches) || inherited_changed,
                                      child_selectors);
                        close(destination, nodes_before, work_before, bytes_before);
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
                const auto apply_rule = [&](const Rule& r, unsigned origin) {
                    if (r.media &&
                        (!forgiving || !r.media->holds(forgiving->width, forgiving->height, forgiving->devices))) {
                        return;
                    }
                    if (!r.matches(path)) {
                        return;
                    }
                    const auto& specificity = r.selector.specificity;
                    for (const auto& d : r.declarations) {
                        // Cascade layers: an earlier layer loses among normal declarations and wins among important
                        // ones (CSS Cascade 5 6.4).
                        const unsigned layer = d.important ? std::numeric_limits<unsigned>::max() - r.layer : r.layer;
                        apply(d, {d.important ? 1u : 0u, d.important && origin == 0 ? 2u : origin, 0, layer,
                                  specificity[0], specificity[1], specificity[2], 0});
                    }
                };
                auto sheet = [&](const std::vector<Rule>& list, unsigned origin, const RuleIndex* index) {
                    if (!index) {
                        for (const auto& r : list) {
                            apply_rule(r, origin);
                        }
                        return;
                    }
                    index->candidates(generated_kind(dom) != dom::PseudoElement::None && path.size() >= 2
                                          ? *path[path.size() - 2]
                                          : dom,
                                      candidates);
                    for (const auto i : candidates) {
                        apply_rule(list[i], origin);
                    }
                };
                // Stable rule matching is shared by all inline animation samples.
                // Presentation declarations and inline declarations remain evaluated
                // in their original cascade positions and do not enter this cache.
                bool winner_hit{};
                if (memoize) {
                    if (auto it = memo_state->winners.find(selectors); it != memo_state->winners.end()) {
                        touch(memo_state->winner_uses, it, memo_state->clock);
                        winning = it->second.declarations;
                        ranks = it->second.ranks;
                        order = it->second.order;
                        winner_hit = true;
                    }
                }
                if (!winner_hit) {
                    sheet(ua, 0, nullptr);
                    sheet(author, 1, forgiving ? forgiving->index : nullptr);
                    if (memoize) {
                        std::size_t payload = selectors.size();
                        for (const auto& [k, d] : winning) {
                            payload += k.size() + serialize(d.tokens).size() + sizeof(Declaration) + sizeof(Rank);
                        }
                        if (payload <= constants::style::memo_max_bytes) {
                            while (!memo_state->winners.empty() &&
                                   (memo_state->winners.size() >= constants::style::memo_max_entries ||
                                    memo_state->winner_bytes + payload > constants::style::memo_max_bytes)) {
                                const auto victim = memo_state->winner_uses.begin()->second;
                                memo_state->winner_uses.erase(memo_state->winner_uses.begin());
                                memo_state->winner_bytes -= victim->second.payload;
                                memo_state->winners.erase(victim);
                            }
                            memo_state->winner_bytes += payload;
                            const auto [entry, added] = memo_state->winners.emplace(
                                selectors, Winners{winning, ranks, order, payload, ++memo_state->clock});
                            if (added) {
                                memo_state->winner_uses.emplace(entry->second.used, entry);
                            }
                        }
                    }
                }
                // dir=auto: its direction as a presentational hint (below any author rule, as Chrome's).
                if (profile.scene_syntax && profile.properties.contains("direction")) {
                    if (const auto direction = auto_direction(dom)) {
                        for (const auto& d :
                             declarations("direction:" + std::string(*direction), profile, warnings())) {
                            auto rank = Rank{0, 1, 0, 0, 0, 0, 0, 0};
                            if (!ranks.contains(d.key) || ranks[d.key] < rank) {
                                ranks[d.key] = rank;
                                winning[d.key] = d;
                            }
                        }
                    }
                }
                // An img's width and height attributes as HTML maps them: the width and height properties (a number
                // of px or a percentage), and with both numbers, aspect-ratio auto w / h (its ratio until an image
                // gives its own), below any author rule.
                if (profile.scene_syntax && dom.namespace_uri == constants::xhtml_namespace_uri &&
                    dom.local_name == "img") {
                    const auto hint = [&](const std::string& text) {
                        for (const auto& d : declarations(text, profile, warnings())) {
                            auto rank = Rank{0, 1, 0, 0, 0, 0, 0, 0};
                            if (!ranks.contains(d.key) || ranks[d.key] < rank) {
                                ranks[d.key] = rank;
                                winning[d.key] = d;
                            }
                        }
                    };
                    std::array<std::optional<std::string>, 2> sizes;
                    for (std::size_t k = 0; k < 2; ++k) {
                        if (const auto it = dom.attributes.find(k ? "height" : "width"); it != dom.attributes.end()) {
                            if ((sizes[k] = html_dimension(it->second))) {
                                hint(std::string(k ? "height:" : "width:") + *sizes[k]);
                            }
                        }
                    }
                    if (sizes[0] && sizes[1] && sizes[0]->ends_with("px") && sizes[1]->ends_with("px") &&
                        profile.properties.contains("aspect-ratio")) {
                        hint("aspect-ratio:auto " + sizes[0]->substr(0, sizes[0]->size() - 2) + " / " +
                             sizes[1]->substr(0, sizes[1]->size() - 2));
                    }
                }
                if (profile.scene_syntax && dom.namespace_uri == constants::svg_namespace_uri) {
                    for (const auto k : presentation_attributes) {
                        if (const auto it = dom.attributes.find(k); it != dom.attributes.end()) {
                            for (const auto& d : declarations(std::string(k) + ":" + it->second, profile, warnings())) {
                                auto rank = Rank{0, 1, 0, 0, 0, 0, 0, 0};
                                if (!ranks.contains(d.key) || ranks[d.key] < rank) {
                                    ranks[d.key] = rank;
                                    winning[d.key] = d;
                                }
                            }
                        }
                    }
                }
                if (dom.attributes.contains("style")) {
                    for (const auto& d : declarations(comments(dom.attributes.at("style"), profile.scene_syntax),
                                                      profile, warnings())) {
                        apply(d, {d.important ? 1u : 0u, 1, 1, 0, 0, 0, 0, 0});
                    }
                }
                // Charge inherited token copies before allocating a child environment.
                // Input dimensions alone do not bound nested/exponential token copying.
                for (const auto& [k, t] : inherited) {
                    if (t) {
                        charge(token_count(*t));
                    }
                }
                Variables variables = inheritable(inherited);
                std::map<std::string, Tokens> local;
                for (const auto& [k, d] : winning) {
                    if (custom(k)) {
                        const auto word = keyword(d.tokens);
                        const auto* r = registered(k);
                        if (word == "revert" || word == "revert-layer") {
                            fail("cascade rollback outside profile");
                        }
                        if (word == "initial" || (word == "unset" && r && !r->inherits)) {
                            // A registered property's initial value (none: guaranteed-invalid).
                            variables[k] = r ? r->initial : std::nullopt;
                        } else if (word == "inherit" || word == "unset") {
                            if (!variables.contains(k)) {
                                variables[k] = inherited.contains(k) ? inherited.at(k) : std::nullopt;
                            }
                        } else {
                            local[k] = d.tokens;
                        }
                    }
                }
                const auto custom_bound =
                    std::size_t(profile.forgiving ? constants::style::max_forgiving_custom_properties
                                                  : constants::style::max_custom_properties);
                std::set<std::string> variable_names;
                for (const auto& [k, t] : variables) {
                    variable_names.insert(k);
                }
                for (const auto& [k, t] : local) {
                    variable_names.insert(k);
                }
                if (variable_names.size() > custom_bound) {
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
                    if (const auto it = variables.find(k); it != variables.end()) {
                        return it->second;
                    }
                    // A registered property no element set: its initial value.
                    const auto* r = registered(k);
                    return r ? r->initial : std::nullopt;
                };
                for (const auto& [k, t] : local) {
                    resolve(k);
                }
                if (variables.size() > custom_bound) {
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
                const auto compute_property = [&](const std::string& k, const Declaration& d) {
                    {
                        const auto& prop = profile.properties.at(k);
                        auto tokens = substitute(d.tokens, resolve);
                        if (tokens && d.box_edge >= 0 && !wide(keyword(*tokens))) {
                            if (tokens->empty() || tokens->size() > 4) {
                                fail("box shorthand needs one to four values");
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
                                // A circular corner is one radius (the scene draws one value a corner).
                                if (profile.forgiving && (*tokens)[0] == (*tokens)[1]) {
                                    tokens->pop_back();
                                }
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
                            // The part of the layer this longhand takes (background_layer.hpp), or its initial value.
                            std::vector<std::string> names;
                            names.reserve(tokens->size());
                            std::vector<BackgroundComponent> components;
                            for (const auto& t : *tokens) {
                                using Part = BackgroundComponent::Kind;
                                names.push_back(lower(t.text));
                                const auto kind =
                                    t.kind == Kind::Ident                    ? Part::Ident
                                    : t.kind == Kind::Function               ? Part::Function
                                    : t.kind == Kind::Hash                   ? Part::Hash
                                    : t.kind == Kind::Delim && t.text == "/" ? Part::Slash
                                    : t.kind == Kind::Number || t.kind == Kind::Dimension || t.kind == Kind::Percentage
                                        ? Part::Length
                                        : Part::Other;
                                components.push_back({kind, names.back()});
                            }
                            const auto parts = background_parts(components);
                            if (!parts) {
                                fail("background shorthand outside one layer: " + serialize(*tokens));
                            }
                            const auto& longhand = background_longhands[std::size_t(d.background_layer)];
                            Tokens taken;
                            for (std::size_t i = 0; i < parts->size(); ++i) {
                                const auto part = (*parts)[i];
                                if (part == longhand.part || (part == BackgroundPart::OriginAndClip &&
                                                              (longhand.part == BackgroundPart::Origin ||
                                                               longhand.part == BackgroundPart::Clip))) {
                                    taken.push_back((*tokens)[i]);
                                }
                            }
                            if (taken.empty()) {
                                for (const auto& word : css::split(longhand.initial, ' ').parts) {
                                    taken.push_back(word.ends_with('%') ? Token{Kind::Percentage, word, {}}
                                                                        : Token{Kind::Ident, word, {}});
                                }
                            }
                            tokens = std::move(taken);
                        }
                        if (tokens && d.transition_part >= 0 && !wide(keyword(*tokens))) {
                            // A var() that substituted no transition list makes the longhands invalid at computed
                            // value time.
                            auto parts = transition_parts(*tokens);
                            if (parts.error.empty()) {
                                tokens = std::move(parts.lists[std::size_t(d.transition_part)]);
                            } else {
                                tokens = std::nullopt;
                            }
                        }
                        std::string word = tokens ? keyword(*tokens) : "";
                        if (profile.forgiving && tokens && !wide(word)) {
                            normalize(k, *tokens, out.values, parent ? &parent->values : nullptr);
                        }
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
                        // A forgiving profile's initial value left empty is the consumer's default: no value.
                        const auto set_initial = [&] {
                            const auto& initial = profile_index.initial[std::size_t(d.id)];
                            if (profile.forgiving && initial.text().empty()) {
                                out.values.erase(d.id);
                            } else {
                                out.values.set(d.id, initial);
                            }
                        };
                        if (word == "inherit" || (word == "unset" && prop.inherited)) {
                            const auto* inherited_value = parent ? parent->values.find(d.id) : nullptr;
                            if (inherited_value) {
                                out.values.set(d.id, *inherited_value);
                            } else {
                                set_initial();
                            }
                        } else if (word == "initial" || word == "unset") {
                            set_initial();
                        } else {
                            out.values.set(d.id, CssValue(serialize(*tokens)));
                        }
                    }
                };
                const auto run = [&](const std::string& k, const Declaration& d) {
                    if (!profile.forgiving) {
                        compute_property(k, d);
                        return;
                    }
                    try {
                        compute_property(k, d);
                    } catch (const std::invalid_argument& e) {
                        if (bounds_passed()) {
                            throw;
                        }
                        // Invalid at computed-value time: the property is unset, and the page is told.
                        warn(std::string(e.what()) + " (" + k + ")");
                        out.invalid_at_computed_value.push_back(k);
                        const auto& prop = profile.properties.at(k);
                        const auto* inherited_value = prop.inherited && parent ? parent->values.find(d.id) : nullptr;
                        if (inherited_value) {
                            out.values.set(d.id, *inherited_value);
                        } else if (profile_index.initial[std::size_t(d.id)].text().empty()) {
                            out.values.erase(d.id);
                        } else {
                            out.values.set(d.id, profile_index.initial[std::size_t(d.id)]);
                        }
                    }
                };
                // color first: the other properties' currentcolor in a color-mix() is this element's.
                if (const auto color = winning.find("color"); color != winning.end()) {
                    run(color->first, color->second);
                }
                for (const auto& [k, d] : winning) {
                    if (!custom(k) && k != "color") {
                        run(k, d);
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
                // What the children inherit: all but the registered properties that do not inherit.
                const Variables child_variables = inheritable(variables);
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
                            const auto victim = memo_state->entry_uses.begin()->second;
                            memo_state->entry_uses.erase(memo_state->entry_uses.begin());
                            memo_state->bytes -= victim->second.payload_bytes;
                            memo_state->entries.erase(victim);
                        }
                        const auto [entry, added] = memo_state->entries.emplace(
                            std::move(key),
                            Memo{out, child_variables, work - work_before, output_bytes, payload, ++memo_state->clock});
                        if (added) {
                            memo_state->entry_uses.emplace(entry->second.used, entry);
                        }
                        memo_state->bytes += payload;
                    }
                }
                const bool reaches = reaches_children(dom, destination, out);
                const bool changed = own_into(destination, out, dom.identity);
                destination.own_work = work - work_before;
                destination.own_bytes = bytes - bytes_before;
                const bool inherited_changed = inherit(destination, child_variables, parent);
                generate(dom, destination, child_variables);
                children_into(dom, destination, child_variables, old, (changed && reaches) || inherited_changed,
                              child_selectors);
                close(destination, nodes_before, work_before, bytes_before);
            }

            // A forgiving profile's computed value of a property: its math functions simplified and its colors in the
            // forms the scene draws. own: the element's values so far (its color, for currentcolor inside a
            // color-mix()); parent: its parent's (the color property's own currentcolor).
            void normalize(const std::string& key, Tokens& tokens, const StyleValues& own, const StyleValues* parent) {
                simplify_math(tokens);
                clamp_numbers(tokens);
                if (!color_bearing(key)) {
                    return;
                }
                std::optional<color::Rgba> current;
                if (const auto* source = key == "color" ? parent : &own) {
                    if (const auto* c = source->find(PropertyId::Color)) {
                        current = parse_computed_color(c->text());
                    }
                }
                if (!current) {
                    current = parse_computed_color(constants::default_color);
                }
                normalize_colors(tokens, current);
            }
        };
    } // namespace

    namespace {
        // A sheet compiled for a profile: its rules, and for a forgiving profile their index, the sheet's registered
        // custom properties, its @keyframes and what it dropped.
        struct Compiled {
            std::vector<Rule> ua;
            std::vector<Rule> author;
            std::unique_ptr<RuleIndex> index;
            std::map<std::string, Registered> registered;
            std::string keyframes;
            std::vector<FontFaceRule> font_faces;
            bool sized_media{};
            Warnings warnings;

            Compiled(std::string sheet, const StyleProfile& profile) {
                // A forgiving profile's UA sheet takes its selectors (:focus-visible) as its author sheet does.
                ua = profile.forgiving ? parse_sheet(profile.ua_stylesheet, profile).rules
                                       : rules(profile.ua_stylesheet, profile);
                if (!profile.forgiving) {
                    author = rules(std::move(sheet), profile);
                    return;
                }
                auto parsed = parse_sheet(std::move(sheet), profile);
                author = std::move(parsed.rules);
                registered = std::move(parsed.registered);
                keyframes = std::move(parsed.keyframes);
                font_faces = std::move(parsed.font_faces);
                sized_media = parsed.sized_media;
                warnings = std::move(parsed.warnings);
                index = std::make_unique<RuleIndex>(author);
            }
        };
    } // namespace

    ComputedStyle compute_style(const DomSnapshot& root, std::string_view sheet, const StyleProfile& profile) {
        return compute_style(root, sheet, profile, 0, 0);
    }

    ComputedStyle compute_style(const DomSnapshot& root, std::string_view sheet, const StyleProfile& profile,
                                float viewport_width, float viewport_height) {
        if (profile.properties.size() > constants::style::max_registered_properties) {
            fail("property registry count bound");
        }
        Compiled compiled(std::string(sheet), profile);
        const ProfileIndex index(profile);
        const ForgivingContext context{compiled.index.get(), &compiled.registered, &compiled.warnings, viewport_width,
                                       viewport_height};
        Engine engine{profile, index,   compiled.ua, compiled.author,
                      false,   nullptr, nullptr,     profile.forgiving ? &context : nullptr};
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
        std::unique_ptr<Compiled> compiled;
        std::vector<Rule>& ua;
        std::vector<Rule>& author;
        std::set<std::string> attributes{"id", "class", "style"};
        std::vector<dom::Compound> state_parts;
        // What the sheet's selectors read beyond names, attributes, hover and active: the focused element (:focus,
        // :focus-visible), the focused element's ancestors (:focus-within), emptiness.
        bool uses_focus{};
        bool uses_focus_within{};
        bool uses_empty{};
        // Text fields: selectors read their state (:read-write, :placeholder-shown), rules style their pseudo-elements.
        bool uses_field{};
        bool field_parts{};
        // A forgiving sheet's cascade context, the viewport its @media conditions read, and their outcomes there.
        ForgivingContext context;
        std::vector<bool> media_outcomes;

        static Compiled& build(std::unique_ptr<Compiled>& out, std::string sheet, const StyleProfile& profile) {
            if (profile.properties.size() > constants::style::max_registered_properties) {
                fail("property registry count bound");
            }
            out = std::make_unique<Compiled>(std::move(sheet), profile);
            return *out;
        }

        Impl(std::string sheet, StyleProfile p)
            : profile(std::move(p)), ua(build(compiled, std::move(sheet), profile).ua), author(compiled->author) {
            index = ProfileIndex(profile);
            context = {compiled->index.get(), &compiled->registered, &compiled->warnings, 0, 0};
            for (const auto* list : {&ua, &author}) {
                for (const auto& rule : *list) {
                    each_compound(rule.selector, [&](const dom::Compound& part) {
                        if (part.hover || part.active) {
                            state_parts.push_back(lone_state_part(part));
                        }
                        uses_focus |= part.focus || part.focus_visible;
                        uses_focus_within |= part.focus_within;
                        uses_empty |= part.empty;
                        uses_field |= part.read_only || part.read_write || part.placeholder_shown;
                        for (const auto& attribute : part.attributes) {
                            attributes.insert(attribute.name);
                            attributes.insert(attribute.lower_name);
                        }
                        if (part.disabled || part.enabled || part.checked) {
                            attributes.insert("disabled");
                            attributes.insert("checked");
                            attributes.insert("selected");
                        }
                    });
                    const auto pseudo = rule.selector.pseudo();
                    field_parts |= pseudo == dom::PseudoElement::Placeholder || pseudo == dom::PseudoElement::Selection;
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

    std::vector<std::string> CompiledStyle::take_warnings() const {
        auto& warnings = impl_->compiled->warnings;
        auto out = std::move(warnings.list);
        warnings.list.clear();
        return out;
    }

    const std::string& CompiledStyle::keyframes() const {
        return impl_->compiled->keyframes;
    }

    const std::vector<FontFaceRule>& CompiledStyle::font_faces() const {
        return impl_->compiled->font_faces;
    }

    bool CompiledStyle::set_viewport(float width, float height, const InputDevices& devices) {
        auto& i = *impl_;
        i.context.width = width;
        i.context.height = height;
        i.context.devices = devices;
        if (!i.profile.forgiving || !i.compiled->sized_media) {
            return false;
        }
        std::vector<bool> outcomes;
        for (const auto& rule : i.author) {
            if (rule.media && rule.media->sized()) {
                outcomes.push_back(rule.media->holds(width, height, devices));
            }
        }
        if (outcomes == i.media_outcomes) {
            return false;
        }
        const bool first = i.media_outcomes.empty();
        i.media_outcomes = std::move(outcomes);
        // Other rules match now: what the memos and style inputs hold is of the other outcomes.
        i.identity = std::make_shared<const int>(0);
        return !first;
    }

    ComputedStyle CompiledStyle::compute(const DomSnapshot& dom) const {
        Engine engine{impl_->profile, impl_->index, impl_->ua, impl_->author,
                      true,           nullptr,      nullptr,   impl_->profile.forgiving ? &impl_->context : nullptr};
        return engine.compute(dom, nullptr, {});
    }

    ComputedStyle CompiledStyle::compute(const DomSnapshot& dom, StyleMemo& memo) const {
        auto& cache = *memo.impl_;
        cache.owner.require(Diag::StyleMemoOwner);
        if (cache.binding != impl_->identity) {
            cache.values = {};
            cache.binding = impl_->identity;
        }
        Engine engine{impl_->profile, impl_->index,  impl_->ua, impl_->author,
                      true,           &cache.values, nullptr,   impl_->profile.forgiving ? &impl_->context : nullptr};
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
        Engine engine{impl_->profile, impl_->index,  impl_->ua, impl_->author,
                      true,           &cache.values, changed,   impl_->profile.forgiving ? &impl_->context : nullptr};
        engine.compute_into(dom, output, nullptr, {});
    }

    void CompiledStyle::update(const DomSnapshot& dom, const DomSnapshot* previous, ComputedStyle& output,
                               StyleMemo& memo, std::uint64_t stamp, std::vector<NodeIdentity>* changed) const {
        auto& cache = *memo.impl_;
        cache.owner.require(Diag::StyleMemoOwner);
        if (cache.binding != impl_->identity) {
            cache.values = {};
            cache.binding = impl_->identity;
        }
        if (changed) {
            changed->clear();
        }
        Engine engine{impl_->profile, impl_->index,  impl_->ua, impl_->author,
                      true,           &cache.values, changed,   impl_->profile.forgiving ? &impl_->context : nullptr};
        engine.stamp = stamp;
        // The root's context is the document's: it changes only with the root itself.
        engine.compute_into(dom, output, nullptr, {},
                            previous && previous->identity == dom.identity ? previous : nullptr, false, false);
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
            return seen_state(impl_->state_parts, n, depth == 0 && n.local_name == "html");
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
            assign(k.focused, impl_->uses_focus && n.focused);
            assign(k.focus_visible, impl_->uses_focus && n.focus_visible);
            assign(k.focus_within, impl_->uses_focus_within && n.focus_within);
            assign(k.content, impl_->uses_empty && has_content(n));
            const auto [writable, placeholder] = field_flags(n);
            assign(k.field, (impl_->uses_field || impl_->field_parts) && n.field != nullptr);
            assign(k.writable, impl_->uses_field && writable);
            assign(k.placeholder, impl_->uses_field && placeholder);
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
            const auto [h, a] = seen_state(impl_->state_parts, n, depth == 0 && n.local_name == "html");
            n.hovered = h;
            n.active = a;
            n.focused &= impl_->uses_focus;
            n.focus_visible &= impl_->uses_focus;
            n.focus_within &= impl_->uses_focus_within;
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

    std::string simplify_math_text(std::string_view value) {
        if (value.find('(') == std::string_view::npos) {
            return std::string(value);
        }
        try {
            Lexer lexer{value};
            lexer.forgiving = true;
            auto tokens = lexer.scan();
            simplify_math(tokens);
            return serialize(tokens);
        } catch (const std::invalid_argument&) {
            return std::string(value);
        }
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
