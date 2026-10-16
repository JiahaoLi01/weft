#include "script/script_dom.hpp"
#include "support/check.hpp"
#include "support/files.hpp"
#include "support/json_value.hpp"

#include <array>
#include <iostream>
#include <string>

// d3-sankey 0.12.3 (third_party/d3-sankey) over D3 7.9.0, both unmodified, in weft's QuickJS and native DOM: the shared
// fixture tests/fixtures/d3_sankey_probe.js lays a month of a good's flow out and draws it as svg, and its report (the
// layout's numbers, and the svg as a node tree: every path's d and stroke-width, every rect, every label) is Chrome's
// exactly (tests/fixtures/d3-sankey-reference.json, tools/browser_d3_sankey_reference.mjs), which was recorded from the
// same three scripts (their sha256, given by the build). How weft paints that svg is the "sankey" case of
// scene_paint_reference.
namespace {
    using weft::testing::JsonValue;
    using weft::testing::require;

    // The same value: kinds, strings and numbers exactly (both engines print and parse the shortest round trip), arrays
    // and objects member by member in order.
    void same(const JsonValue& weft, const JsonValue& chrome, const std::string& at) {
        require(weft.kind == chrome.kind, at + ": a value of another kind");
        switch (chrome.kind) {
            case JsonValue::Kind::Null:
                return;
            case JsonValue::Kind::Bool:
                require(weft.boolean == chrome.boolean, at + ": " + (weft.boolean ? "true" : "false"));
                return;
            case JsonValue::Kind::Number:
                require(weft.number == chrome.number,
                        at + ": " + std::to_string(weft.number) + " against Chrome's " + std::to_string(chrome.number));
                return;
            case JsonValue::Kind::String:
                require(weft.string == chrome.string,
                        at + ": \"" + weft.string + "\" against Chrome's \"" + chrome.string + "\"");
                return;
            case JsonValue::Kind::Array:
                require(weft.array.size() == chrome.array.size(), at + ": " + std::to_string(weft.array.size()) +
                                                                      " items against Chrome's " +
                                                                      std::to_string(chrome.array.size()));
                for (std::size_t i = 0; i < chrome.array.size(); ++i) {
                    same(weft.array[i], chrome.array[i], at + "[" + std::to_string(i) + "]");
                }
                return;
            case JsonValue::Kind::Object:
                require(weft.object.size() == chrome.object.size(), at + ": other members");
                for (std::size_t i = 0; i < chrome.object.size(); ++i) {
                    require(weft.object[i].first == chrome.object[i].first,
                            at + ": member " + weft.object[i].first + " against Chrome's " + chrome.object[i].first);
                    same(weft.object[i].second, chrome.object[i].second, at + "." + chrome.object[i].first);
                }
                return;
        }
    }
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 4,
                "usage: weft_d3_sankey_tests <d3.js sha256> <d3-sankey.js sha256> <d3_sankey_probe.js sha256>");
        const std::array<const char*, 3> scripts{"third_party/d3/d3.js", "third_party/d3-sankey/d3-sankey.js",
                                                 "tests/fixtures/d3_sankey_probe.js"};
        const auto reference =
            weft::testing::parse_json(weft::testing::read_source_file("tests/fixtures/d3-sankey-reference.json"));
        // Chrome's report is of these scripts.
        for (std::size_t i = 0; i < scripts.size(); ++i) {
            const auto* recorded = reference["scripts"].find(scripts[i]);
            require(recorded && recorded->string == argv[i + 1],
                    std::string("Chrome's report is of another ") + scripts[i] +
                        ": record it again with tools/browser_d3_sankey_reference.mjs");
        }
        weft::ScriptDom page;
        std::string report;
        for (const auto* script : scripts) {
            report = page.eval(weft::testing::read_source_file(script), script);
        }
        const auto ours = weft::testing::parse_json(report);
        same(ours, reference["report"], "report");
        require(ours["checks_passed"].number == 7 && ours["nodes"].array.size() == 12 &&
                    ours["svg"]["children"].array.size() == 3,
                "the fixture's checks, nodes and svg groups");
        page.collect_garbage();
        std::cout << "PASS d3-sankey 0.12.3 over D3 7.9.0: layout and svg as Chrome's (" << reference["browser"].string
                  << "), " << ours["nodes"].array.size() << " nodes, " << ours["links"].array.size() << " links\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
