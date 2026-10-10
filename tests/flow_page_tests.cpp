#include "caseelephant/flow_page_probe.hpp"
#include "caseelephant/script_platform.hpp"
#include "support/check.hpp"
#include "support/files.hpp"
#include "test_constants/layout.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
using namespace caseelephant;

namespace {
    using caseelephant::testing::read_text_file;
    using caseelephant::testing::require;

    template <class F>
    void reject(F f) {
        try {
            f();
        } catch (const std::exception& e) {
            require(std::string(e.what()).find("CE_") != std::string::npos, "missing diagnostic");
            return;
        }
        throw std::runtime_error("invalid behavior accepted");
    }

    const ProbeDomSnapshot& find(const ProbeDomSnapshot& n, const std::string& id) {
        if (n.attributes.contains("id") && n.attributes.at("id") == id) {
            return n;
        }
        for (const auto& c : n.children) {
            try {
                return find(c, id);
            } catch (const std::out_of_range&) {
            }
        }
        throw std::out_of_range(id);
    }

    const ProbeElementGeometry& geom(const ProbeFlowPage& p, ProbeNodeIdentity id) {
        for (const auto& e : p.elements) {
            if (e.identity == id) {
                return e;
            }
        }
        throw std::runtime_error("geometry absent");
    }

    void report(std::ostream& out, const ProbeFlowPage& p) {
        out << '[';
        for (std::size_t i = 0; i < p.elements.size(); ++i) {
            const auto& g = p.elements[i];
            const auto& b = p.page.geometry.boxes[i];
            if (i) {
                out << ',';
            }
            out << "{\"id\":\"" << b.id << "\",\"x\":" << g.rect[0] << ",\"y\":" << g.rect[1]
                << ",\"width\":" << g.rect[2] << ",\"height\":" << g.rect[3]
                << ",\"clientWidth\":" << std::round(g.client_width)
                << ",\"clientHeight\":" << std::round(g.client_height)
                << ",\"scrollWidth\":" << std::round(g.scroll_width)
                << ",\"scrollHeight\":" << std::round(g.scroll_height) << ",\"scrollLeft\":" << g.scroll_left
                << ",\"scrollTop\":" << g.scroll_top << ",\"rects\":[";
            for (std::size_t j = 0; j < g.client_rects.size(); ++j) {
                if (j) {
                    out << ',';
                }
                auto r = g.client_rects[j];
                out << '[' << r[0] << ',' << r[1] << ',' << r[2] << ',' << r[3] << ']';
            }
            out << "]}";
        }
        out << ']';
    }
} // namespace

int main(int argc, char** argv) {
    try {
        TextProbeFontSet fonts({CE_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf",
                                CE_SOURCE_DIR "/third_party/noto-cjk/source/NotoSansCJKsc-Regular.otf"});
        auto boundaries = probe_unicode_breaks("A\u00a0B cafe\u0301 中文（运输）");
        require(std::find(boundaries.line.begin(), boundaries.line.end(), 1) == boundaries.line.end(), "NBSP split");
        auto grapheme = probe_unicode_breaks("a\u0301👩‍👩‍👧‍👦🇨🇳");
        require(grapheme.grapheme == std::vector<std::size_t>{3, 28, 36}, "UAX29 combining/ZWJ/regional indicators");
        reject([&] {
            probe_unicode_breaks(std::string("bad\xc0\xaf", 5));
        });
        ProbeInlineInput inline_input;
        inline_input.runs = {{"仓库（运输） cafe\u0301 A\u00a0B", {}, 16, 24, {0, 0, 0, 1}, "normal"}};
        auto narrow = layout_inline_probe(inline_input, 32, fonts);
        require(narrow.lines > 4 && narrow.min_content > 16, "Unicode wrapping intrinsics");
        for (const auto& f : narrow.fragments) {
            require(!f.text.starts_with("）"), "closing CJK punctuation at line start");
        }
        inline_input.runs = {{"A", {}, 16, 24}, {"  B", {}, 16, 24}};
        auto collapsed = layout_inline_probe(inline_input, 500, fonts);
        require(collapsed.fragments[0].text == "A" && collapsed.fragments[1].text == " B",
                "cross-node whitespace collapse");
        inline_input.runs = {{"a", {}, 16, 24}, {"\u0301", {}, 16, 24}};
        reject([&] {
            layout_inline_probe(inline_input, 500, fonts);
        });
        inline_input.runs = {{"of", {}, 16, 24}, {"fice", {}, 16, 24}};
        reject([&] {
            layout_inline_probe(inline_input, 500, fonts);
        });
        inline_input.runs = {{"AV words / more", {}, 16, 24}};
        auto slash = layout_inline_probe(inline_input, 70, fonts);
        require(slash.fragments[0].text == "AV words", "space/solidus CSS tailoring");
        inline_input.runs = {{"仓 \n 库", {}, 16, 24}};
        require(layout_inline_probe(inline_input, 100, fonts).fragments[0].text == "仓库",
                "CJK segment-break transform");
        inline_input.runs = {{"a\rB", {}, 16, 24, {0, 0, 0, 1}, "pre-line"}};
        require(layout_inline_probe(inline_input, 100, fonts).lines == 1, "DOM CR is a space");
        inline_input.runs = {{"a", {}, 16, 24}, {"", {}, 16, 24, {0, 0, 0, 1}, "normal", true}};
        require(layout_inline_probe(inline_input, 100, fonts).lines == 1, "terminal br extra line");
        inline_input.runs.push_back({"", {}, 16, 24, {0, 0, 0, 1}, "normal", true});
        require(layout_inline_probe(inline_input, 100, fonts).lines == 2, "consecutive br empty line");
        inline_input.runs = {{"a\u2028b", {}, 16, 24}};
        require(layout_inline_probe(inline_input, 100, fonts).lines == 2, "Unicode mandatory break");
        inline_input.runs = {{"a\u00adb", {}, 16, 24}};
        reject([&] {
            layout_inline_probe(inline_input, 100, fonts);
        });
        inline_input.runs = {{"one two", {}, 16, 24, {0, 0, 0, 1}, "nowrap"}};
        auto no_wrap = layout_inline_probe(inline_input, 1, fonts);
        require(no_wrap.lines == 1 && no_wrap.min_content == no_wrap.max_content, "nowrap intrinsic opportunities");
        inline_input.runs = {{"A\u00a0B", {}, 16, 24}};
        require(layout_inline_probe(inline_input, 1, fonts).lines == 1, "NBSP wrapping forbidden");
        inline_input.runs = {{"仓", {"outer", "left"}, 16, 24, {0, 0, 0, 1}, "nowrap", false, {true, false}},
                             {"库", {"outer", "right"}, 16, 24, {0, 0, 0, 1}, "nowrap", false, {true, false}}};
        require(layout_inline_probe(inline_input, 17, fonts).lines == 2, "nonspace boundary uses common ancestor");
        inline_input.runs = {{"A", {}, 16, 24}};
        inline_input.white_space = "pre-wrap";
        reject([&] {
            layout_inline_probe(inline_input, 100, fonts);
        });
        inline_input.white_space = "normal";
        const auto css = read_text_file(CE_SOURCE_DIR "/tests/fixtures/flow-layout/profile.css");
        ScriptDomProbe vm;
        caseelephant::install_script_platform(vm, caseelephant::ScriptPlatformModule::WebHost);
        vm.eval(read_text_file(CE_SOURCE_DIR "/tests/fixtures/flow-layout/fixture.js"));
        vm.install_layout_host([&](const auto& document) {
            return build_flow_page_probe(find(document, "gallery"), css, caseelephant::constants::default_view_width,
                                         caseelephant::constants::default_view_height, fonts)
                .elements;
        });
        const auto viewport = vm.node_identity("#viewport");
        const auto intro = vm.node_identity("#intro");
        auto dom = vm.snapshot_subtree("#gallery");
        auto cascade = compute_style_probe(
            dom,
            "#viewport{--r:30px 20% / 12px 5px;border-radius:var(--r);border-top-left-radius:7px 9px;overflow:hidden auto;overflow-x:scroll}",
            flow_page_style_profile());
        auto vs = cascade.children[2];
        require(vs.values.at("border-top-left-radius") == "7px 9px" &&
                    vs.values.at("border-bottom-left-radius") == "20% 5px" && vs.values.at("overflow-x") == "scroll" &&
                    vs.values.at("overflow-y") == "auto",
                "radius/overflow shorthand cascade");
        auto bad = compute_style_probe(dom, "#viewport{border-radius:8px;border-radius:var(--r);--r:2px -1px}",
                                       flow_page_style_profile());
        for (const auto* k : {"border-top-left-radius", "border-top-right-radius", "border-bottom-right-radius",
                              "border-bottom-left-radius"}) {
            require(bad.children[2].values.at(k) == "0", "invalid radius shorthand recascaded");
        }
        for (const auto* unsupported :
             {"#intro{font-weight:700}", "#intro{white-space:pre-wrap}", "#emphasis{position:relative}",
              "#emphasis{padding:1px}", "#viewport{overflow:clip}", "#viewport{overflow-x:potato}",
              "#viewport{display:grid}"}) {
            reject([&] {
                build_flow_page_probe(dom, css + unsupported, caseelephant::constants::default_view_width,
                                      caseelephant::constants::default_view_height, fonts);
            });
        }
        auto invalid_scroll = dom;
        invalid_scroll.scroll_top = std::nan("");
        reject([&] {
            build_flow_page_probe(invalid_scroll, css, caseelephant::constants::default_view_width,
                                  caseelephant::constants::default_view_height, fonts);
        });
        require(
            vm.eval(
                "document.getElementById('viewport').scrollHeight > document.getElementById('viewport').clientHeight") ==
                "true",
            "fresh scroll extent");
        std::ostringstream out;
        out << "{\"fixture\":\"flow-layout-v1\",\"frames\":[";
        std::vector<ProbeFlowPage> owned;
        for (int frame = 0; frame < caseelephant::test_constants::layout::fixture_frames; ++frame) {
            if (frame) {
                vm.eval("flowLayoutFixture.advance()");
            }
            if (frame) {
                out << ',';
            }
            out << '[';
            owned.push_back(build_flow_page_probe(vm.snapshot_subtree("#gallery"), css,
                                                  caseelephant::constants::default_view_width,
                                                  caseelephant::constants::default_view_height, fonts));
            report(out, owned.back());
            for (int i = 0; i < caseelephant::test_constants::layout::flow_fixture_scenes; ++i) {
                out << ',';
                report(out, build_flow_page_probe(vm.snapshot_subtree("#scene-" + std::to_string(i)), "",
                                                  caseelephant::constants::default_view_width,
                                                  caseelephant::constants::default_view_height, fonts));
            }
            out << ']';
            if (frame == 1) {
                require(vm.eval("document.getElementById('viewport').scrollTop") == "63", "scroll setter");
                require(std::abs(geom(owned.back(), intro).rect[1] - geom(owned[0], intro).rect[1] + 63) <
                            caseelephant::test_constants::layout::geometry_tolerance,
                        "descendant scroll geometry");
            }
            if (frame == 2) {
                require(vm.eval("document.getElementById('viewport').scrollTop") == "0",
                        "content removal clamps synchronously");
            }
        }
        out << "],\"passed\":true}";
        vm.eval(
            "globalThis.scrollEvents=0;document.getElementById('viewport').addEventListener('scroll',()=>scrollEvents++);document.getElementById('viewport').scrollLeft=3;document.getElementById('viewport').scrollLeft=4;");
        require(vm.eval("scrollEvents") == "1", "scroll event coalescing");
        vm.eval("document.getElementById('viewport').addEventListener('wheel',e=>e.preventDefault(),{once:true})");
        require(vm.dispatch_wheel(viewport, 20, 0) == ProbeDispatchResult::DefaultPrevented, "wheel cancel");
        require(vm.eval("document.getElementById('viewport').scrollLeft") == "4", "prevented wheel moved");
        require(vm.dispatch_wheel(viewport, 20, 0) == ProbeDispatchResult::DefaultAllowed, "wheel default");
        require(vm.eval("document.getElementById('viewport').scrollLeft") == "24", "wheel pixel delta");
        vm.eval("globalThis.detached=document.getElementById('intro');detached.parentNode.removeChild(detached);");
        require(vm.eval("detached.getClientRects().length+detached.getBoundingClientRect().width") == "0",
                "detached geometry");
        require(vm.dispatch_wheel(intro, 0, 2) == ProbeDispatchResult::StaleTarget, "detached wheel accepted");
        vm.install_layout_host([&](const auto&) -> std::vector<ProbeElementGeometry> {
            vm.eval("1");
            return {};
        });
        reject([&] {
            vm.eval("document.getElementById('viewport').clientWidth");
        });
        vm.install_layout_host([&](const auto& d) {
            auto values = build_flow_page_probe(find(d, "gallery"), css, caseelephant::constants::default_view_width,
                                                caseelephant::constants::default_view_height, fonts)
                              .elements;
            values[0].identity.view_epoch++;
            return values;
        });
        reject([&] {
            vm.eval("document.getElementById('viewport').clientWidth");
        });
        vm.install_layout_host([&](const auto& d) {
            return build_flow_page_probe(find(d, "gallery"), css, caseelephant::constants::default_view_width,
                                         caseelephant::constants::default_view_height, fonts)
                .elements;
        });
        bool wrong_thread{};
        std::thread t([&] {
            try {
                vm.install_layout_host({});
            } catch (const std::logic_error&) {
                wrong_thread = true;
            }
        });
        t.join();
        require(wrong_thread, "geometry host wrong thread");
        vm.reset_document();
        vm.collect_garbage();
        require(!vm.is_live(viewport) && !owned[1].page.paints.empty(), "owning paint survives VM reset");
        require(vm.dispatch_wheel(viewport, 2, 0) == ProbeDispatchResult::StaleTarget, "old epoch wheel");
        for (int cycle = 0; cycle < caseelephant::test_constants::layout::scroll_lifecycle_cycles; ++cycle) {
            vm.eval(R"JS(
      const root=document.createElement('div');root.id='life';root.setAttribute('style','position:relative;width:80px;height:28px;overflow:auto;font-family:ProbeRoboto, ProbeCjk;font-size:16px;line-height:24px');
      const child=document.createElement('div');child.id='life-child';child.textContent='仓库运输仓库运输仓库运输';root.appendChild(child);document.body.appendChild(root);
    )JS");
            vm.install_layout_host([&](const auto& d) {
                return build_flow_page_probe(find(d, "life"), "", 100, 100, fonts).elements;
            });
            auto id = vm.node_identity("#life-child");
            vm.eval(
                "document.getElementById('life').scrollTop=9999;document.getElementById('life').scrollTop=Object.create(null,{valueOf:{value(){return 7}}});");
            require(vm.eval("document.getElementById('life').scrollTop") == "7", "WebIDL scroll conversion");
            require(vm.dispatch_wheel(id, 0, 8) == ProbeDispatchResult::DefaultAllowed,
                    "descendant wheel scrolls ancestor");
            vm.eval(
                "globalThis.held=document.getElementById('life-child');held.parentNode.removeChild(held);held.scrollTop=50;");
            require(vm.is_live(id) && vm.eval("held.scrollTop") == "0", "detached retained scroll geometry");
            vm.reset_document();
            vm.collect_garbage();
            require(!vm.is_live(id) && vm.stats().live_nodes == 3 && vm.stats().native_roots == 1,
                    "scroll geometry lifecycle leak");
        }
        ProbeRoundedClip radius{{0, 0, 40, 20}, {30, 30, 30, 30}, {30, 30, 30, 30}};
        radius = normalize_rounded_probe(radius);
        require(radius.radius_x[0] == 10 && !contains_rounded_probe(radius, 0, 0) &&
                    contains_rounded_probe(radius, 20, 10),
                "corner overlap normalization");
        auto coverage = rounded_coverage_probe({radius}, {0, 0, 40, 20}, 40, 20);
        require(coverage.front() == 0 && coverage[10 * 40 + 20] == 255, "corner mask");
        require(argc == 2, "report path");
        std::filesystem::path path(argv[1]);
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << out.str();
        std::cout
            << "PASS inline/Unicode/scroll/rounded: 27 mutation scenes, fresh geometry, canceled wheel, detached/epoch/reentry contracts\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
