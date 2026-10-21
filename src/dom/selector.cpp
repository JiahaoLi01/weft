#include "dom/selector.hpp"

#include "constants/dom.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace weft::dom {
    namespace {
        bool space(char c) {
            return std::isspace(static_cast<unsigned char>(c)) != 0;
        }

        // ASCII letters, digits, hyphen, underscore and every non-ASCII byte (CSS name code points in UTF-8).
        bool name_char(char c) {
            const auto u = static_cast<unsigned char>(c);
            return std::isalnum(u) || c == '-' || c == '_' || u >= constants::dom::ascii_end;
        }

        std::string lower(std::string s) {
            for (auto& c : s) {
                c = char(std::tolower(static_cast<unsigned char>(c)));
            }
            return s;
        }

        // Pseudo-classes of states and parts a weft document does not have: valid in the modern grammar, matching
        // nothing.
        bool absent_pseudo_class(std::string_view name) {
            for (const auto* k : {"link",
                                  "visited",
                                  "any-link",
                                  "local-link",
                                  "target",
                                  "target-within",
                                  "placeholder-shown",
                                  "autofill",
                                  "invalid",
                                  "valid",
                                  "user-invalid",
                                  "user-valid",
                                  "required",
                                  "optional",
                                  "default",
                                  "indeterminate",
                                  "in-range",
                                  "out-of-range",
                                  "read-write",
                                  "popover-open",
                                  "modal",
                                  "fullscreen",
                                  "picture-in-picture",
                                  "host",
                                  "open",
                                  "closed",
                                  "playing",
                                  "paused",
                                  "current",
                                  "past",
                                  "future",
                                  "blank",
                                  "user-error",
                                  "focus-ring"}) {
                if (name == k) {
                    return true;
                }
            }
            return name.starts_with("-webkit-") || name.starts_with("-moz-") || name.starts_with("-ms-");
        }

        // Pseudo-elements a weft document does not have: valid in the modern grammar, matching nothing.
        bool absent_pseudo_element(std::string_view name) {
            for (const auto* k : {"placeholder", "selection", "backdrop", "marker", "file-selector-button",
                                  "first-line", "first-letter", "cue", "grammar-error", "spelling-error", "target-text",
                                  "highlight", "part", "slotted", "view-transition"}) {
                if (name == k) {
                    return true;
                }
            }
            return name.starts_with("-webkit-") || name.starts_with("-moz-") || name.starts_with("-ms-");
        }

        struct Parser {
            std::string_view text;
            const SelectorGrammar& grammar;
            std::size_t p{};
            std::optional<Diagnostic> error;

            bool fail(Diag code, std::string_view detail) {
                if (!error) {
                    error = make_diagnostic(code, detail);
                }
                return false;
            }

            // Text that is not a selector.
            bool syntax(std::string_view detail) {
                return fail(grammar.syntax, detail);
            }

            // A valid selector this grammar does not support.
            bool unsupported(std::string_view detail) {
                return fail(grammar.unsupported, detail);
            }

            void spaces() {
                while (p < text.size() && space(text[p])) {
                    ++p;
                }
            }

            // A CSS identifier: an unescaped digit, or a hyphen and a digit, cannot begin one.
            bool name(std::string& out) {
                out.clear();
                const auto digit = [&](std::size_t i) {
                    return i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]));
                };
                if (digit(p) || (p < text.size() && text[p] == '-' && digit(p + 1))) {
                    return syntax("selector identifier");
                }
                while (p < text.size()) {
                    if (name_char(text[p])) {
                        out += text[p++];
                    } else if (grammar.escapes && text[p] == '\\') {
                        ++p;
                        if (p == text.size()) {
                            return syntax("incomplete selector escape");
                        }
                        if (std::isxdigit(static_cast<unsigned char>(text[p]))) {
                            unsigned value{};
                            unsigned digits{};
                            while (p < text.size() && digits < constants::dom::max_escape_hex_digits &&
                                   std::isxdigit(static_cast<unsigned char>(text[p]))) {
                                const auto c = std::tolower(static_cast<unsigned char>(text[p++]));
                                value = value * 16 + unsigned(c <= '9' ? c - '0' : c - 'a' + 10);
                                ++digits;
                            }
                            if (p < text.size() && space(text[p])) {
                                ++p;
                            }
                            if (value == 0 || value >= unsigned(constants::dom::ascii_end)) {
                                return unsupported("selector escape outside ASCII");
                            }
                            out += char(value);
                        } else {
                            out += text[p++];
                        }
                    } else {
                        break;
                    }
                }
                return !out.empty() || syntax("selector identifier");
            }

            bool attribute(Compound& c) {
                AttributeSelector a;
                spaces();
                if (!name(a.name)) {
                    return false;
                }
                a.lower_name = lower(a.name);
                spaces();
                using Op = AttributeSelector::Operator;
                if (p < text.size() && text[p] != ']') {
                    const char first = text[p];
                    if (first == '=') {
                        a.op = Op::Equals;
                        ++p;
                    } else if (p + 1 < text.size() && text[p + 1] == '=' &&
                               std::string_view("~|^$*").find(first) != std::string_view::npos) {
                        a.op = first == '~'   ? Op::Includes
                               : first == '|' ? Op::DashMatch
                               : first == '^' ? Op::Prefix
                               : first == '$' ? Op::Suffix
                                              : Op::Substring;
                        p += 2;
                    } else {
                        return syntax("attribute operator");
                    }
                    spaces();
                    if (p == text.size()) {
                        return syntax("attribute value");
                    }
                    if (text[p] == '\'' || text[p] == '"') {
                        const auto quote = text[p++];
                        const auto start = p;
                        while (p < text.size() && text[p] != quote) {
                            ++p;
                        }
                        if (p == text.size()) {
                            return syntax("attribute string");
                        }
                        a.value = std::string(text.substr(start, p - start));
                        ++p;
                    } else if (!name(a.value)) {
                        return false;
                    }
                    spaces();
                }
                if (p < text.size() && a.op != Op::Exists && std::isalpha(static_cast<unsigned char>(text[p]))) {
                    if (!grammar.modern) {
                        return unsupported("attribute flags outside profile");
                    }
                    const char flag = char(std::tolower(static_cast<unsigned char>(text[p++])));
                    if (flag != 'i' && flag != 's') {
                        return syntax("attribute flag");
                    }
                    a.ignore_case = flag == 'i';
                    spaces();
                }
                if (p == text.size() || text[p++] != ']') {
                    return syntax("unterminated attribute selector");
                }
                c.attributes.push_back(std::move(a));
                return true;
            }

            // The text of a functional pseudo-class's argument, up to its closing parenthesis (consumed); strings and
            // nested parentheses stay inside.
            bool argument(std::string_view& out) {
                const auto start = p;
                int depth = 1;
                char quote{};
                for (; p < text.size(); ++p) {
                    const char c = text[p];
                    if (quote) {
                        if (c == '\\' && p + 1 < text.size()) {
                            ++p;
                        } else if (c == quote) {
                            quote = 0;
                        }
                    } else if (c == '"' || c == '\'') {
                        quote = c;
                    } else if (c == '\\' && p + 1 < text.size()) {
                        ++p;
                    } else if (c == '(') {
                        ++depth;
                    } else if (c == ')' && --depth == 0) {
                        out = text.substr(start, p - start);
                        ++p;
                        return true;
                    }
                }
                return syntax("unterminated pseudo-class argument");
            }

            // A selector list of a functional pseudo-class. :is() and :where() forgive (an item outside the grammar is
            // left out), :not() does not.
            bool nested_list(std::string_view argument, bool forgiving, std::vector<Selector>& out) {
                for (const auto& item : split_list(argument)) {
                    auto s = parse_selector(item, grammar);
                    if (!s) {
                        if (forgiving) {
                            continue;
                        }
                        error = s.error();
                        return false;
                    }
                    if (s->pseudo() != PseudoElement::None) {
                        if (forgiving) {
                            continue;
                        }
                        return unsupported("pseudo-element in a selector argument");
                    }
                    out.push_back(std::move(*s));
                }
                return true;
            }

            // An+B (CSS Syntax 3 §6): odd, even, an integer, or [+-]?[digits]n[ [+-] digits]?.
            bool an_plus_b(std::string_view s, int& a, int& b) {
                std::string t;
                for (const char c : s) {
                    if (!space(c)) {
                        t += char(std::tolower(static_cast<unsigned char>(c)));
                    }
                }
                if (t == "odd") {
                    a = 2;
                    b = 1;
                    return true;
                }
                if (t == "even") {
                    a = 2;
                    b = 0;
                    return true;
                }
                const auto integer = [](std::string_view digits, int& out) {
                    if (digits.empty() || digits.size() > 9 || !std::all_of(digits.begin(), digits.end(), [](char c) {
                            return std::isdigit(static_cast<unsigned char>(c)) != 0;
                        })) {
                        return false;
                    }
                    out = 0;
                    for (const char c : digits) {
                        out = out * 10 + (c - '0');
                    }
                    return true;
                };
                const auto n = t.find('n');
                if (n == std::string::npos) {
                    a = 0;
                    const bool negative = !t.empty() && t[0] == '-';
                    const auto digits = std::string_view(t).substr(!t.empty() && (t[0] == '-' || t[0] == '+'));
                    if (!integer(digits, b)) {
                        return syntax("An+B");
                    }
                    b = negative ? -b : b;
                    return true;
                }
                const auto coefficient = std::string_view(t).substr(0, n);
                if (coefficient.empty() || coefficient == "+") {
                    a = 1;
                } else if (coefficient == "-") {
                    a = -1;
                } else {
                    const bool negative = coefficient[0] == '-';
                    if (!integer(coefficient.substr(coefficient[0] == '-' || coefficient[0] == '+'), a)) {
                        return syntax("An+B");
                    }
                    a = negative ? -a : a;
                }
                const auto rest = std::string_view(t).substr(n + 1);
                if (rest.empty()) {
                    b = 0;
                    return true;
                }
                if (rest[0] != '+' && rest[0] != '-') {
                    return syntax("An+B");
                }
                if (!integer(rest.substr(1), b)) {
                    return syntax("An+B");
                }
                b = rest[0] == '-' ? -b : b;
                return true;
            }

            static std::vector<std::string_view> split_list(std::string_view text) {
                std::vector<std::string_view> out;
                std::size_t start{};
                int depth{};
                char quote{};
                for (std::size_t i = 0; i <= text.size(); ++i) {
                    if (i < text.size()) {
                        const char c = text[i];
                        if (quote) {
                            if (c == '\\' && i + 1 < text.size()) {
                                ++i;
                            } else if (c == quote) {
                                quote = 0;
                            }
                            continue;
                        }
                        if (c == '\\' && i + 1 < text.size()) {
                            ++i;
                            continue;
                        }
                        if (c == '"' || c == '\'') {
                            quote = c;
                            continue;
                        }
                        if (c == '(' || c == '[') {
                            ++depth;
                            continue;
                        }
                        if (c == ')' || c == ']') {
                            --depth;
                            continue;
                        }
                    }
                    if (i == text.size() || (text[i] == ',' && depth == 0)) {
                        out.push_back(text.substr(start, i - start));
                        start = i + 1;
                    }
                }
                return out;
            }

            static std::array<unsigned, 3> max_specificity(const std::vector<Selector>& list) {
                std::array<unsigned, 3> best{};
                for (const auto& s : list) {
                    if (s.specificity > best) {
                        best = s.specificity;
                    }
                }
                return best;
            }

            static void add(std::array<unsigned, 3>& to, const std::array<unsigned, 3>& from) {
                for (std::size_t i = 0; i < to.size(); ++i) {
                    to[i] += from[i];
                }
            }

            // A pseudo-class or (after "::", or one of the CSS 2 one-colon spellings) a pseudo-element.
            bool pseudo(Compound& c, Selector& s) {
                const bool element = p < text.size() && text[p] == ':';
                if (element) {
                    ++p;
                    if (!grammar.modern) {
                        return unsupported("pseudo-element outside profile");
                    }
                }
                std::string name_text;
                if (!name(name_text)) {
                    return false;
                }
                const auto pseudo_name = lower(name_text);
                const bool function = p < text.size() && text[p] == '(';
                std::string_view arg;
                if (function) {
                    ++p;
                    if (!argument(arg)) {
                        return false;
                    }
                }
                const bool legacy_element = !element && !function &&
                                            (pseudo_name == "before" || pseudo_name == "after" ||
                                             pseudo_name == "first-line" || pseudo_name == "first-letter");
                if (element || legacy_element) {
                    if (!grammar.modern) {
                        return unsupported("pseudo-element outside profile");
                    }
                    if (c.pseudo != PseudoElement::None) {
                        return syntax("two pseudo-elements");
                    }
                    c.pseudo = pseudo_name == "before" && !function  ? PseudoElement::Before
                               : pseudo_name == "after" && !function ? PseudoElement::After
                               : absent_pseudo_element(pseudo_name)  ? PseudoElement::Other
                                                                     : PseudoElement::None;
                    if (c.pseudo == PseudoElement::None) {
                        return unsupported("pseudo-element outside profile");
                    }
                    ++s.specificity[2];
                    return true;
                }
                if (c.pseudo != PseudoElement::None) {
                    // A pseudo-class after a pseudo-element (::before:hover): an element state of the pseudo-element,
                    // which the scene does not model.
                    if (!grammar.modern) {
                        return unsupported("pseudo-class outside profile");
                    }
                    c.never = true;
                    ++s.specificity[1];
                    return true;
                }
                if (!function && pseudo_name == "root" && !c.root) {
                    c.root = true;
                } else if (!function && grammar.states && pseudo_name == "hover" && !c.hover) {
                    c.hover = true;
                } else if (!function && grammar.states && pseudo_name == "active" && !c.active) {
                    c.active = true;
                } else if (!grammar.modern) {
                    return unsupported("pseudo-class outside profile");
                } else if (function && (pseudo_name == "is" || pseudo_name == "where" || pseudo_name == "not" ||
                                        pseudo_name == "matches" || pseudo_name == "-webkit-any")) {
                    using Kind = SelectorAlternatives::Kind;
                    SelectorAlternatives alternatives;
                    alternatives.kind = pseudo_name == "where" ? Kind::Where
                                        : pseudo_name == "not" ? Kind::Not
                                                               : Kind::Is;
                    if (!nested_list(arg, alternatives.kind != Kind::Not, alternatives.list)) {
                        return false;
                    }
                    if (alternatives.kind != Kind::Where) {
                        add(s.specificity, max_specificity(alternatives.list));
                    }
                    c.alternatives.push_back(std::move(alternatives));
                    return true;
                } else if (function && (pseudo_name == "nth-child" || pseudo_name == "nth-last-child" ||
                                        pseudo_name == "nth-of-type" || pseudo_name == "nth-last-of-type")) {
                    NthSelector n;
                    n.from_end = pseudo_name.find("last") != std::string::npos;
                    n.of_type = pseudo_name.ends_with("of-type");
                    auto formula = arg;
                    if (!n.of_type) {
                        // :nth-child(An+B of S)
                        const auto lowered = lower(std::string(arg));
                        const auto of = lowered.find(" of ");
                        if (of != std::string::npos) {
                            formula = arg.substr(0, of);
                            if (!nested_list(arg.substr(of + 4), false, n.of)) {
                                return false;
                            }
                            add(s.specificity, max_specificity(n.of));
                        }
                    }
                    if (!an_plus_b(formula, n.a, n.b)) {
                        return false;
                    }
                    c.nth.push_back(std::move(n));
                } else if (!function && (pseudo_name == "first-child" || pseudo_name == "last-child" ||
                                         pseudo_name == "first-of-type" || pseudo_name == "last-of-type")) {
                    NthSelector n;
                    n.b = 1;
                    n.from_end = pseudo_name.starts_with("last");
                    n.of_type = pseudo_name.ends_with("of-type");
                    c.nth.push_back(std::move(n));
                } else if (!function && (pseudo_name == "only-child" || pseudo_name == "only-of-type")) {
                    const bool of_type = pseudo_name == "only-of-type";
                    c.nth.push_back({0, 1, false, of_type, {}});
                    c.nth.push_back({0, 1, true, of_type, {}});
                } else if (!function && pseudo_name == "empty") {
                    c.empty = true;
                } else if (!function && pseudo_name == "focus") {
                    c.focus = true;
                } else if (!function && pseudo_name == "focus-visible") {
                    c.focus_visible = true;
                } else if (!function && pseudo_name == "focus-within") {
                    c.focus_within = true;
                } else if (!function && pseudo_name == "disabled") {
                    c.disabled = true;
                } else if (!function && pseudo_name == "enabled") {
                    c.enabled = true;
                } else if (!function && pseudo_name == "checked") {
                    c.checked = true;
                } else if (!function && pseudo_name == "scope") {
                    // In a style sheet, the scoping root is the document's root element.
                    c.root = true;
                } else if (!function && pseudo_name == "defined") {
                    // Every element of a weft document is defined.
                } else if (!function && pseudo_name == "read-only") {
                    // No element of a weft document is editable.
                } else if ((!function && absent_pseudo_class(pseudo_name)) ||
                           (function && (pseudo_name == "host" || pseudo_name == "host-context" ||
                                         pseudo_name.starts_with("-webkit-") || pseudo_name.starts_with("-moz-")))) {
                    c.never = true;
                } else {
                    return unsupported("pseudo-class outside profile");
                }
                ++s.specificity[1];
                return true;
            }

            bool compound(Compound& c, Selector& s) {
                while (p < text.size() && !space(text[p]) && text[p] != '>' && text[p] != '+' && text[p] != '~') {
                    const char prefix = text[p];
                    if (c.pseudo != PseudoElement::None && prefix != ':') {
                        return syntax("a pseudo-element ends its compound");
                    }
                    if (prefix == '.' || prefix == '#') {
                        ++p;
                        std::string value;
                        if (!name(value)) {
                            return false;
                        }
                        (prefix == '.' ? c.classes : c.ids).push_back(std::move(value));
                        ++s.specificity[prefix == '#' ? 0 : 1];
                    } else if (prefix == ':') {
                        ++p;
                        if (!pseudo(c, s)) {
                            return false;
                        }
                    } else if (prefix == '[') {
                        ++p;
                        if (!attribute(c)) {
                            return false;
                        }
                        ++s.specificity[1];
                    } else {
                        if (!c.tag.empty() || !c.ids.empty() || !c.classes.empty() || !c.attributes.empty() || c.root ||
                            c.hover || c.active || c.never || c.focus || c.focus_visible || c.focus_within ||
                            c.disabled || c.enabled ||
                            c.checked || c.empty || !c.nth.empty() || !c.alternatives.empty()) {
                            return syntax("type selector must begin compound");
                        }
                        if (prefix == '*') {
                            c.tag = "*";
                            ++p;
                        } else {
                            if (!name(c.tag)) {
                                return false;
                            }
                            c.lower_tag = lower(c.tag);
                            ++s.specificity[2];
                        }
                    }
                }
                if (c.tag.empty() && c.ids.empty() && c.classes.empty() && c.attributes.empty() && !c.root &&
                    !c.hover && !c.active && !c.never && !c.focus && !c.focus_visible && !c.focus_within &&
                    !c.disabled && !c.enabled &&
                    !c.checked && !c.empty && c.nth.empty() && c.alternatives.empty() &&
                    c.pseudo == PseudoElement::None) {
                    return syntax("empty selector compound");
                }
                return true;
            }

            bool selector(Selector& s) {
                spaces();
                if (p < text.size() && (text[p] == '>' || text[p] == '+' || text[p] == '~')) {
                    return syntax("empty selector compound");
                }
                while (p < text.size()) {
                    Compound c;
                    if (!compound(c, s)) {
                        return false;
                    }
                    const bool pseudo_element = c.pseudo != PseudoElement::None;
                    s.parts.push_back(std::move(c));
                    if (s.parts.size() > std::size_t(constants::max_selector_compounds)) {
                        return unsupported("selector depth bound");
                    }
                    spaces();
                    if (p < text.size()) {
                        if (pseudo_element) {
                            return syntax("a pseudo-element ends its selector");
                        }
                        char relation = ' ';
                        if (text[p] == '>') {
                            relation = '>';
                            ++p;
                            spaces();
                        } else if (text[p] == '+' || text[p] == '~') {
                            if (!grammar.modern) {
                                return unsupported("sibling combinator outside profile");
                            }
                            relation = text[p];
                            ++p;
                            spaces();
                        }
                        if (p == text.size()) {
                            return syntax("selector chain");
                        }
                        s.relations.push_back(relation);
                    }
                }
                if (s.parts.empty() || s.parts.size() != s.relations.size() + 1) {
                    return syntax("selector chain");
                }
                return true;
            }
        };
    } // namespace

    Result<Selector> parse_selector(std::string_view text, const SelectorGrammar& grammar) {
        Parser parser{text, grammar};
        Selector out;
        if (!parser.selector(out)) {
            return *parser.error;
        }
        return out;
    }

    Result<std::vector<Selector>> parse_selector_list(std::string_view text, const SelectorGrammar& grammar,
                                                      std::size_t max_items) {
        std::vector<Selector> out;
        for (const auto item : Parser::split_list(text)) {
            if (out.size() >= max_items) {
                return make_diagnostic(grammar.unsupported, "selector list bound");
            }
            auto parsed = parse_selector(item, grammar);
            if (!parsed) {
                return parsed.error();
            }
            out.push_back(std::move(*parsed));
        }
        return out;
    }

    bool has_class(std::string_view list, std::string_view name) {
        // HTML's ASCII whitespace: space, tab, line feed, form feed and carriage return.
        const auto separator = [](char c) {
            return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
        };
        std::size_t i{};
        while (i < list.size()) {
            while (i < list.size() && separator(list[i])) {
                ++i;
            }
            const auto start = i;
            while (i < list.size() && !separator(list[i])) {
                ++i;
            }
            if (i > start && list.substr(start, i - start) == name) {
                return true;
            }
        }
        return false;
    }

    bool attribute_matches(const AttributeSelector& a, std::string_view value) {
        using Op = AttributeSelector::Operator;
        std::string folded_value;
        std::string folded_wanted;
        std::string_view wanted = a.value;
        if (a.ignore_case) {
            folded_value = lower(std::string(value));
            folded_wanted = lower(a.value);
            value = folded_value;
            wanted = folded_wanted;
        }
        switch (a.op) {
            case Op::Exists:
                return true;
            case Op::Equals:
                return value == wanted;
            case Op::Includes:
                return !wanted.empty() && wanted.find_first_of(" \t\n\f\r") == std::string::npos &&
                       has_class(value, wanted);
            case Op::DashMatch:
                return value == wanted ||
                       (value.size() > wanted.size() && value.starts_with(wanted) && value[wanted.size()] == '-');
            case Op::Prefix:
                return !wanted.empty() && value.starts_with(wanted);
            case Op::Suffix:
                return !wanted.empty() && value.ends_with(wanted);
            case Op::Substring:
                return !wanted.empty() && value.find(wanted) != std::string_view::npos;
        }
        return false;
    }

    bool nth_matches(int a, int b, std::size_t position) {
        const auto x = static_cast<long long>(position) - b;
        if (a == 0) {
            return x == 0;
        }
        return x % a == 0 && x / a >= 0;
    }
} // namespace weft::dom
