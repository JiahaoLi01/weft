#include "dom/selector.hpp"

#include "constants/dom.hpp"

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
                    return unsupported("attribute flags outside profile");
                }
                if (p == text.size() || text[p++] != ']') {
                    return syntax("unterminated attribute selector");
                }
                c.attributes.push_back(std::move(a));
                return true;
            }

            bool compound(Compound& c, Selector& s) {
                while (p < text.size() && !space(text[p]) && text[p] != '>') {
                    const char prefix = text[p];
                    if (prefix == '+' || prefix == '~') {
                        return unsupported("sibling combinator outside profile");
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
                        if (p < text.size() && text[p] == ':') {
                            return unsupported("pseudo-element outside profile");
                        }
                        std::string pseudo;
                        if (!name(pseudo)) {
                            return false;
                        }
                        if (pseudo == "root" && !c.root) {
                            c.root = true;
                        } else if (grammar.states && pseudo == "hover" && !c.hover) {
                            c.hover = true;
                        } else if (grammar.states && pseudo == "active" && !c.active) {
                            c.active = true;
                        } else {
                            return unsupported("pseudo-class outside profile");
                        }
                        ++s.specificity[1];
                    } else if (prefix == '[') {
                        ++p;
                        if (!attribute(c)) {
                            return false;
                        }
                        ++s.specificity[1];
                    } else {
                        if (!c.tag.empty() || !c.ids.empty() || !c.classes.empty() || !c.attributes.empty() || c.root ||
                            c.hover || c.active) {
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
                    !c.hover && !c.active) {
                    return syntax("empty selector compound");
                }
                return true;
            }

            bool selector(Selector& s) {
                spaces();
                while (p < text.size()) {
                    Compound c;
                    if (!compound(c, s)) {
                        return false;
                    }
                    s.parts.push_back(std::move(c));
                    if (s.parts.size() > std::size_t(constants::max_selector_compounds)) {
                        return unsupported("selector depth bound");
                    }
                    spaces();
                    if (p < text.size()) {
                        char relation = ' ';
                        if (text[p] == '>') {
                            relation = '>';
                            ++p;
                            spaces();
                        } else if (text[p] == '+' || text[p] == '~') {
                            return unsupported("sibling combinator outside profile");
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
        std::size_t start{};
        char quote{};
        for (std::size_t i = 0; i <= text.size(); ++i) {
            if (i < text.size() && quote) {
                if (text[i] == quote) {
                    quote = 0;
                }
                continue;
            }
            if (i < text.size() && (text[i] == '\'' || text[i] == '"')) {
                quote = text[i];
                continue;
            }
            if (i == text.size() || text[i] == ',') {
                if (out.size() >= max_items) {
                    return make_diagnostic(grammar.unsupported, "selector list bound");
                }
                auto item = parse_selector(text.substr(start, i - start), grammar);
                if (!item) {
                    return item.error();
                }
                out.push_back(std::move(*item));
                start = i + 1;
            }
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
        switch (a.op) {
            case Op::Exists:
                return true;
            case Op::Equals:
                return value == a.value;
            case Op::Includes:
                return !a.value.empty() && a.value.find_first_of(" \t\n\f\r") == std::string::npos &&
                       has_class(value, a.value);
            case Op::DashMatch:
                return value == a.value ||
                       (value.size() > a.value.size() && value.starts_with(a.value) && value[a.value.size()] == '-');
            case Op::Prefix:
                return !a.value.empty() && value.starts_with(a.value);
            case Op::Suffix:
                return !a.value.empty() && value.ends_with(a.value);
            case Op::Substring:
                return !a.value.empty() && value.find(a.value) != std::string_view::npos;
        }
        return false;
    }
} // namespace weft::dom
