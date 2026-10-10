#include "layout/document.hpp"
#include "script/script_platform.hpp"
#include "support/check.hpp"
#include "support/files.hpp"
#include "input/mouse_router.hpp"
#include "test_constants/layout.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
using namespace weft;

namespace {
    using weft::testing::read_source_text_file;
    using weft::testing::require;

    const DomSnapshot& find(const DomSnapshot& n, const std::string& id) {
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

    std::string hit_id(const Page& p, double x, double y) {
        auto hit = hit_page(p, x, y);
        return hit ? p.boxes[*hit].id : "";
    }
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "output path");
        FontSet fonts({WEFT_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf",
                       WEFT_SOURCE_DIR "/third_party/noto-cjk/source/NotoSansCJKsc-Regular.otf"});
        const auto css = read_source_text_file("/tests/fixtures/flow-input/profile.css");
        ScriptDom vm;
        weft::install_script_platform(vm, weft::ScriptPlatformModule::WebHost);
        vm.eval(read_source_text_file("/tests/fixtures/flow-input/fixture.js"));
        vm.install_layout_host([&](const auto& d) {
            return layout_document(find(d, "inputpage"), css, weft::constants::default_view_width,
                                   weft::constants::default_view_height, fonts)
                .elements;
        });
        auto layout = [&] {
            return layout_document(vm.snapshot_subtree("#inputpage"), css, weft::constants::default_view_width,
                                   weft::constants::default_view_height, fonts)
                .page.geometry;
        };
        MouseRouter router(vm, layout);
        const auto initial = layout();
        require(hit_id(initial, 340, 40) == "high", "paint stacking differs from reverse DOM order");
        require(hit_id(initial, 320, 315) == "inputpage" && hit_id(initial, 370, 350) == "round",
                "elliptical border hit");
        require(hit_id(initial, 20, 20) == "inputpage" && hit_id(initial, 30, 150) == "row-4",
                "rounded overflow/pointer inheritance");
        require(hit_id(initial, 639, 410) == "edge" && hit_id(initial, 641, 410).empty(),
                "surface clip permits visible overflow only inside viewport");
        require(hit_id(initial, 1, 1).empty(), "root rounded hit clipping");
        std::ostringstream report;
        report << "{\"fixture\":\"flow-input-v1\",\"frames\":[";
        std::size_t points{};
        for (int frame = 0; frame < weft::test_constants::layout::fixture_frames; ++frame) {
            if (frame) {
                vm.eval("inputScenario.advance()");
            }
            const auto page = layout();
            if (frame) {
                report << ',';
            }
            report << '[';
            bool first = true;
            auto sample = [&](double x, double y) {
                if (!first) {
                    report << ',';
                }
                first = false;
                report << "{\"x\":" << x << ",\"y\":" << y << ",\"target\":\"" << hit_id(page, x, y) << "\"}";
                ++points;
            };
            for (int y = weft::test_constants::layout::hit_grid_first; y < weft::constants::default_view_height;
                 y += weft::test_constants::layout::hit_grid_step) {
                for (int x = weft::test_constants::layout::hit_grid_first; x < weft::constants::default_view_width;
                     x += weft::test_constants::layout::hit_grid_step) {
                    sample(x, y);
                }
            }
            for (const auto& p : weft::test_constants::layout::flow_input_fragment_points[std::size_t(frame)]) {
                sample(p[0], p[1]);
            }
            for (auto p : std::vector<std::array<double, 2>>{
                     {1, 1}, {20, 20}, {320, 315}, {370, 350}, {641, 410}, {-1, 40}, {260, 199}}) {
                sample(p[0], p[1]);
            }
            report << ']';
        }
        report << "],\"passed\":true}";
        // Restore an independent DOM so the report's input sequence stays immutable.
        vm.reset_document();
        weft::install_script_platform(vm, weft::ScriptPlatformModule::WebHost);
        vm.eval(read_source_text_file("/tests/fixtures/flow-input/fixture.js"));
        vm.install_layout_host([&](const auto& d) {
            return layout_document(find(d, "inputpage"), css, weft::constants::default_view_width,
                                   weft::constants::default_view_height, fonts)
                .elements;
        });
        // Independent arithmetic oracle for random fresh-scroll and z-order mutations.
        // Fixed heights/padding make expected row identity independent of the layout code.
        unsigned seed = weft::test_constants::layout::flow_input_seed;
        for (unsigned i = 0; i < weft::test_constants::layout::flow_input_mutations; ++i) {
            seed = seed * weft::constants::lcg_multiplier + weft::constants::lcg_increment;
            const double offset = (seed % 140) + .25;
            const int high_z = int((seed >> 12) % 7);
            const int low_z = int((seed >> 20) % 7);
            vm.eval("document.getElementById('scroller').scrollTop=" + std::to_string(offset) +
                    ";document.getElementById('high').style.zIndex='" + std::to_string(high_z) +
                    "';document.getElementById('low').style.zIndex='" + std::to_string(low_z) + "'");
            const auto current = layout();
            require(hit_id(current, 50, 43) == "row-" + std::to_string(int((13 + offset) / 26)),
                    "random fresh scroll identity");
            require(hit_id(current, 340, 40) == (high_z > low_z ? "high" : "low"),
                    "random paint-order identity, including ties");
        }
        vm.eval("document.getElementById('scroller').scrollTop=0");
        MouseEvent mouse;
        mouse.client_x = 50;
        mouse.client_y = 43;
        router.route(MouseAction::Down, mouse);
        vm.collect_garbage();
        router.route(MouseAction::Up, mouse);
        require(vm.eval("inputScenario.clicks.join(',')") == "row-0", "fresh flow mouse routed to row");
        WheelEvent wheel{10.25, 12.75, 50, 43, -20, 900, 123, true, true, true, true};
        require(router.wheel(wheel) == DispatchResult::DefaultAllowed, "flow wheel delivery");
        require(vm.eval("JSON.stringify(inputScenario.wheels[0])") ==
                    "[\"row-0\",10.25,12.75,0,50,43,-20,900,true,true,true,true,123,true,true]",
                "WheelEvent payload/prototype/modifiers/time");
        require(vm.eval("document.getElementById('scroller').scrollTop") == "12.75", "fractional scroll consumed");
        vm.eval("inputScenario.blockWheel=true");
        require(router.wheel(wheel) == DispatchResult::DefaultPrevented, "wheel canceled before default");
        require(vm.eval("document.getElementById('scroller').scrollTop") == "12.75", "canceled wheel scrolls");
        vm.eval("inputScenario.blockWheel=false");
        wheel.client_x = 20;
        wheel.client_y = 20;
        router.wheel(wheel);
        require(vm.eval("document.getElementById('scroller').scrollTop") == "12.75",
                "clipped corner wheel reaches scroller");
        mouse.client_x = 350;
        mouse.client_y = 260;
        router.route(MouseAction::Down, mouse);
        router.route(MouseAction::Up, mouse);
        require(vm.eval("inputScenario.clicks.length") == "1", "disabled nested button activated");
        // A mutation before the next input must move targets without a render.
        vm.eval("document.getElementById('scroller').scrollTop=52;document.getElementById('scroller').scrollLeft=0");
        mouse.client_x = 50;
        mouse.client_y = 43;
        router.route(MouseAction::Down, mouse);
        router.route(MouseAction::Up, mouse);
        require(vm.eval("inputScenario.clicks[1]") == "row-2", "next hit ignores latest scroll");
        router.route(MouseAction::Down, mouse);
        router.cancel();
        router.route(MouseAction::Up, mouse);
        require(vm.eval("inputScenario.clicks.length") == "2", "cancel fabricated click");
        const auto old = vm.node_identity("#row-2");
        vm.eval(
            "document.getElementById('row-2').addEventListener('wheel',()=>{const e=document.getElementById('scroller');globalThis.held=e;e.parentNode.removeChild(e);}, {once:true})");
        wheel.client_x = 50;
        wheel.client_y = 43;
        router.wheel(wheel);
        require(vm.is_live(old) && vm.eval("held.scrollTop") == "0",
                "wheel mutation invalidated default identity or detached node");
        require(vm.dispatch_wheel(old, wheel) == DispatchResult::StaleTarget, "detached wheel dispatched");
        bool wrong{};
        bool wrong_mouse{};
        std::thread t([&] {
            try {
                router.wheel(wheel);
            } catch (const std::logic_error&) {
                wrong = true;
            }
            try {
                router.route(MouseAction::Move, mouse);
            } catch (const std::logic_error&) {
                wrong_mouse = true;
            }
        });
        t.join();
        require(wrong && wrong_mouse, "wrong-owner input routing");
        vm.reset_document();
        vm.collect_garbage();
        require(!vm.is_live(old) && vm.stats().live_nodes == 3 && vm.stats().native_roots == 1,
                "input roots leaked after reset");
        std::filesystem::path p(argv[1]);
        std::filesystem::create_directories(p.parent_path());
        std::ofstream(p) << report.str();
        std::cout
            << "PASS flow input: " << points
            << " hit queries, 128 random mutation checks, fresh clipped/rounded/stacked fragments, mouse and fractional/canceled wheel, detached/epoch/owner\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
