// The developer tools' protocol against a real View (docs/devtools.md), through the socket a tool connects to: the DOM and
// its changes, the CSS answers against what Chrome's own DevTools domains say of the same page
// (tools/browser_devtools_reference.mjs), what the engine does not take, the console, the overlay and picking an element.
#include "devtools/agent.hpp"
#include "devtools/json.hpp"
#include "devtools/server.hpp"
#include "support/check.hpp"
#include "support/devtools_client.hpp"
#include "support/files.hpp"
#include "view/view.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace {
    using weft::devtools::Json;
    using weft::testing::require;

    const std::string directory = WEFT_SOURCE_DIR "/tests/fixtures/devtools/";

    std::shared_ptr<const weft::FontCatalog> catalog() {
        auto c = std::make_shared<weft::FontCatalog>();
        weft::FontFaceEntry face;
        face.family = "Roboto";
        face.source.path = WEFT_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf";
        c->faces.push_back(face);
        c->generic_families = {{"sans-serif", "Roboto"}, {"system-ui", "Roboto"}, {"monospace", "Roboto"}};
        return c;
    }

    using Tool = weft::testing::DevtoolsClient;

    const Json& at(const Json& value, std::string_view key) {
        const auto* member = value.find(key);
        require(member != nullptr, "no " + std::string(key) + " in " + value.dump().substr(0, 300));
        return *member;
    }

    const Json::Array& items(const Json& value, std::string_view key) {
        const auto* list = at(value, key).array();
        require(list != nullptr, std::string(key) + " is not an array");
        return *list;
    }

    int query(Tool& tool, const std::string& selector) {
        const auto id =
            int(tool.result("DOM.querySelector", {{"nodeId", 1}, {"selector", selector}}).number_at("nodeId"));
        require(id != 0, "no element for " + selector);
        return id;
    }

    void check_quad(const Json& got, const Json& want, const std::string& what) {
        const auto* a = got.array();
        const auto* b = want.array();
        require(a && b && a->size() == 8 && b->size() == 8, what + ": a quad of eight numbers");
        for (std::size_t i = 0; i < 8; ++i) {
            require(std::abs(*(*a)[i].number() - *(*b)[i].number()) < 0.01,
                    what + " differs from Chrome: " + got.dump() + " and " + want.dump());
        }
    }

    // What Chrome's CSS and DOM domains say of the same page.
    void check_against_chrome(Tool& tool) {
        const auto reference = Json::parse(weft::testing::read_file(directory + "chrome-results.json"));
        require(reference.has_value(), "the reference");
        for (const auto& element : items(*reference, "elements")) {
            const auto name = element.string_at("id");
            const int id = query(tool, "#" + name);
            const auto matched = tool.result("CSS.getMatchedStylesForNode", {{"nodeId", id}});
            Json::Array ours;
            for (const auto& rule : items(matched, "matchedCSSRules")) {
                if (at(rule, "rule").string_at("origin") == "regular") {
                    ours.push_back(rule);
                }
            }
            const auto& theirs = items(element, "rules");
            std::string lines;
            for (const auto& rule : ours) {
                lines += " " + at(at(at(rule, "rule"), "style"), "range").dump();
            }
            require(ours.size() == theirs.size(), name + ": " + std::to_string(ours.size()) + " rules, Chrome " +
                                                      std::to_string(theirs.size()) + ":" + lines);
            for (std::size_t r = 0; r < ours.size(); ++r) {
                const auto label = name + " rule " + std::to_string(r);
                const auto& rule = at(ours[r], "rule");
                const auto& theirs_rule = theirs[r];
                require(items(ours[r], "matchingSelectors").size() == std::size_t(theirs_rule.number_at("matching")),
                        label + ": selectors that match");
                std::vector<const Json*> written;
                for (const auto& property : items(at(rule, "style"), "cssProperties")) {
                    if (property.find("range")) {
                        written.push_back(&property);
                    }
                }
                const auto& declarations = items(theirs_rule, "declarations");
                require(written.size() == declarations.size(), label + ": declarations");
                for (std::size_t d = 0; d < written.size(); ++d) {
                    const auto& w = *written[d];
                    const auto& t = declarations[d];
                    const auto where = label + " " + t.string_at("name");
                    require(w.string_at("name") == t.string_at("name"), where + ": name");
                    require(w.string_at("value") == t.string_at("value"),
                            where + ": value " + w.string_at("value") + " Chrome " + t.string_at("value"));
                    require(w.bool_at("important") == t.bool_at("important"), where + ": important");
                    const auto& place = at(w, "range");
                    require(place.number_at("startLine") == t.number_at("line"), where + ": line");
                    require(place.number_at("startColumn") == t.number_at("column"), where + ": column");
                    require(place.number_at("endColumn") == t.number_at("end"), where + ": end");
                    // Where the two differ on what parses, the engine's list is the point: below.
                    if (t.string_at("name") != "column-count") {
                        require(w.bool_at("parsedOk") == t.bool_at("parsedOk"), where + ": parsedOk");
                    }
                }
            }
            const auto model = at(tool.result("DOM.getBoxModel", {{"nodeId", id}}), "model");
            check_quad(at(model, "border"), at(element, "border"), name + " border");
            check_quad(at(model, "content"), at(element, "content"), name + " content");
            check_quad(at(model, "margin"), at(element, "margin"), name + " margin");
        }
    }

    void check_document(Tool& tool) {
        const auto document = tool.result("DOM.getDocument", {{"depth", -1}});
        const auto& root = at(document, "root");
        require(root.number_at("nodeType") == 9, "the root is the document");
        require(items(root, "children")[0].string_at("nodeName") == "HTML", "its element is html");

        const int one = query(tool, "#one");
        const auto described = at(tool.result("DOM.describeNode", {{"nodeId", one}}), "node");
        require(described.string_at("localName") == "div", "described");
        const auto outer = tool.result("DOM.getOuterHTML", {{"nodeId", one}}).string_at("outerHTML");
        require(outer.starts_with("<div") && outer.ends_with(">one</div>"), "outer HTML: " + outer);
        require(items(tool.result("DOM.querySelectorAll", {{"nodeId", 1}, {"selector", ".box"}}), "nodeIds").size() ==
                    2,
                "querySelectorAll");
    }

    void check_mutations(Tool& tool, weft::View& view) {
        const int root = query(tool, "#root");
        tool.result("DOM.requestChildNodes", {{"nodeId", root}, {"depth", -1}});
        tool.settle();
        tool.clear();

        view.evaluate(
            "var n=document.createElement('div');n.id='added';document.getElementById('root').appendChild(n)");
        const auto inserted = tool.take("DOM.childNodeInserted", "added");
        require(inserted.has_value(), "an element added is sent");
        const int added = int(at(at(*inserted, "params"), "node").number_at("nodeId"));

        view.evaluate("document.getElementById('added').setAttribute('title','x')");
        require(tool.take("DOM.attributeModified", "title").has_value(), "an attribute set is sent");
        view.evaluate("document.getElementById('added').removeAttribute('title')");
        require(tool.take("DOM.attributeRemoved", "title").has_value(), "an attribute removed is sent");
        view.evaluate("document.getElementById('one').firstChild.nodeValue='uno'");
        require(tool.take("DOM.characterDataModified", "uno").has_value(), "text changed is sent");
        view.evaluate("var a=document.getElementById('added');a.parentNode.removeChild(a)");
        const auto removed = tool.take("DOM.childNodeRemoved");
        require(removed && at(*removed, "params").number_at("nodeId") == added, "an element removed is sent");

        // Children the tool holds are not sent again: it would lose what it holds of them.
        tool.settle();
        tool.clear();
        tool.result("DOM.requestChildNodes", {{"nodeId", root}});
        tool.settle(4);
        require(!tool.take("DOM.setChildNodes", "\"parentId\":" + std::to_string(root)).has_value(),
                "children asked for again are not sent again");

        // The tools' own edits reach the page.
        const int two = query(tool, "#two");
        tool.result("DOM.setAttributeValue", {{"nodeId", two}, {"name", "lang"}, {"value", "en"}});
        require(view.evaluate("document.getElementById('two').getAttribute('lang')").second == "en", "set attribute");
        tool.result("DOM.removeAttribute", {{"nodeId", two}, {"name", "lang"}});
        require(view.evaluate("String(document.getElementById('two').hasAttribute('lang'))").second == "false",
                "removed attribute");
    }

    void check_styles(Tool& tool, weft::View& view) {
        const int two = query(tool, "#two");
        const auto inline_style = at(tool.result("CSS.getInlineStylesForNode", {{"nodeId", two}}), "inlineStyle");
        require(inline_style.string_at("cssText") == "height: 24px; column-count: 3", "the style attribute");
        const auto& declared = items(inline_style, "cssProperties");
        require(declared.size() == 2 && declared[0].bool_at("parsedOk") && !declared[1].bool_at("parsedOk"),
                "an inline declaration the engine does not take: " + inline_style.dump());

        const auto computed = items(tool.result("CSS.getComputedStyleForNode", {{"nodeId", two}}), "computedStyle");
        bool height = false;
        for (const auto& entry : computed) {
            height = height || (entry.string_at("name") == "height" && entry.string_at("value") == "24px");
        }
        require(height, "computed height");

        // Editing the inline style.
        const auto edited =
            tool.result("CSS.setStyleTexts",
                        {{"edits", Json::Array{Json::Object{{"styleSheetId", inline_style.string_at("styleSheetId")},
                                                            {"range", at(inline_style, "range")},
                                                            {"text", "height: 26px"}}}}});
        require(items(edited, "styles")[0].string_at("cssText") == "height: 26px", "edited style");
        require(view.evaluate("document.getElementById('two').getAttribute('style')").second == "height: 26px",
                "the attribute changed");
        // The sheet itself cannot be edited yet: an error, not a change that did not happen.
        const auto refused = tool.call(
            "CSS.setStyleTexts", {{"edits", Json::Array{Json::Object{{"styleSheetId", "weft-sheet"}, {"text", "x"}}}}});
        require(refused.find("error") != nullptr, "the sheet is not editable yet");

        const auto text = tool.result("CSS.getStyleSheetText", {{"styleSheetId", "weft-sheet"}}).string_at("text");
        require(text == weft::testing::read_file(directory + "page.css"), "the sheet's text");
    }

    void check_unsupported(Tool& tool) {
        const int five = query(tool, "#five");
        const auto matched = tool.result("CSS.getMatchedStylesForNode", {{"nodeId", five}});
        bool column_count = false;
        for (const auto& rule : items(matched, "matchedCSSRules")) {
            for (const auto& property : items(at(at(rule, "rule"), "style"), "cssProperties")) {
                if (property.string_at("name") == "column-count") {
                    column_count = !property.bool_at("parsedOk");
                }
            }
        }
        require(column_count, "a declaration the engine does not take is shown as not parsed");

        const auto report = tool.result("Weft.getCompatibilityReport");
        bool found = false;
        for (const auto& entry : items(report, "declarations")) {
            if (entry.string_at("property") == "column-count" && entry.string_at("selector") == ".multi") {
                found = true;
                require(entry.number_at("line") == 66, "its line: " + entry.dump());
                require(entry.number_at("elements") == 1, "the elements it applies to: " + entry.dump());
                require(entry.string_at("reason").find("column-count") != std::string::npos,
                        "its reason: " + entry.dump());
            }
        }
        require(found, "the report lists it: " + report.dump().substr(0, 400));
    }

    void check_console(Tool& tool, weft::View& view) {
        tool.result("Runtime.enable");
        require(tool.take("Runtime.executionContextCreated").has_value(), "the page's context");
        // What the page wrote before the tool asked is there.
        require(tool.take("Runtime.consoleAPICalled", "before the tools").has_value(), "earlier console output");
        view.evaluate("console.warn('careful')");
        const auto warning = tool.take("Runtime.consoleAPICalled", "careful");
        require(warning && at(*warning, "params").string_at("type") == "warning", "a warning");

        const auto sum = at(tool.result("Runtime.evaluate", {{"expression", "1 + 2"}}), "result");
        require(sum.string_at("type") == "number" && sum.number_at("value") == 3, "a number: " + sum.dump());
        const auto object = at(tool.result("Runtime.evaluate", {{"expression", "({a: 1, b: 'x'})"}}), "result");
        require(object.string_at("type") == "object" && items(at(object, "preview"), "properties").size() == 2,
                "an object: " + object.dump());
        const auto thrown = tool.result("Runtime.evaluate", {{"expression", "throw new Error('boom')"}});
        require(thrown.find("exceptionDetails") &&
                    at(thrown, "result").string_at("description").find("boom") != std::string::npos,
                "what a script threw: " + thrown.dump());
        // The preview as one types may have no side effects, which the engine cannot promise.
        require(tool.call("Runtime.evaluate", {{"expression", "1"}, {"throwOnSideEffect", true}}).find("error") !=
                    nullptr,
                "side effects are not promised");

        const int one = query(tool, "#one");
        tool.result("DOM.setInspectedNode", {{"nodeId", one}});
        require(at(tool.result("Runtime.evaluate", {{"expression", "$0.id"}}), "result").string_at("value") == "one",
                "$0");

        tool.result("Log.enable");
        require(tool.take("Log.entryAdded", "column-count").has_value(), "what the sheet dropped");
    }

    // A tool that connects late is sent what the page said before, unless it asks not to be.
    void check_late_tools(const std::function<void()>& step, std::uint16_t port, weft::View& view) {
        Tool late(step, port, "/devtools/page/target");
        late.result("Runtime.enable");
        require(late.take("Runtime.consoleAPICalled", "before the tools").has_value(), "what the page said before");
        Tool fresh(step, port, "/devtools/page/target");
        fresh.result("Runtime.discardConsoleEntries");
        fresh.result("Runtime.enable");
        view.evaluate("console.log('after the late tools')");
        require(fresh.take("Runtime.consoleAPICalled", "after the late tools").has_value(), "what the page says now");
        require(!fresh.has("Runtime.consoleAPICalled", "before the tools"), "and not what it said before");
    }

    std::size_t overlay_children(weft::View& view) {
        const auto frame = view.shared()->frame();
        require(frame != nullptr, "a frame");
        std::size_t out{};
        for (const auto& child : frame->scene->root.children) {
            out += child.identity.view_epoch == std::uint64_t(1) << 40 ? child.children.size() + 1 : 0;
        }
        return out;
    }

    void check_overlay(Tool& tool, weft::View& view) {
        const auto elements = view.evaluate("String(document.querySelectorAll('*').length)").second;
        const int one = query(tool, "#one");
        tool.result("Overlay.highlightNode", {{"nodeId", one}, {"highlightConfig", Json::Object{{"showInfo", true}}}});
        tool.settle();
        require(overlay_children(view) > 0, "the highlight is in the frames");
        // It is in no document: no script sees it.
        require(view.evaluate("String(document.querySelectorAll('*').length)").second == elements,
                "the document is the same");
        require(tool.result("DOM.getNodeForLocation", {{"x", 30}, {"y", 15}}).number_at("nodeId") == one,
                "nor does a hit test");
        tool.result("Overlay.hideHighlight");
        tool.settle();
        require(overlay_children(view) == 0, "the highlight goes");
    }

    void check_picking(Tool& tool, weft::View& view) {
        const int two = query(tool, "#two");
        const auto clicks = [&] {
            return view.evaluate("String(window.clicks)").second;
        };
        tool.result("Overlay.setInspectMode",
                    {{"mode", "searchForNode"}, {"highlightConfig", Json::Object{{"showInfo", true}}}});
        require(view.hit(30, 50).element.generation != 0, "a host that asks what is under the pointer sends it all");
        view.pointer({weft::PointerKind::Move, 30, 50});
        const auto hovered = tool.take("Overlay.nodeHighlightRequested");
        require(hovered && at(*hovered, "params").number_at("nodeId") == two,
                "the element under the pointer: " + (hovered ? hovered->dump() : ""));
        tool.settle();
        require(overlay_children(view) > 0, "it is highlighted");
        view.pointer({weft::PointerKind::Down, 30, 50, 1});
        view.pointer({weft::PointerKind::Up, 30, 50, 0});
        const auto picked = tool.take("Overlay.inspectNodeRequested");
        require(picked && at(*picked, "params").number_at("backendNodeId") == two, "the element pressed is picked");
        require(clicks() == "0", "the page does not get the press");
        tool.result("Overlay.setInspectMode", {{"mode", "none"}});
        view.pointer({weft::PointerKind::Down, 30, 50, 1});
        view.pointer({weft::PointerKind::Up, 30, 50, 0});
        tool.settle();
        require(clicks() == "1", "after picking the page gets the pointer again");
    }

    using Step = std::function<void()>;

    void check_errors(Tool& tool, std::uint16_t port, const Step& step) {
        const auto unknown = tool.call("No.suchMethod");
        require(unknown.find("error") && at(at(unknown, "error"), "code").number() &&
                    *at(at(unknown, "error"), "code").number() == -32601,
                "an unknown method");
        require(tool.call("DOM.getOuterHTML", {{"nodeId", 999999}}).find("error") != nullptr, "an unknown node");
        tool.raw("this is not JSON");
        tool.settle(4);
        require(tool.result("Runtime.getIsolateId").string_at("id") == "weft",
                "the connection survives text that is not JSON");

        // A web page the developer has open must not reach the game.
        {
            Tool evil(step, port, "/devtools/page/target", "Origin: https://evil.example\r\n");
            require(evil.head().find("403") != std::string::npos, "another origin is refused: " + evil.head());
        }
        {
            Tool ours(step, port, "/devtools/page/target", "Origin: devtools://devtools\r\n");
            require(ours.head().find("101") != std::string::npos, "the tools' own origin is taken");
        }
    }
} // namespace

// The style of the rule of a selector among an element's matched rules.
Json rule_style(Tool& tool, int node, std::string_view selector) {
    const auto matched = tool.result("CSS.getMatchedStylesForNode", {{"nodeId", node}});
    for (const auto& rule : items(matched, "matchedCSSRules")) {
        if (at(at(rule, "rule"), "selectorList").string_at("text") == selector) {
            return at(rule, "rule");
        }
    }
    require(false, "no rule " + std::string(selector));
    return {};
}

double box_width(Tool& tool, int node) {
    return at(tool.result("DOM.getBoxModel", {{"nodeId", node}}), "model").number_at("width");
}

double box_height(Tool& tool, int node) {
    return at(tool.result("DOM.getBoxModel", {{"nodeId", node}}), "model").number_at("height");
}

std::string replaced(std::string text, std::string_view from, std::string_view to) {
    const auto found = text.find(from);
    require(found != std::string::npos, "no " + std::string(from) + " in " + text);
    return text.replace(found, from.size(), to);
}

// The sheet edited from the tools (docs/devtools.md, "热重载"): the scene is made again, the scripts' state stays.
void check_sheet_edits(Tool& tool, weft::View& view) {
    const auto original = tool.result("CSS.getStyleSheetText", {{"styleSheetId", "weft-sheet"}}).string_at("text");
    const int five = query(tool, "#five");
    view.evaluate("window.kept = 41");
    tool.result("CSS.enable");
    require(tool.take("CSS.styleSheetAdded", "weft-sheet").has_value(), "the page's sheet");
    tool.settle(2);
    tool.clear();

    // A declaration's value.
    auto rule = rule_style(tool, five, ".multi");
    auto style = at(rule, "style");
    require(box_width(tool, five) == 80, "the width the sheet gives");
    auto text = replaced(style.string_at("cssText"), "80px", "90px");
    auto edited = items(
        tool.result("CSS.setStyleTexts",
                    {{"edits", Json::Array{Json::Object{
                                   {"styleSheetId", "weft-sheet"}, {"range", at(style, "range")}, {"text", text}}}}}),
        "styles")[0];
    require(edited.string_at("cssText") == text, "the edited block's text");
    const auto& first = items(edited, "cssProperties")[0];
    require(first.string_at("name") == "width" && first.string_at("value") == "90px" && first.find("range"),
            "the edited declaration: " + first.dump());
    require(box_width(tool, five) == 90, "the scene is made with the edited sheet");
    require(view.evaluate("String(window.kept)").second == "41", "the scripts' state stays");
    require(tool.take("CSS.styleSheetChanged").has_value(), "the tools are told the sheet changed");
    require(tool.result("CSS.getStyleSheetText", {{"styleSheetId", "weft-sheet"}}).string_at("text") ==
                replaced(original, "width: 80px;\n  height: 16px", "width: 90px;\n  height: 16px"),
            "the sheet's text is the edited one");
    // The rule asked for again is what the edit answered, at the same place.
    style = at(rule_style(tool, five, ".multi"), "style");
    require(style.dump() == edited.dump(), "the rule read again: " + style.dump() + " and " + edited.dump());

    // A declaration turned off is a comment in its place, and is read back as a disabled one.
    text = replaced(text, "width: 90px;", "/* width: 90px; */");
    edited = items(tool.result("CSS.setStyleTexts", {{"edits", Json::Array{Json::Object{{"styleSheetId", "weft-sheet"},
                                                                                        {"range", at(style, "range")},
                                                                                        {"text", text}}}}}),
                   "styles")[0];
    const auto& off = items(edited, "cssProperties")[0];
    require(off.string_at("name") == "width" && off.bool_at("disabled") &&
                off.string_at("text") == "/* width: 90px; */" && off.find("range"),
            "a disabled declaration: " + off.dump());
    require(items(edited, "cssProperties")[1].string_at("name") == "height", "the others follow it");
    require(box_width(tool, five) == 800, "without its width the box stretches");

    // The whole sheet, as the Sources panel saves it.
    tool.result("CSS.setStyleSheetText", {{"styleSheetId", "weft-sheet"}, {"text", original}});
    require(box_width(tool, five) == 80, "the sheet as it was");

    // A rule's selector.
    const int six = query(tool, "#six");
    rule = rule_style(tool, six, ".only-unknown");
    const auto& selector = items(at(rule, "selectorList"), "selectors")[0];
    require(selector.find("range") != nullptr, "a selector has its place: " + rule.dump());
    const auto list = at(tool.result("CSS.setRuleSelector", {{"styleSheetId", "weft-sheet"},
                                                             {"range", at(selector, "range")},
                                                             {"selector", ".only-unknown, .extra"}}),
                         "selectorList");
    require(list.string_at("text") == ".only-unknown, .extra" && items(list, "selectors").size() == 2 &&
                items(list, "selectors")[1].find("range"),
            "the edited selector list: " + list.dump());
    const auto with_selector = tool.result("CSS.getStyleSheetText", {{"styleSheetId", "weft-sheet"}}).string_at("text");
    // One the sheet does not take changes nothing.
    const auto refused = tool.call(
        "CSS.setRuleSelector",
        {{"styleSheetId", "weft-sheet"}, {"range", at(items(list, "selectors")[1], "range")}, {"selector", "..bad"}});
    require(refused.find("error") != nullptr, "a selector that is none is refused");
    require(tool.result("CSS.getStyleSheetText", {{"styleSheetId", "weft-sheet"}}).string_at("text") == with_selector,
            "and the sheet is as it was");

    // A rule added: a page has one sheet, the rule goes to its end.
    require(tool.result("CSS.createStyleSheet", {{"frameId", "weft-main"}}).string_at("styleSheetId") == "weft-sheet",
            "the sheet rules are added to");
    const auto lines = double(std::count(with_selector.begin(), with_selector.end(), '\n'));
    const auto column = double(with_selector.size() - (with_selector.rfind('\n') + 1));
    const auto added = at(
        tool.result(
            "CSS.addRule",
            {{"styleSheetId", "weft-sheet"},
             {"ruleText", "#five { height: 33px; }"},
             {"location",
              Json::Object{{"startLine", lines}, {"startColumn", column}, {"endLine", lines}, {"endColumn", column}}}}),
        "rule");
    require(at(added, "selectorList").string_at("text") == "#five" &&
                items(at(added, "style"), "cssProperties")[0].string_at("value") == "33px",
            "the added rule: " + added.dump());
    require(box_height(tool, five) == 33, "the added rule applies");
    tool.result("CSS.setStyleSheetText", {{"styleSheetId", "weft-sheet"}, {"text", original}});
    require(box_height(tool, five) == 16, "the sheet as it was again");

    // A style attribute's declaration turned off: the attribute holds the comment, as Chrome's does.
    const int two = query(tool, "#two");
    const auto before = at(tool.result("CSS.getInlineStylesForNode", {{"nodeId", two}}), "inlineStyle");
    const auto commented =
        items(tool.result("CSS.setStyleTexts",
                          {{"edits", Json::Array{Json::Object{{"styleSheetId", before.string_at("styleSheetId")},
                                                              {"range", at(before, "range")},
                                                              {"text", "/* height: 26px; */ width: 50px"}}}}}),
              "styles")[0];
    const auto& declared = items(commented, "cssProperties");
    require(declared.size() == 2 && declared[0].bool_at("disabled") && declared[0].string_at("name") == "height" &&
                declared[1].string_at("name") == "width" && !declared[1].bool_at("disabled") &&
                declared[1].bool_at("parsedOk"),
            "a style attribute with a comment: " + commented.dump());
    require(box_width(tool, two) == 50 && box_height(tool, two) == 20, "the attribute applies without its comment");
    tool.result("CSS.setStyleTexts",
                {{"edits", Json::Array{Json::Object{{"styleSheetId", before.string_at("styleSheetId")},
                                                    {"range", at(commented, "range")},
                                                    {"text", before.string_at("cssText")}}}}});
    require(box_height(tool, two) == 26, "the attribute as it was");
    require(view.evaluate("String(window.kept)").second == "41", "the scripts' state stayed through it all");
}

// The page as it is, for laying it out elsewhere, and what it was loaded with.
void check_snapshot(Tool& tool) {
    const auto snapshot = tool.result("Weft.getSnapshot");
    require(snapshot.number_at("width") == 800 && snapshot.number_at("height") == 600, "the viewport");
    const auto& tree = at(snapshot, "tree");
    require(tree.string_at("tag") == "div" && at(tree, "attributes").string_at("id") == "root", "the scene's root");
    const Json* one = nullptr;
    const Json* five = nullptr;
    for (const auto& child : items(tree, "children")) {
        if (const auto* attributes = child.find("attributes")) {
            one = attributes->string_at("id") == "one" ? &child : one;
            five = attributes->string_at("id") == "five" ? &child : five;
        }
    }
    require(one && five, "the root's elements");
    const auto& rect = items(*one, "rect");
    require(*rect[0].number() == 6 && *rect[1].number() == 2 && *rect[2].number() == 100 && *rect[3].number() == 20,
            "an element's box: " + one->dump());
    require(items(*one, "children").size() == 1 && items(*one, "children")[0].find("text"), "its text");
    require(at(*five, "unsupported").dump().find(".multi { column-count: 2 }") != std::string::npos,
            "what the engine does not take of an element's rules: " + five->dump());
    require(snapshot.string_at("stylesheet").find(".multi") != std::string::npos, "the sheet");

    const auto page = tool.result("Weft.getPage");
    require(items(page, "scripts")[0].string_at("name") == "page.js" && page.bool_at("forgivingCss") &&
                page.string_at("base") == "/fixtures/devtools/page.css" && !page.bool_at("supplied"),
            "the page as loaded");
}

const weft::SceneNode* find_class(const weft::SceneNode& node, std::string_view name) {
    if (const auto found = node.attributes.find("class"); found != node.attributes.end() && found->second == name) {
        return &node;
    }
    for (const auto& child : node.children) {
        if (const auto* inner = find_class(child, name)) {
            return inner;
        }
    }
    return nullptr;
}

// A page of the strict profile, where a sheet can fail: the sheet in place stays; the page's files from the tools;
// and a page loaded from the tools into this View (without a host that can try one aside).
void check_strict_page(Tool& tool, weft::View& view) {
    tool.result("DOM.getDocument");
    const auto sheet = tool.result("CSS.getStyleSheetText", {{"styleSheetId", "weft-sheet"}}).string_at("text");
    const int a = query(tool, ".a");
    require(box_width(tool, a) == 10, "the strict page's box");
    view.evaluate("window.kept = 5");

    const auto refused = tool.call("CSS.setStyleSheetText",
                                   {{"styleSheetId", "weft-sheet"}, {"text", sheet + " .a { column-count: 2; }"}});
    require(refused.find("error") && refused.dump().find("column-count") != std::string::npos,
            "a sheet the scene cannot be made with is refused: " + refused.dump());
    require(tool.result("CSS.getStyleSheetText", {{"styleSheetId", "weft-sheet"}}).string_at("text") == sheet &&
                box_width(tool, a) == 10,
            "and the sheet in place stays");
    const auto pushed = tool.result("Weft.setStyleSheet", {{"text", sheet + " .a { column-count: 2; }"}});
    require(!pushed.bool_at("applied") && pushed.string_at("error").find("column-count") != std::string::npos,
            "a sheet pushed that fails says why: " + pushed.dump());
    const auto wider = replaced(sheet, "width: 10px", "width: 12px");
    require(tool.result("Weft.setStyleSheet", {{"text", wider}}).bool_at("applied") && box_width(tool, a) == 12,
            "a sheet pushed that the scene takes");
    require(tool.result("Weft.getPage").bool_at("supplied"), "and it is the page's from now on");
    require(view.evaluate("String(window.kept)").second == "5", "the scripts' state stays");

    // The page names a file its host does not have: the tools supply it.
    const auto* box = find_class(view.scene().root, "a");
    require(box && !box->background_image, "no image without its file");
    const auto count = tool.result(
        "Weft.setResources",
        {{"resources", Json::Array{Json::Object{{"path", "dot.png"},
                                                {"data", "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4"
                                                         "2mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg=="}}}}});
    require(count.number_at("resources") == 1, "one file taken");
    tool.settle(2);
    box = find_class(view.scene().root, "a");
    require(box && box->background_image && box->background_image->width == 1, "the image from the tools' file");
    require(view.evaluate("String(window.kept)").second == "5", "and the scripts' state stays");

    // A page from the tools: the tool's connection is told of the new document.
    tool.clear();
    const auto loaded = tool.result(
        "Weft.setPage",
        {{"page", Json::Object{{"scripts", Json::Array{Json::Object{
                                               {"name", "fresh.js"},
                                               {"source", "var d = document.createElement('div'); d.id = 'fresh';"
                                                          "document.getElementById('root').appendChild(d);"}}}}}}});
    require(loaded.bool_at("loaded"), "a page from the tools: " + loaded.dump());
    require(tool.take("DOM.documentUpdated").has_value(), "its document");
    require(view.evaluate("String(!!document.getElementById('fresh')) + String(window.kept)").second == "trueundefined",
            "the new page, its scripts' state new");
    require(box_width(tool, query(tool, "#fresh")) == 100, "with the sheet pushed before, which is the page's");
    const auto broken = tool.result(
        "Weft.setPage",
        {{"page", Json::Object{{"scripts", Json::Array{Json::Object{{"name", "broken.js"},
                                                                    {"source", "throw new Error('broken page')"}}}}}}});
    require(!broken.bool_at("loaded") && broken.string_at("error").find("broken page") != std::string::npos,
            "a page that does not load says why: " + broken.dump());
}

int main() {
    try {
        auto fonts = catalog();
        weft::SceneText text(*fonts);
        weft::View view(fonts, text, 800, 600, 1);
        weft::ViewPage page;
        page.stylesheet = weft::testing::read_file(directory + "page.css");
        page.base = "/fixtures/devtools/page.css";
        page.scripts.push_back({"page.js", weft::testing::read_file(directory + "page.js")});
        page.forgiving_css = true;
        page.web_platform = true;
        view.load(page);
        view.evaluate("console.log('before the tools')");

        // A second View, of the strict profile, whose page names a file its host does not have.
        weft::View strict(fonts, text, 400, 300, 1);
        weft::ViewPage strict_page;
        strict_page.stylesheet = "#root { width: 400px; height: 300px; } "
                                 ".a { width: 10px; height: 10px; background-image: url(dot.png); } "
                                 "#fresh { width: 100px; height: 4px; }";
        strict_page.base = "/fixtures/strict/page.css";
        strict_page.scripts.push_back({"strict.js", "var a = document.createElement('div'); a.className = 'a';"
                                                    "document.getElementById('root').appendChild(a);"});
        strict.load(strict_page);

        auto started = weft::devtools::Server::start({});
        require(started.has_value(), "the socket");
        auto server = *started;
        const auto port = server->port();
        weft::devtools::Agent agent(view, server, "target");
        weft::devtools::Agent strict_agent(strict, server, "strict");
        double time = 0;
        const Step step = [&] {
            time += 16;
            view.advance(time);
            agent.pump();
            strict.advance(time);
            strict_agent.pump();
        };
        step();

        using weft::testing::devtools_http_get;
        const auto list = devtools_http_get(port, "/json/list");
        require(list.find("/devtools/page/target") != std::string::npos &&
                    list.find("/devtools/page/strict") != std::string::npos &&
                    list.find("devtools://devtools") != std::string::npos,
                "the list of pages: " + list);
        require(devtools_http_get(port, "/json/version").find("Protocol-Version") != std::string::npos, "the version");
        require(devtools_http_get(port, "/nothing").find("404") != std::string::npos, "anything else");

        Tool tool(step, port, "/devtools/page/target");
        require(tool.head().find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos,
                "the handshake of RFC 6455: " + tool.head());

        check_document(tool);
        check_against_chrome(tool);
        check_unsupported(tool);
        check_styles(tool, view);
        check_sheet_edits(tool, view);
        check_snapshot(tool);
        check_mutations(tool, view);
        check_console(tool, view);
        check_late_tools(step, port, view);
        check_overlay(tool, view);
        check_picking(tool, view);
        check_errors(tool, port, step);
        {
            Tool strict_tool(step, port, "/devtools/page/strict");
            check_strict_page(strict_tool, strict);
        }

        // A new page tells the tool.
        tool.clear();
        view.load(page);
        require(tool.take("DOM.documentUpdated").has_value(), "a new page's document");
        require(tool.take("Runtime.executionContextsCleared").has_value(), "a new page's script context");

        server->stop();
        std::cout << "devtools protocol: ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
