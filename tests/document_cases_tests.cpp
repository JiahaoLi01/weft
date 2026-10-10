#include "layout/document.hpp"
#include "support/check.hpp"
#include "constants/layout.hpp"
#include "test_constants/layout.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace weft;

// Document formatter cases carried over from the retired positioned and auto profiles: selector chains and
// specificity, absolute containing blocks, inset/calc, nested crop UV, isolated stacking, display:none, font fallback
// identity, UA defaults that need explicit overrides, padding shorthands and the diagnostics of the supported subset.
namespace {
    using weft::testing::require;

    void near(float a, float b) {
        require(std::abs(a - b) < weft::test_constants::layout::document_geometry_tolerance, "document geometry/UV");
    }

    DomSnapshot node(std::string name, std::string id, std::string style, std::string text = {}) {
        DomSnapshot n;
        n.type = weft::constants::element_node_type;
        n.namespace_uri = weft::constants::xhtml_namespace_uri;
        n.local_name = std::move(name);
        n.attributes = {{"id", std::move(id)}, {"style", std::move(style)}};
        if (!text.empty()) {
            DomSnapshot t;
            t.type = weft::constants::text_node_type;
            t.data = std::move(text);
            n.children.push_back(std::move(t));
        }
        return n;
    }

    template <class F>
    void rejects(F f) {
        try {
            f();
        } catch (const std::invalid_argument& e) {
            require(std::string(e.what()).starts_with("WEFT_"), "layout diagnostic");
            return;
        }
        throw std::runtime_error("unsupported CSS silently accepted");
    }
} // namespace

int main() {
    try {
        FontSet fonts({WEFT_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf",
                       WEFT_SOURCE_DIR "/third_party/noto-cjk/source/NotoSansCJKsc-Regular.otf"});
        auto root = node("div", "root",
                         "position:relative;width:200px;height:100px;background-color:#0000ff;isolation:isolate");
        root.attributes["class"] = "surface";
        auto outer =
            node("div", "outer", "position:absolute;left:20px;top:10px;width:80px;height:60px;overflow:hidden");
        outer.attributes["class"] = "wrap";
        auto bridge = node("div", "bridge", "height:60px");
        auto image = node("img", "image", "position:absolute;left:-20px;top:-10px;width:200px;height:120px");
        image.attributes["src"] = "art/example.png";
        bridge.children.push_back(image);
        outer.children.push_back(bridge);
        root.children.push_back(outer);
        const std::string sheet =
            ".surface {background-color:#ff0000} #root {background-color:#00ff00} .wrap IMG {pointer-events:none} #outer > DIV#bridge {background-color:#123456}";
        const auto page = layout_document(root, sheet, 200, 100, fonts).page;
        require(page.geometry.boxes.size() == 4 && page.paints.size() == 3, "real selector chain did not match");
        require(page.paints[0].paint.srgb[2] == 1 && page.paints[0].paint.srgb[0] == 0,
                "inline did not beat specificity");
        const auto& p = page.paints.back();
        require(p.image_source == "art/example.png", "image identity must stay a value");
        near(p.paint.x, 20);
        near(p.paint.y, 10);
        near(p.paint.width, 80);
        near(p.paint.height, 60);
        near(p.uv[0], .1f);
        near(p.uv[1], 10 / 120.f);
        near(p.uv[2], .4f);
        near(p.uv[3], .5f);
        // A static ancestor is not a containing block: the image is placed against #outer.
        near(page.geometry.boxes[3].x, 0);
        near(page.geometry.boxes[3].y, 0);
        root.children.clear();
        auto inset = node("div", "inset", "position:absolute;inset:0;left:calc(100% - 70px);width:50%;height:50%");
        root.children.push_back(inset);
        auto resolved = layout_document(root, "", 200, 100, fonts).page;
        near(resolved.geometry.boxes[1].x, 130);
        near(resolved.geometry.boxes[1].width, 100);
        near(resolved.geometry.boxes[1].height, 50);
        root.children[0].attributes["style"] = "position:absolute;inset:10px;background-color:#ffff00;z-index:-1";
        auto positive = node("div", "positive", "position:absolute;inset:0;z-index:2;background-color:#00ff00");
        root.children.insert(root.children.begin(), positive);
        auto stacked = layout_document(root, "", 200, 100, fonts).page;
        require(stacked.paints[0].paint.id == "root" && stacked.paints[1].paint.id == "inset" &&
                    stacked.paints[2].paint.id == "positive",
                "negative stacking must stay above isolated parent background and below positive sibling");
        auto copy = stacked;
        root.children.clear();
        require(copy.paints.size() == 3, "paint snapshot borrows DOM");
        auto hidden = node("div", "hidden", "display:none");
        hidden.children.push_back(node("i", "hidden-child", ""));
        root.children.push_back(hidden);
        auto suppressed = layout_document(root, "", 200, 100, fonts).page;
        require(suppressed.geometry.boxes.size() == 3 && suppressed.paints.size() == 1,
                "display:none must suppress a subtree without requiring sizes");
        near(suppressed.geometry.boxes[2].width, 0);
        near(suppressed.geometry.boxes[2].x, 0);
        root.children.clear();
        for (const auto* css : {"@media screen {#root {display:block}}", "#root {display:grid}", "#root {opacity:.5}",
                                "#root {right:calc(100%*2)}", "#root {right:NaNpx}", "#root {z-index:1}"}) {
            rejects([&] {
                layout_document(root, css, 200, 100, fonts);
            });
        }
        auto label = node(
            "div", "mixed",
            "width:200px;height:24px;font-family:ProbeRoboto, ProbeCjk;font-size:16px;line-height:24px;white-space:nowrap",
            "A仓B");
        auto mixed = layout_document(label, "", 200, 24, fonts).page;
        require(mixed.paints[0].paint.shaped.glyphs[1].font_id == 1, "page lost fallback face identity");
        label.children[0].data = "ABC";
        auto latin = layout_document(label, "", 200, 24, fonts).page;
        near(mixed.paints[0].paint.y, latin.paints[0].paint.y);
        // UA defaults that the supported subset does not model must be overridden explicitly.
        label.local_name = "i";
        rejects([&] {
            layout_document(label, "", 200, 24, fonts);
        });
        label.attributes["style"] += ";display:block;font-style:normal";
        require(layout_document(label, "", 200, 24, fonts).page.paints.size() == 1, "explicit normal i");
        label.local_name = "li";
        rejects([&] {
            layout_document(label, "", 200, 24, fonts);
        });
        label.attributes["style"] += ";list-style:none";
        require(layout_document(label, "", 200, 24, fonts).page.paints.size() == 1, "explicit marker suppression");
        label.local_name = "b";
        rejects([&] {
            layout_document(label, "", 200, 24, fonts);
        });
        label.attributes["style"] += ";font-weight:400";
        require(layout_document(label, "", 200, 24, fonts).page.paints.size() == 1, "explicit normal-weight b");
        label.local_name = "ul";
        rejects([&] {
            layout_document(label, "", 200, 24, fonts);
        });
        label.attributes["style"] += ";margin:0;padding:0";
        require(layout_document(label, "", 200, 24, fonts).page.paints.size() == 1, "explicit list box edges");

        // Block flow, padding shorthands and intrinsic CJK measure.
        const std::string base =
            "display:block;position:relative;width:200px;font-family:ProbeRoboto, ProbeCjk;font-size:16px;line-height:20px;white-space:nowrap";
        auto box = node("div", "root", base + ";padding:2px 3px");
        box.children.push_back(node("div", "a", "display:block", "仓"));
        box.children.push_back(node("div", "b", "display:block;padding:1px 4px", "ABC"));
        box.children.push_back(
            node("div", "abs", "display:block;position:absolute;left:0;top:0;width:100px;height:999px"));
        auto flow = layout_document(box, "", 300, 200, fonts).page;
        near(flow.geometry.boxes[0].height, 46);
        near(flow.geometry.boxes[0].width, 206);
        near(flow.geometry.boxes[1].x, 3);
        near(flow.geometry.boxes[2].y, 22);
        auto auto_width = box;
        auto_width.attributes["style"] += ";width:auto";
        near(layout_document(auto_width, "", 300, 200, fonts).page.geometry.boxes[0].width, 300);
        require(flow.paints[0].paint.shaped.glyphs[0].font_id == 1, "CJK intrinsic measure/fallback paint");
        auto owned = flow;
        box.children.clear();
        require(owned.paints.size() == 2 && owned.paints[0].paint.text == "仓", "borrowed layout output");
        box.children.push_back(node("div", "a", "display:block", "仓"));
        auto style = compute_style(box, "#a{padding:var(--pad);--pad:1px 2px 3px 4px;padding-left:9px}",
                                   document_style_profile());
        require(style.children[0].values.at("padding-bottom") == "3px" &&
                    style.children[0].values.at("padding-left") == "9px",
                "shorthand substitution/longhand cascade");
        const auto invalid =
            compute_style(box, "#a{padding:9px;padding:var(--pad);--pad:2px invalid}", document_style_profile());
        for (const auto* k : {"padding-top", "padding-left", "padding-right", "padding-bottom"}) {
            require(invalid.children[0].values.at(k) == "0",
                    "invalid shorthand must unset every edge without recascade");
        }
        auto percent = box;
        percent.children[0].attributes["style"] = "display:block;height:50%";
        rejects([&] {
            layout_document(percent, "", 300, 200, fonts);
        });
        for (const auto* bad : {"margin:1px", "display:grid", "padding:2%", "display:block;gap:2px", "font-weight:700",
                                "width:calc(100% - 2px)"}) {
            auto n = node("div", "bad", base + ';' + bad, "仓");
            rejects([&] {
                layout_document(n, "", 300, 200, fonts);
            });
        }
        auto stale = style;
        stale.identity.generation++;
        rejects([&] {
            layout_document(box, stale, 300, 200, fonts);
        });
        std::cout
            << "PASS document CSS: cascade/combinators, containing blocks, inset/calc, nested crop UV, isolated "
               "stacking, display:none, fallback identity, explicit UA overrides, block padding and diagnostics\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
