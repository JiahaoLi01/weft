#include "caseelephant/input_probe.hpp"
#include "test_constants/layout.hpp"
#include "../probes/window_frame_rate.hpp"
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {
    void require(bool b, const char* text) {
        if (!b) {
            throw std::runtime_error(text);
        }
    }

    std::string read(const char* path) {
        std::ifstream f(std::string(CE_SOURCE_DIR) + path, std::ios::binary);
        require(bool(f), "missing fixture");
        std::ostringstream s;
        s << f.rdbuf();
        return s.str();
    }

    void pump(caseelephant::ScriptDomProbe& s, unsigned time = 0) {
        for (unsigned i = 0; i < caseelephant::test_constants::layout::max_pump_rounds; ++i) {
            s.eval("__ceHost.pump(" + std::to_string(time) + ")");
            if (s.eval("__ceHost.pending()") == "0") {
                return;
            }
        }
        throw std::runtime_error("tasks not idle");
    }
} // namespace

int main() {
    using namespace caseelephant;
    try {
        auto viewport = ProbeViewport::fit(1000, 420);
        require(viewport.x == 180 && viewport.scale == 1 &&
                    viewport.logical(280, 220) == std::array<double, 2>{100, 220},
                "letterbox mapping");
        require(viewport.logical(100, 220)[0] < 0, "letterbox must stay outside page");
        require(ProbeViewport::fit(0, 0).logical(0, 0) == std::array<double, 2>{-1, -1}, "minimized mapping");
        // 640x420 content in a 16:9 4K surface has an INNER letterbox, even
        // when its 1000x420 window has a different OUTER letterbox.
        const auto surface = ProbeViewport::through_surface(1000, 420, caseelephant::constants::uhd_width,
                                                            caseelephant::constants::uhd_height);
        require(std::abs(surface.x - 180) < caseelephant::test_constants::layout::geometry_tolerance &&
                    std::abs(surface.scale - 1) < caseelephant::test_constants::layout::scale_tolerance &&
                    std::abs(surface.logical(340, 175)[0] - 160) <
                        caseelephant::test_constants::layout::geometry_tolerance,
                "composed raster/window mapping");
        require(surface.logical(160, 175)[0] < 0 &&
                    surface.logical(850, 175)[0] > caseelephant::constants::default_view_width,
                "both surface/window margins stay outside page");
        const auto native4k =
            ProbeViewport::through_surface(caseelephant::constants::uhd_width, caseelephant::constants::uhd_height,
                                           caseelephant::constants::uhd_width, caseelephant::constants::uhd_height);
        require(std::abs(native4k.logical(1920, 1080)[0] - 320) <
                        caseelephant::test_constants::layout::geometry_tolerance &&
                    std::abs(native4k.logical(1920, 1080)[1] - 210) <
                        caseelephant::test_constants::layout::geometry_tolerance &&
                    native4k.logical(8, 1000)[0] < 0,
                "native 4K inverse mapping");
        require(ProbeViewport::through_surface(0, 0, caseelephant::constants::uhd_width,
                                               caseelephant::constants::uhd_height)
                        .logical(0, 0) == std::array<double, 2>{-1, -1},
                "composed minimized mapping");
        bool invalid_surface{};
        try {
            (void)ProbeViewport::through_surface(1000, 420, 0, caseelephant::constants::uhd_height);
        } catch (const std::invalid_argument&) {
            invalid_surface = true;
        }
        require(invalid_surface, "zero raster surface accepted");
        WindowFrameRate rate;
        rate.reset(10);
        for (int i = 1; i <= 60; ++i) {
            rate.presented(10 + i / 60.);
        }
        require(rate.ready && std::abs(rate.fps - 60) < caseelephant::test_constants::layout::fps_tolerance &&
                    std::abs(rate.frame_ms - 1000. / 60) < caseelephant::test_constants::layout::fps_tolerance,
                "FPS uses actual presentation intervals");
        // A half-second stall lowers the next rate; no nominal 60Hz constant.
        for (int i = 1; i <= 30; ++i) {
            rate.presented(11.5 + i / 60.);
        }
        require(std::abs(rate.fps - 30) < caseelephant::test_constants::layout::fps_tolerance && rate.samples == 2,
                "slow frame missing from FPS denominator");
        rate.reset(100);
        require(!rate.ready && rate.fps == 0, "pause retains stale FPS");
        for (int i = 1; i <= 20; ++i) {
            rate.presented(100 + i / 20.);
        }
        require(std::abs(rate.fps - 20) < caseelephant::test_constants::layout::fps_tolerance,
                "paused interval counted in resumed FPS");
        bool reversed{};
        try {
            rate.presented(99);
        } catch (const std::invalid_argument&) {
            reversed = true;
        }
        require(reversed, "reversed FPS clock accepted");
        ProbePage p;
        p.hits = {{{},
                   SIZE_MAX,
                   0,
                   0,
                   caseelephant::constants::default_view_width,
                   caseelephant::constants::default_view_height,
                   true,
                   false},
                  {{}, 0, 40, 40, 100, 100, true, false},
                  {{}, 0, 60, 60, 100, 100, true, false}};
        require(hit_page_probe(p, 70, 70) == 2 && hit_page_probe(p, 40, 40) == 1 && hit_page_probe(p, 160, 70) == 0,
                "paint order and half-open box edges");
        p.hits[2].enabled = false;
        require(hit_page_probe(p, 70, 70) == 1 && !hit_page_probe(p, caseelephant::constants::default_view_width, 0) &&
                    !hit_page_probe(p, std::numeric_limits<double>::quiet_NaN(), 0),
                "hidden/invalid/viewport hit");
        TextProbeFont font(CE_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf");
        ScriptProbeConfig config{caseelephant::test_constants::layout::script_stack_bytes};
#ifdef CE_REACT_GC_STRESS
        config.execution_budget_ms = caseelephant::test_constants::layout::gc_stress_execution_budget_ms;
#endif
        ScriptDomProbe s(config);
        const auto bootstrap = read("/probes/web_host_bootstrap.js");
        s.eval(bootstrap);
        s.eval(read("/examples/react-vite/native/app.js"));
        pump(s);
        ProbeMouseRouter router(s, font);
        s.eval(
            R"JS(globalThis.events=[];for(const name of ['mousedown','mouseup','click']) document.body.addEventListener(name,e=>events.push([e.type,e.target.id,e.clientX,e.clientY,e.buttons,e.isTrusted,e instanceof MouseEvent,e.ctrlKey,e.timeStamp]));)JS");
        ProbeMouseEvent event;
        event.client_x = 100;
        event.client_y = 220;
        event.ctrl = true;
        event.time_ms = 11;
        router.route(ProbeMouseAction::Down, event);
        s.collect_garbage();
        router.route(ProbeMouseAction::Up, event);
        pump(s, 16);
        require(s.eval("caseElephantReact.snapshot().count") == "Count: 1", "layout-hit React useState click");
        require(
            s.eval("JSON.stringify(events)") ==
                R"JS([["mousedown","increment",100,220,1,true,true,true,11],["mouseup","increment",100,220,0,true,true,true,11],["click","increment",100,220,0,true,true,true,11]])JS",
            "real native MouseEvent payload and propagation");
        router.route(ProbeMouseAction::Down, event);
        router.cancel();
        router.route(ProbeMouseAction::Up, event);
        pump(s, 16);
        require(s.eval("caseElephantReact.snapshot().count") == "Count: 1", "lost capture fabricated click");
        router.route(ProbeMouseAction::Down, event);
        event.client_y = 340;
        router.route(ProbeMouseAction::Up, event);
        pump(s, 16);
        require(s.eval("events[events.length-1][1]") == "page" &&
                    s.eval("caseElephantReact.snapshot().order.join(',')") == "ember,tide,moss",
                "different targets must click common ancestor");
        event.client_y = 220;
        s.eval("document.getElementById('increment').setAttribute('disabled','')");
        router.route(ProbeMouseAction::Down, event);
        router.route(ProbeMouseAction::Up, event);
        pump(s, 16);
        require(s.eval("caseElephantReact.snapshot().count") == "Count: 1", "disabled native button activated React");
        s.eval(
            "document.getElementById('increment').removeAttribute('disabled');document.getElementById('increment').style.display='none'");
        router.route(ProbeMouseAction::Down, event);
        router.route(ProbeMouseAction::Up, event);
        pump(s, 16);
        require(s.eval("caseElephantReact.snapshot().count") == "Count: 1", "display:none hit");
        s.reset_document();
        s.eval(bootstrap);
        s.eval(R"JS(
          let page=document.createElement('div');page.id='page';page.setAttribute('style','display:flex;width:400px;height:200px;');document.getElementById('root').appendChild(page);
          let b=document.createElement('button');b.id='button';b.setAttribute('style','display:flex;width:160px;height:40px;border-width:0;');page.appendChild(b);
          for(let i=0;i<2;++i){let child=document.createElement('div');child.id='child'+i;child.setAttribute('style','display:flex;width:60px;height:40px;');b.appendChild(child);}
          globalThis.clicked=[];globalThis.order=[];
          b.addEventListener('click',e=>clicked.push(e.target.id));
          page.addEventListener('click',e=>order.push('bubble'));
          page.addEventListener('click',e=>order.push('capture'),true);
        )JS");
        ProbeMouseRouter nested(s, font);
        event.client_x = 20;
        event.client_y = 20;
        nested.route(ProbeMouseAction::Down, event);
        event.client_x = 80;
        nested.route(ProbeMouseAction::Up, event);
        require(s.eval("clicked.join(',')") == "button" && s.eval("order.join(',')") == "capture,bubble",
                "nested child down/up button ancestor and phase order");
        s.eval(
            "document.getElementById('button').style.pointerEvents='none';document.getElementById('child1').style.pointerEvents='auto'");
        auto page = build_page_probe(s.snapshot_subtree("#page"), font);
        require(page.hits[*hit_page_probe(page, 20, 20)].identity == s.node_identity("#page") &&
                    page.hits[*hit_page_probe(page, 80, 20)].identity == s.node_identity("#child1"),
                "pointer-events inheritance and child override");
        s.eval(
            "document.getElementById('button').style.pointerEvents='auto';document.getElementById('button').addEventListener('mousedown',()=>{globalThis.detached=document.getElementById('button');detached.parentNode.removeChild(detached);})");
        const auto old = s.node_identity("#button");
        event.client_x = 20;
        nested.route(ProbeMouseAction::Down, event);
        nested.route(ProbeMouseAction::Up, event);
        require(s.eval("clicked.length") == "1" && s.dispatch_mouse(old, event) == ProbeDispatchResult::StaleTarget,
                "detached target received click");
        s.reset_document();
        require(s.dispatch_mouse(old, event) == ProbeDispatchResult::StaleTarget, "old View epoch dispatched");
        require(s.stats().listener_exceptions == 0, "host listener exception");
        std::cout
            << "PASS viewport/order/edges, actual React mouse click, modifiers/GC, cancel/disabled/hidden, common ancestor, pointer-events and detached/epoch input\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
