#include "layout/positioned_page.hpp"
#include "support/check.hpp"
#include "constants/layout.hpp"
#include "test_constants/layout.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace weft;

namespace {
    using weft::testing::require;

    void near(float a, float b) {
        require(std::abs(a - b) < weft::test_constants::layout::positioned_geometry_tolerance,
                "geometry/UV discrepancy");
    }

    DomSnapshot node(std::string name, std::string id, std::string style) {
        DomSnapshot n;
        n.type = weft::constants::element_node_type;
        n.namespace_uri = weft::constants::xhtml_namespace_uri;
        n.local_name = std::move(name);
        n.attributes = {{"id", std::move(id)}, {"style", std::move(style)}};
        return n;
    }

    template <class F>
    void rejects(F f) {
        try {
            f();
        } catch (const std::invalid_argument& e) {
            require(std::string(e.what()).starts_with("WEFT_POSITIONED_PROFILE:"), "diagnostic category");
            return;
        }
        throw std::runtime_error("unsupported CSS silently accepted");
    }
} // namespace

int main() {
    try {
        Font font(WEFT_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf");
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
        const auto page = build_positioned_page(root, sheet, 200, 100, font);
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
        near(page.geometry.boxes[3].x, 0);
        near(page.geometry.boxes[3].y, 0); // static bridge cannot become absolute containing block.
        root.children.clear();
        auto inset = node("div", "inset", "position:absolute;inset:0;left:calc(100% - 70px);width:50%;height:50%");
        root.children.push_back(inset);
        auto resolved = build_positioned_page(root, "", 200, 100, font);
        near(resolved.geometry.boxes[1].x, 130);
        near(resolved.geometry.boxes[1].width, 100);
        near(resolved.geometry.boxes[1].height, 50);
        root.children[0].attributes["style"] = "position:absolute;inset:10px;background-color:#ffff00;z-index:-1";
        auto positive = node("div", "positive", "position:absolute;inset:0;z-index:2;background-color:#00ff00");
        root.children.insert(root.children.begin(), positive);
        auto stacked = build_positioned_page(root, "", 200, 100, font);
        require(stacked.paints[0].paint.id == "root" && stacked.paints[1].paint.id == "inset" &&
                    stacked.paints[2].paint.id == "positive",
                "negative stacking must stay above isolated parent background and below positive sibling");
        auto copy = stacked;
        root.children.clear();
        require(copy.paints.size() == 3, "paint snapshot borrows DOM");
        auto hidden = node("div", "hidden", "display:none");
        hidden.children.push_back(node("i", "hidden-child", ""));
        root.children.push_back(hidden);
        auto suppressed = build_positioned_page(root, "", 200, 100, font);
        require(suppressed.geometry.boxes.size() == 3 && suppressed.paints.size() == 1,
                "display:none must suppress a subtree without requiring sizes");
        near(suppressed.geometry.boxes[2].width, 0);
        near(suppressed.geometry.boxes[2].x, 0);
        root.children.clear();
        for (const auto* css :
             {"#root:hover {color:#ffffff}", "@media screen {#root {display:block}}", "#root {display:grid}",
              "#root {padding:2px}", "#root {opacity:.5}", "#root {right:calc(100%*2)}", "#root {right:NaNpx}",
              "#root {overflow:auto}", "#root {z-index:1}"}) {
            rejects([&] {
                build_positioned_page(root, css, 200, 100, font);
            });
        }
        root.children.push_back(node("div", "bad", "position:absolute;width:10px"));
        rejects([&] {
            build_positioned_page(root, "", 200, 100, font);
        });
        FontSet fonts({WEFT_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf",
                       WEFT_SOURCE_DIR "/third_party/noto-cjk/source/NotoSansCJKsc-Regular.otf"});
        auto label = node(
            "div", "mixed",
            "width:200px;height:24px;font-family:ProbeRoboto, ProbeCjk;font-size:16px;line-height:24px;white-space:nowrap");
        DomSnapshot text;
        text.type = weft::constants::text_node_type;
        text.data = "A仓B";
        label.children.push_back(text);
        auto mixed = build_positioned_page(label, "", 200, 24, fonts);
        require(mixed.paints[0].paint.shaped.glyphs[1].font_id == 1, "page lost fallback face identity");
        label.children[0].data = "ABC";
        auto latin = build_positioned_page(label, "", 200, 24, fonts);
        near(mixed.paints[0].paint.y, latin.paints[0].paint.y);
        rejects([&] {
            build_positioned_page(label, "", 200, 24, font);
        }); // A fallback family requires its resource set.
        label.local_name = "i";
        rejects([&] {
            build_positioned_page(label, "", 200, 24, fonts);
        });
        label.attributes["style"] += ";display:block;font-style:normal";
        require(build_positioned_page(label, "", 200, 24, fonts).paints.size() == 1, "explicit normal i");
        label.local_name = "li";
        rejects([&] {
            build_positioned_page(label, "", 200, 24, fonts);
        });
        label.attributes["style"] += ";list-style:none";
        require(build_positioned_page(label, "", 200, 24, fonts).paints.size() == 1, "explicit marker suppression");
        label.local_name = "b";
        rejects([&] {
            build_positioned_page(label, "", 200, 24, fonts);
        });
        label.attributes["style"] += ";font-weight:400";
        require(build_positioned_page(label, "", 200, 24, fonts).paints.size() == 1, "explicit normal-weight b");
        label.local_name = "ul";
        rejects([&] {
            build_positioned_page(label, "", 200, 24, fonts);
        });
        std::cout
            << "PASS positioned CSS: cascade/combinators, containing block, inset/calc, nested crop UV, isolated stacking, unsupported diagnostics\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
