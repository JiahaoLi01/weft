#include "constants/common.hpp"
#include "dom/selector.hpp"
#include "support/check.hpp"
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The one selector engine of DOM queries and the style cascade (dom/selector.hpp), over a small element tree.
namespace {
    using weft::testing::require;
    namespace dom = weft::dom;

    struct Node {
        std::string local;
        bool html{true};
        std::map<std::string, std::string> attributes;
        int parent{-1};
        bool hovered{};
    };

    struct Element {
        const std::vector<Node>* nodes{};
        int index{};

        const Node& n() const {
            return (*nodes)[std::size_t(index)];
        }

        std::string_view local_name() const {
            return n().local;
        }

        bool html() const {
            return n().html;
        }

        const std::string* attribute(std::string_view name) const {
            const auto it = n().attributes.find(std::string(name));
            return it == n().attributes.end() ? nullptr : &it->second;
        }

        bool hovered() const {
            return n().hovered;
        }

        bool active() const {
            return false;
        }

        bool document_root() const {
            return n().parent < 0;
        }

        std::optional<Element> parent() const {
            return n().parent < 0 ? std::nullopt : std::optional(Element{nodes, n().parent});
        }
    };

    const dom::SelectorGrammar css{true, true, weft::Diag::DomSelector, weft::Diag::DomUnsupportedSelector};
    const dom::SelectorGrammar plain{false, false, weft::Diag::StyleProfile, weft::Diag::StyleProfile};

    dom::Selector parse(std::string_view text, const dom::SelectorGrammar& grammar = css) {
        return dom::parse_selector(text, grammar).value();
    }

    bool refused(std::string_view text, const dom::SelectorGrammar& grammar, weft::Diag code, std::string_view detail) {
        const auto parsed = dom::parse_selector(text, grammar);
        return !parsed && parsed.error().code == code && parsed.error().message.find(detail) != std::string::npos;
    }

    // Text that is not a selector, and a valid selector the DOM grammar does not support.
    bool syntax(std::string_view text, std::string_view detail) {
        return refused(text, css, weft::Diag::DomSelector, detail);
    }

    bool unsupported(std::string_view text, std::string_view detail) {
        return refused(text, css, weft::Diag::DomUnsupportedSelector, detail);
    }
} // namespace

int main() {
    try {
        // html > body > div.card#main[data-kind=axis-major] > svg:g > svg:linearGradient (SVG names keep their case).
        std::vector<Node> tree{
            {"html", true, {}, -1},
            {"body", true, {}, 0},
            {"div", true, {{"class", "card\tbig\fwide"}, {"id", "main"}, {"data-kind", "axis-major"}}, 1, true},
            {"g", false, {{"class", "tick"}}, 2},
            {"linearGradient", false, {{"viewBox", "0 0 1 1"}}, 3},
        };
        const Element html{&tree, 0};
        const Element div{&tree, 2};
        const Element g{&tree, 3};
        const Element gradient{&tree, 4};

        // Types: HTML elements compare the lower-cased selector exactly, so an upper-case HTML local name (from
        // createElementNS) matches neither spelling; SVG elements compare the selector as written.
        const std::vector<Node> upper{{"DIV", true, {}, -1}};
        require(dom::matches(parse("DIV"), div) && dom::matches(parse("linearGradient"), gradient) &&
                    !dom::matches(parse("lineargradient"), gradient) &&
                    !dom::matches(parse("div"), Element{&upper, 0}) && !dom::matches(parse("DIV"), Element{&upper, 0}),
                "type selector case");
        // Attributes: HTML names case-insensitively, SVG names exactly.
        require(dom::matches(parse("[DATA-KIND]"), div) && dom::matches(parse("[viewBox]"), gradient) &&
                    !dom::matches(parse("[viewbox]"), gradient),
                "attribute name case");
        require(dom::matches(parse("[data-kind=axis-major]"), div) && dom::matches(parse("[data-kind|=axis]"), div) &&
                    !dom::matches(parse("[data-kind|=axi]"), div) &&
                    dom::matches(parse("[data-kind^=axis][data-kind$=major][data-kind*='s-m']"), div) &&
                    !dom::matches(parse("[data-kind^='']"), div) && dom::matches(parse("[class~=big]"), div) &&
                    !dom::matches(parse("[class~='big wide']"), div),
                "attribute operators");
        // Classes split on ASCII whitespace only.
        require(dom::matches(parse(".card.big.wide"), div) && dom::has_class("a\vb", "a\vb") &&
                    !dom::has_class("a\vb", "a"),
                "class whitespace");
        // Combinators: child, and descendant with backtracking past a closer ancestor that matches the compound but
        // not the rest (the universal compound first meets g, whose parent is not body).
        require(dom::matches(parse("body > div > g"), g) && !dom::matches(parse("html > div"), div) &&
                    dom::matches(parse("html div g linearGradient"), gradient) &&
                    dom::matches(parse("body > div g > linearGradient"), gradient) &&
                    dom::matches(parse("body > * linearGradient"), gradient) &&
                    !dom::matches(parse("div > linearGradient"), gradient) && !dom::matches(parse("div div"), div),
                "combinators");
        // :root is the topmost element; states read the element unless matching ignores them.
        require(dom::matches(parse(":root"), html) && !dom::matches(parse(":root"), div) &&
                    dom::matches(parse("div:hover"), div) && !dom::matches(parse("g:hover"), g) &&
                    dom::matches(parse("g:hover"), g, false) && !dom::matches(parse("div:active"), div),
                "pseudo-classes");
        // Specificity: ids, classes/attributes/pseudo-classes, types.
        const auto s = parse("div#main.card[data-kind]:hover > g");
        require(s.specificity == std::array<unsigned, 3>{1, 3, 2}, "specificity");
        // Lists separate on commas outside quotes.
        const auto list = dom::parse_selector_list("g, [title='a,b'], #main", css, 3).value();
        require(list.size() == 3 && dom::matches(list, div) && dom::matches(list, g), "selector list");
        const auto bound = dom::parse_selector_list("a, b, c, d", css, 3);
        require(!bound && bound.error().code == weft::Diag::DomUnsupportedSelector, "selector list bound");
        // Identifiers may begin with an escaped digit, a hyphen and a letter, or two hyphens.
        const auto escaped = dom::parse_selector(".\\31 a.-b.--c", css);
        require(escaped && escaped->parts[0].classes == std::vector<std::string>{"1a", "-b", "--c"},
                "identifier starts");
        // Escapes and states depend on the grammar.
        require(dom::matches(parse(".c\\61 rd"), div) && dom::matches(parse("\\64iv"), div) &&
                    refused("\\64iv", plain, weft::Diag::StyleProfile, "selector identifier") &&
                    refused("div:hover", plain, weft::Diag::StyleProfile, "pseudo-class outside profile") &&
                    unsupported(".\\100", "escape outside ASCII"),
                "grammar options");
        // Text that is not a selector is a syntax error; a valid selector outside the grammar is unsupported.
        for (const auto& [text, detail] :
             std::vector<std::pair<const char*, const char*>>{{"", "selector chain"},
                                                              {".a*", "type selector must begin compound"},
                                                              {"[x", "unterminated attribute selector"},
                                                              {"[x i]", "attribute operator"},
                                                              {"[x!=1]", "attribute operator"},
                                                              {"[x=", "attribute value"},
                                                              {"[x=]", "selector identifier"},
                                                              {"[x=1]", "selector identifier"},
                                                              {".1a", "selector identifier"},
                                                              {"#-1", "selector identifier"},
                                                              {"[x='1]", "attribute string"},
                                                              {"> div", "empty selector compound"}}) {
            require(syntax(text, detail), std::string("not a syntax error: ") + text);
        }
        for (const auto& [text, detail] :
             std::vector<std::pair<const char*, const char*>>{{"div + g", "sibling combinator"},
                                                              {"div ~ g", "sibling combinator"},
                                                              {"div+g", "sibling combinator"},
                                                              {"[x='1' i]", "attribute flags"},
                                                              {":focus-visible", "pseudo-class outside profile"},
                                                              {"div:not(.a)", "pseudo-class outside profile"},
                                                              {"div::before", "pseudo-element outside profile"}}) {
            require(unsupported(text, detail), std::string("not unsupported: ") + text);
        }
        std::string deep = "a";
        for (int i = 0; i < weft::constants::max_selector_compounds; ++i) {
            deep += " a";
        }
        require(unsupported(deep, "selector depth bound"), "compound bound");
        std::cout << "PASS one selector engine: type and attribute case, operators, class whitespace, combinators with "
                     "backtracking, pseudo-classes, specificity, lists, grammar options and diagnostics\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
