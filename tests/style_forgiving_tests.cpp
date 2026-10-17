// The forgiving style profile (StyleProfile::forgiving, WEFT_PAGE_FORGIVING_CSS): style sheets parsed as CSS Syntax
// parses them, with cascade layers, @media, @supports, @property, @keyframes and nesting; the selectors of Selectors 4;
// logical properties; colors and calc() computed to the forms the scene draws; and what is outside the profile dropped
// with a warning instead of failing the sheet. Each check is the browser's answer for the same sheet and document.
#include "scene/scene.hpp"
#include "style/style.hpp"
#include "support/check.hpp"
#include "constants/common.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace weft;

namespace {
    using weft::testing::require;

    // A document of elements: tag, attributes, children (text nodes as "#text" with data in "text").
    struct Element {
        std::string tag;
        std::map<std::string, std::string> attributes;
        std::vector<Element> children;
        std::string text;
    };

    std::uint32_t next_slot = 1;

    DomSnapshot snapshot(const Element& e) {
        DomSnapshot n;
        n.identity = {1, next_slot++, 1};
        if (e.tag == "#text") {
            n.type = constants::text_node_type;
            n.local_name = "#text";
            n.data = e.text;
            return n;
        }
        n.type = constants::element_node_type;
        n.local_name = e.tag;
        n.namespace_uri = constants::xhtml_namespace_uri;
        for (const auto& [k, v] : e.attributes) {
            n.attributes.emplace(k, v);
        }
        for (const auto& c : e.children) {
            n.children.push_back(snapshot(c));
        }
        return n;
    }

    const ComputedStyle* by_id(const DomSnapshot& dom, const ComputedStyle& style, const std::string& id) {
        if (const auto it = dom.attributes.find("id"); it != dom.attributes.end() && it->second == id) {
            return &style;
        }
        std::size_t i{};
        for (const auto& c : dom.children) {
            if (c.type != constants::element_node_type) {
                continue;
            }
            if (const auto* found = by_id(c, style.children[i++], id)) {
                return found;
            }
        }
        return nullptr;
    }

    struct Result {
        DomSnapshot dom;
        ComputedStyle style;

        std::string value(const std::string& id, PropertyId property) const {
            const auto* s = by_id(dom, style, id);
            require(s != nullptr, "no element #" + id);
            const auto* v = s->values.find(property);
            return v ? v->text() : std::string("<none>");
        }
    };

    Result cascade(const std::string& sheet, const Element& root, float width = 800, float height = 600) {
        Result r;
        r.dom = snapshot(root);
        r.style = compute_style(r.dom, sheet, scene_style_profile(true), width, height);
        return r;
    }

    void same(const std::string& got, const std::string& want, const std::string& what) {
        require(got == want, what + ": got '" + got + "', want '" + want + "'");
    }

    Element el(std::string tag, std::string id, std::string cls = "", std::vector<Element> children = {}) {
        Element e{std::move(tag), {}, std::move(children), {}};
        if (!id.empty()) {
            e.attributes["id"] = std::move(id);
        }
        if (!cls.empty()) {
            e.attributes["class"] = std::move(cls);
        }
        return e;
    }

    // Cascade layers (CSS Cascade 5 §6.4): later layers win, unlayered rules win over layered ones whatever their
    // specificity, and important declarations reverse the layer order.
    void layers() {
        const std::string sheet = "@layer base, utilities;"
                                  "@layer utilities { .u { color: #00ff00 } }"
                                  "@layer base { button.b:not(:disabled) { color: #ff0000 } #x { width: 1px } }"
                                  ".plain { width: 2px }"
                                  "@layer a { .imp { height: 1px !important } }"
                                  ".imp { height: 2px !important }"
                                  "@layer outer { @layer inner { .n { width: 3px } } .n { width: 4px } }";
        const auto r = cascade(sheet, el("html", "root", "", {el("button", "btn", "b u"), el("div", "x", "plain"),
                                                         el("div", "i", "imp"), el("div", "nested", "n")}));
        same(r.value("btn", PropertyId::Color), "#00ff00", "a later layer beats a more specific earlier one");
        same(r.value("x", PropertyId::Width), "2px", "an unlayered rule beats a layered #id");
        same(r.value("i", PropertyId::Height), "1px", "important declarations reverse the layer order");
        same(r.value("nested", PropertyId::Width), "4px", "a layer's own rules come after its sublayers");
    }

    // @media and @supports as the profile answers them; the viewport is the cascade's.
    void conditions() {
        const std::string sheet = "@media (hover: hover) { .h { width: 1px } }"
                                  "@media (min-width: 600px) { .w { height: 5px } }"
                                  "@media (width >= 40rem) and (orientation: landscape) { .r { height: 6px } }"
                                  "@media print { .p { width: 9px } }"
                                  "@supports (color: color-mix(in lab, red, red)) { .s { width: 3px } }"
                                  "@supports (-webkit-hyphens: none) { .t { width: 4px } }"
                                  "@supports not (margin-trim: inline) { .m { width: 5px } }";
        const auto doc = el("html", "root", "", {el("div", "a", "h w r p s t m")});
        const auto wide = cascade(sheet, doc, 800, 600);
        same(wide.value("a", PropertyId::Height), "6px", "a range and orientation hold in a wide viewport");
        same(wide.value("a", PropertyId::Width), "5px", "@supports not of an unsupported property holds");
        const auto narrow = cascade(".x{}" + sheet, doc, 500, 600);
        same(narrow.value("a", PropertyId::Height), "<none>", "min-width fails in a narrow viewport");
        const auto hover = cascade("@media (hover: hover) { .h { width: 1px } } @media print { .h { width: 9px } }",
                                   el("html", "root", "", {el("div", "a", "h")}));
        same(hover.value("a", PropertyId::Width), "1px", "the device hovers and is no printer");
        const auto supports =
            cascade("@supports (color: color-mix(in lab, red, red)) { .s { width: 3px } }"
                    "@supports (-webkit-hyphens: none) { .s { width: 4px } }",
                    el("html", "root", "", {el("div", "a", "s")}));
        same(supports.value("a", PropertyId::Width), "3px", "color-mix() is supported, -webkit-hyphens is not");

        // A compiled sheet tells its owner when the viewport changes which rules match.
        CompiledStyle compiled("@media (min-width: 600px) { .w { height: 5px } }", scene_style_profile(true));
        compiled.set_viewport(800, 600);
        require(!compiled.set_viewport(900, 600), "a viewport that changes no condition's outcome");
        require(compiled.set_viewport(500, 600), "a viewport that changes a condition's outcome");
    }

    // @property: a registered property's initial value stands where nothing set it, and one that does not inherit
    // starts from it in every element.
    void registered_properties() {
        const std::string sheet = "@property --x { syntax: \"*\"; inherits: false; initial-value: 3px }"
                                  "@property --y { syntax: \"*\"; inherits: true; initial-value: 7px }"
                                  ".p { --x: 9px; --y: 8px } .c { width: var(--x); height: var(--y) }"
                                  ".d { width: var(--x); height: var(--y) }";
        const auto r = cascade(sheet, el("html", "root", "", {el("div", "parent", "p", {el("div", "child", "c")}),
                                                         el("div", "alone", "d")}));
        same(r.value("child", PropertyId::Width), "3px", "a non-inherited registered property does not inherit");
        same(r.value("child", PropertyId::Height), "8px", "an inherited registered property inherits");
        same(r.value("alone", PropertyId::Width), "3px", "a registered property's initial value");
        same(r.value("alone", PropertyId::Height), "7px", "an inherited one's initial value at the root");
    }

    // Colors computed to #rrggbb when opaque and rgba() otherwise, from every CSS Color 4 and 5 form.
    void colors() {
        const std::string sheet =
            ".a { color: oklch(62.7955% 0.257627 29.2339) }"
            ".b { color: color-mix(in oklab, #d4a843 20%, transparent) }"
            ".c { color: #0000 } .d { color: gold } .e { color: hsl(120 100% 50%) }"
            ".f { color: rgb(255 0 0 / 50%) } .g { color: #ff000080 } .h { color: lab(54.2917% 80.8125 69.8851) }"
            ".i { color: color(srgb 0 0 1) } .j { color: hwb(240 0% 0%) }"
            ".k { color: #123456; background-color: color-mix(in srgb, currentcolor 50%, #ffffff) }"
            ".l { box-shadow: 0 0 2px oklch(0% 0 0 / 0.5) }";
        Element root = el("html", "root");
        for (const auto* id : {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l"}) {
            root.children.push_back(el("div", id, id));
        }
        const auto r = cascade(sheet, root);
        same(r.value("a", PropertyId::Color), "#ff0000", "oklch() of sRGB red");
        same(r.value("b", PropertyId::Color), "rgba(212, 168, 67, 0.2)", "a color mixed with transparent");
        same(r.value("c", PropertyId::Color), "rgba(0, 0, 0, 0)", "#rgba");
        same(r.value("d", PropertyId::Color), "#ffd700", "a named color");
        same(r.value("e", PropertyId::Color), "#00ff00", "hsl()");
        same(r.value("f", PropertyId::Color), "rgba(255, 0, 0, 0.5)", "modern rgb() with / alpha");
        same(r.value("g", PropertyId::Color), "rgba(255, 0, 0, 0.502)", "#rrggbbaa");
        same(r.value("h", PropertyId::Color), "#ff0000", "lab() of sRGB red");
        same(r.value("i", PropertyId::Color), "#0000ff", "color(srgb)");
        same(r.value("j", PropertyId::Color), "#0000ff", "hwb()");
        same(r.value("k", PropertyId::BackgroundColor), "#899aab", "currentcolor in color-mix() is the element's");
        same(r.value("l", PropertyId::BoxShadow), "0 0 2px rgba(0, 0, 0, 0.5)", "a color inside a shadow");
    }

    // calc(), min(), max() and clamp() simplified; logical properties as physical ones.
    void math_and_logical() {
        const std::string sheet = ":root { --spacing: .25rem }"
                                  ".a { width: calc(var(--spacing) * 4); height: calc(100% - 2rem) }"
                                  ".b { width: calc(infinity * 1px); height: max(10px, 2rem) }"
                                  ".c { padding-inline: 4px 8px; margin-block: 2px; inset-inline: 0 }"
                                  ".d { border-start-start-radius: 3px; inline-size: 5px; min-block-size: 6px }";
        const auto r = cascade(sheet, el("html", "root", "", {el("div", "a", "a"), el("div", "b", "b"),
                                                         el("div", "c", "c"), el("div", "d", "d")}));
        same(r.value("a", PropertyId::Width), "16px", "calc() of a rem variable times a number");
        same(r.value("a", PropertyId::Height), "calc(100% - 32px)", "a percentage minus rem");
        same(r.value("b", PropertyId::Width), "1e+07px", "infinity clamps to a large finite length");
        same(r.value("b", PropertyId::Height), "32px", "max() of px and rem");
        same(r.value("c", PropertyId::PaddingLeft), "4px", "padding-inline's start");
        same(r.value("c", PropertyId::PaddingRight), "8px", "padding-inline's end");
        same(r.value("c", PropertyId::MarginTop), "2px", "margin-block, one value");
        same(r.value("c", PropertyId::MarginBottom), "2px", "margin-block, one value for both");
        same(r.value("c", PropertyId::Left), "0", "inset-inline");
        same(r.value("d", PropertyId::BorderTopLeftRadius), "3px", "border-start-start-radius");
        same(r.value("d", PropertyId::Width), "5px", "inline-size");
        same(r.value("d", PropertyId::MinHeight), "6px", "min-block-size");
    }

    // Selectors 4: :is(), :where(), :not(), structural pseudo-classes and sibling combinators; pseudo-element rules
    // do not style their originating element.
    void selectors() {
        const std::string sheet = "li:first-child { width: 1px } li:last-child { height: 1px }"
                                  "li:nth-child(2n) { color: #00ff00 } .a + .b { width: 7px } .a ~ .c { height: 8px }"
                                  ":is(.x, .y) .z { width: 9px } :where(.q) .r { width: 1px } .r { width: 2px }"
                                  "div:not(.n) { opacity: 0.5 } .e::before { width: 3px } .e::placeholder { width: 4px }"
                                  ".v:empty { height: 2px } .w:only-child { height: 3px }";
        const auto r = cascade(
            sheet, el("html", "root", "",
                      {el("ul", "list", "", {el("li", "one"), el("li", "two"), el("li", "three")}),
                       el("p", "s", "", {el("i", "a", "a"), el("i", "b", "b"), el("i", "c", "c")}),
                       el("section", "y", "y", {el("span", "z", "z")}), el("section", "q", "q", {el("span", "r", "r")}),
                       el("div", "n", "n"), el("div", "e", "e"), el("div", "v", "v"),
                       el("p", "only", "", {el("b", "w", "w")})}));
        same(r.value("one", PropertyId::Width), "1px", ":first-child");
        same(r.value("three", PropertyId::Height), "1px", ":last-child");
        same(r.value("two", PropertyId::Color), "#00ff00", ":nth-child(2n)");
        same(r.value("b", PropertyId::Width), "7px", "the next-sibling combinator");
        same(r.value("c", PropertyId::Height), "8px", "the subsequent-sibling combinator");
        same(r.value("z", PropertyId::Width), "9px", ":is()");
        same(r.value("r", PropertyId::Width), "2px", ":where() has no specificity");
        same(r.value("n", PropertyId::Opacity), "<none>", ":not()");
        same(r.value("e", PropertyId::Width), "<none>", "a pseudo-element's rule styles no element");
        same(r.value("e", PropertyId::Opacity), "0.5", ":not() elsewhere");
        same(r.value("v", PropertyId::Height), "2px", ":empty");
        same(r.value("w", PropertyId::Height), "3px", ":only-child");
    }

    // CSS Nesting: & is the parent rule's selector; a nested rule without & is a descendant of it; a nested @media
    // holds the parent's declarations.
    void nesting() {
        const std::string sheet = ".p { width: 1px; & .q { width: 2px } > .r { width: 3px } &.s { width: 4px }"
                                  " @media (hover: hover) { height: 5px } }";
        const auto r = cascade(sheet, el("html", "root", "", {el("div", "p", "p", {el("i", "q", "q"), el("i", "r", "r")}),
                                                         el("div", "ps", "p s")}));
        same(r.value("p", PropertyId::Width), "1px", "the parent rule");
        same(r.value("q", PropertyId::Width), "2px", "& and a descendant");
        same(r.value("r", PropertyId::Width), "3px", "a relative selector");
        same(r.value("ps", PropertyId::Width), "4px", "&.class");
        same(r.value("p", PropertyId::Height), "5px", "declarations in a nested @media");
    }

    // What the profile cannot take is dropped with a warning; the rest of the sheet and of the rule stays. A strict
    // profile still fails the sheet.
    void drops() {
        const std::string sheet = ".k { outline: 1px solid red; width: 7px }"
                                  ".bad:has(.x) { width: 8px } .ok { height: 9px }"
                                  "@container (min-width: 1px) { .k { width: 1px } }"
                                  "@keyframes spin { to { transform: rotate(360deg) } }"
                                  "@font-face { font-family: X; src: url(x.ttf) }";
        CompiledStyle compiled(sheet, scene_style_profile(true));
        const auto warnings = compiled.take_warnings();
        const auto says = [&](const std::string& part) {
            return std::any_of(warnings.begin(), warnings.end(), [&](const std::string& w) {
                return w.find(part) != std::string::npos;
            });
        };
        require(says("outline"), "a dropped property is a warning");
        require(says(":has"), "a dropped rule names its selector");
        require(says("@container"), "a dropped at-rule");
        require(says("@font-face"), "@font-face is the host's");
        require(compiled.keyframes().find("@keyframes spin") != std::string::npos, "@keyframes kept for the scene");
        require(compiled.take_warnings().empty(), "warnings are taken once");
        const auto r = cascade(sheet, el("html", "root", "", {el("div", "k", "k"), el("div", "ok", "ok")}));
        same(r.value("k", PropertyId::Width), "7px", "a rule keeps what it can take");
        same(r.value("ok", PropertyId::Height), "9px", "a sheet keeps the rules it can take");
        bool strict_failed{};
        try {
            CompiledStyle strict("@media (min-width:1px){.b{opacity:1}}", scene_style_profile(false));
        } catch (const std::invalid_argument&) {
            strict_failed = true;
        }
        require(strict_failed, "the strict profile still refuses at-rules");
    }
} // namespace

int main() {
    try {
        layers();
        conditions();
        registered_properties();
        colors();
        math_and_logical();
        selectors();
        nesting();
        drops();
        std::cout << "PASS forgiving CSS: cascade layers, @media/@supports, @property, colors, calc, logical "
                     "properties, Selectors 4, nesting and drops with warnings\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
