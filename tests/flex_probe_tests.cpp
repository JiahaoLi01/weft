#include "caseelephant/flex_probe.hpp"
#include "fixtures/flex_layout_cases.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>

namespace {
    void require(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    const caseelephant::FlexProbeBox& box(const std::vector<caseelephant::FlexProbeBox>& boxes, const char* id) {
        const auto it = std::find_if(boxes.begin(), boxes.end(), [&](const auto& b) {
            return b.id == id;
        });
        if (it == boxes.end()) {
            throw std::runtime_error("Missing box");
        }
        return *it;
    }

    const caseelephant::FlexProbeNode& input(const std::vector<caseelephant::fixtures::Case>& cases, const char* id) {
        const auto it = std::find_if(cases.begin(), cases.end(), [&](const auto& c) {
            return c.id == id;
        });
        if (it == cases.end()) {
            throw std::runtime_error("Missing case");
        }
        return it->root;
    }
} // namespace

int main() {
    try {
        caseelephant::TextProbeFont font(CE_SOURCE_DIR "/third_party/ahem/Ahem.ttf");
        const auto cases = caseelephant::fixtures::flex_cases();
        auto fixed = caseelephant::run_flex_probe(input(cases, "fixed-text-final-wrap"), font);
        require(box(fixed, "text").measure_calls == 0, "definite dimensions unexpectedly required a measure callback");
        require(box(fixed, "text").text_lines == 2 && box(fixed, "text").text_height == 40,
                "final text relied on skipped measure callback");
        auto changed = input(cases, "fixed-text-final-wrap");
        changed.children[0].style["width"] = "60px";
        auto narrower = caseelephant::run_flex_probe(changed, font);
        require(box(narrower, "text").text_lines == 3, "changed final width retained stale lines");
        changed.children[0].style["font-size"] = "40px";
        changed.children[0].style["line-height"] = "40px";
        auto larger = caseelephant::run_flex_probe(changed, font);
        require(box(larger, "text").text_width == 80 && box(larger, "text").text_height == 120,
                "changed font metrics retained stale text");
        auto zero = caseelephant::run_flex_probe(input(cases, "basis-zero-indefinite-column"), font);
        auto percent = caseelephant::run_flex_probe(input(cases, "basis-percent-indefinite-column"), font);
        require(box(zero, "root").height == 20 && box(percent, "root").height == 40,
                "0px and indefinite 0% basis collapsed to one semantic");
        require(box(zero, "text").height == 0 && box(zero, "text").text_height == 20,
                "zero-size text box lost overflow content layout");
        const auto content_cap = caseelephant::run_flex_probe(input(cases, "auto-min-max-content-box-edges"), font);
        const auto border_cap = caseelephant::run_flex_probe(input(cases, "auto-min-max-border-box-edges"), font);
        require(box(content_cap, "word").width == 84 && box(border_cap, "word").width == 60,
                "max-size auto minimum used the wrong box");
        const auto bad = [&](caseelephant::FlexProbeNode n, const char* expected) {
            bool rejected{};
            try {
                (void)caseelephant::run_flex_probe(n, font);
            } catch (const std::exception& e) {
                rejected = std::string(e.what()).find(expected) != std::string::npos;
            }
            require(rejected, "unsupported/invalid input silently accepted");
        };
        caseelephant::FlexProbeNode nested{
            "root",
            {{"display", "flex"}, {"width", "120px"}, {"height", "20px"}, {"flex-direction", "column"}},
            "",
            {{"wrapper",
              {{"display", "flex"}, {"height", "20px"}, {"flex-direction", "column"}},
              "",
              {{"text", {{"font-size", "20px"}, {"line-height", "20px"}, {"white-space", "nowrap"}}, "X X", {}}}}}};
        require(box(caseelephant::run_flex_probe(nested, font), "text").text_height == 20,
                "nowrap auto min-height incorrectly requires declared parent width");
        nested.children[0].children[0].style["white-space"] = "normal";
        bad(nested, "CE_UNSUPPORTED_INTRINSIC");
        auto root = input(cases, "web-row-default");
        root.style["display"] = "block";
        bad(root, "CE_UNSUPPORTED_FORMAT");
        root = input(cases, "web-row-default");
        root.style["padding-magic"] = "10px";
        bad(root, "CE_UNSUPPORTED_PROPERTY");
        root = input(cases, "web-row-default");
        root.children[0].style["flex"] = "1";
        root.children[0].style["flex-grow"] = "2";
        bad(root, "CE_UNSUPPORTED_STYLE_CONFLICT");
        root = input(cases, "web-row-default");
        root.style["align-items"] = "auto";
        bad(root, "CE_UNSUPPORTED_ALIGN");
        root = input(cases, "web-row-default");
        root.children[0].style["align-self"] = "space-between";
        bad(root, "CE_UNSUPPORTED_ALIGN");
        root = input(cases, "web-row-default");
        root.style["width"] = "-1px";
        bad(root, "CE_CSS_VALUE");
        root = input(cases, "web-row-default");
        root.style["width"] = "10vw";
        bad(root, "CE_UNSUPPORTED_UNIT");
        root = input(cases, "web-row-default");
        root.children[0].id = "root";
        bad(root, "CE_LAYOUT_ID");
        root = input(cases, "web-row-default");
        root.children[0].text = "\xE4\xB8\xAD";
        bad(root, "CE_UNSUPPORTED_TEXT_BREAK");
        require(caseelephant::run_flex_probe(input(cases, "web-row-default"), font).size() == 3,
                "failed input corrupted subsequent solve");
        std::cout
            << "PASS final text after skipped measure, width/font changes, basis distinction and explicit diagnostics\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
