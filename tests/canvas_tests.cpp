#define NOMINMAX
#include "script/script_dom.hpp"
#include "support/check.hpp"
#include "layout/positioned_page.hpp"
#include "constants/canvas.hpp"
#include "constants/common.hpp"
#include "test_constants/style_text.hpp"
#include <windows.h>
#include <crtdbg.h>
#include <cmath>
#include <iostream>
#include <thread>
#include <limits>

namespace {
    using weft::testing::require;

    void pixel(const weft::CanvasSnapshot& s, unsigned x, unsigned y, std::array<int, 4> expected,
               int tolerance = weft::test_constants::style_text::canvas_pixel_tolerance) {
        for (unsigned c = 0; c < 4; ++c) {
            require(
                std::abs(int((*s.rgba)[(std::size_t(y) * s.width + x) * weft::constants::rgba8_bytes_per_pixel + c]) -
                         expected[c]) <= tolerance,
                "actual canvas pixel mismatch");
        }
    }
} // namespace

int main() {
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    try {
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM init");
        {
            weft::ScriptDom vm;
            vm.install_canvas_host(weft::make_d2d_canvas_factory());
            vm.eval("globalThis.c=document.createElement('canvas');");
            vm.eval("c.id='canvas';document.body.appendChild(c)");
            vm.eval("globalThis.ctx=c.getContext('2d')");
            const auto identity = vm.node_identity("#canvas");
            require(
                vm.eval(
                    "(()=>{try{ctx.fillRect.call(c,0,0,1,1);return false}catch(e){return e instanceof TypeError}})()") ==
                    "true",
                "context receiver brand error");
            require(
                vm.eval(
                    "(()=>{try{c.getContext.call(document.body,'2d');return false}catch(e){return e instanceof TypeError}})()") ==
                    "true",
                "Canvas receiver brand error");
            require(
                vm.eval(
                    "c.width===300 && c.height===150 && ctx.canvas===c && c.getContext('2d')===ctx && c.getContext('webgl2')===null && c.getContext('other')===null") ==
                    "true",
                "default dimensions/context identity/modes");
            require(
                vm.eval(
                    "(()=>{try{document.createElement('canvas').getContext('2d',3);return false}catch(e){return e instanceof TypeError}})()") ==
                    "true",
                "primitive options dictionary");
            vm.eval("c.width=8;c.height=8;ctx.fillStyle='rgba(255,0,0,0.5)';ctx.fillRect(0,0,8,8)");
            const auto first = vm.canvas_snapshot(identity);
            pixel(first, 2, 2, {255, 0, 0, 128});
            require(vm.canvas_snapshot(identity).rgba == first.rgba, "unchanged immutable snapshot not reused");
            vm.eval("ctx.fillStyle='rgba(0,0,255,0.5)';ctx.fillRect(0,0,8,8)");
            const auto second = vm.canvas_snapshot(identity);
            pixel(second, 2, 2, {85, 0, 170, 192});
            pixel(first, 2, 2, {255, 0, 0, 128});
            require(first.version < second.version && first.rgba != second.rgba,
                    "bitmap version mutation reused old pixels");
            vm.eval("ctx.globalAlpha=0;ctx.clearRect(1,1,2,2)");
            pixel(vm.canvas_snapshot(identity), 1, 1, {0, 0, 0, 0});
            require(vm.eval("ctx.globalAlpha=0.4;ctx.globalAlpha=NaN;ctx.globalAlpha=2;ctx.globalAlpha===0.4") ==
                        "true",
                    "invalid alpha must preserve previous value");
            require(vm.eval("ctx.fillStyle='#fff';ctx.fillStyle='invalidColor';ctx.fillStyle==='#ffffff'") == "true",
                    "invalid color/canonical getter");
            require(
                vm.eval(
                    "(()=>{try{ctx.fillStyle='color(display-p3 1 0 0)';return false}catch(e){return String(e).includes('WEFT_CANVAS_COLOR_PROFILE')}})()") ==
                    "true",
                "unsupported color silently accepted");
            vm.eval("ctx.globalAlpha=1;ctx.beginPath();ctx.arc(4,4,2,0,Math.PI*2);ctx.fill()");
            pixel(vm.canvas_snapshot(identity), 4, 4, {255, 255, 255, 255});
            require(
                vm.eval(
                    "(()=>{try{ctx.arc(1,1,-1,0,Math.PI*2);return false}catch(e){return e.name==='IndexSizeError'}})()") ==
                    "true",
                "negative radius error");
            require(
                vm.eval(
                    "(()=>{ctx.beginPath();try{ctx.arc(1,1,1,0,-Math.PI*2);return false}catch(e){return String(e).includes('WEFT_CANVAS_PATH_PROFILE')}})()") ==
                    "true",
                "unsupported arc direction silently changed");
            vm.eval("ctx.fillRect(NaN,0,5,5)");
            vm.eval("c.width=c.width");
            require(
                vm.eval("ctx.fillStyle==='#000000' && ctx.globalAlpha===1 && c.getContext('2d',{alpha:false})===ctx") ==
                    "true",
                "same-value resize reset/identity");
            pixel(vm.canvas_snapshot(identity), 4, 4, {0, 0, 0, 0});
            vm.eval("ctx.fillStyle='red';ctx.fillRect(4,4,-2,-2)");
            pixel(vm.canvas_snapshot(identity), 2, 2, {255, 0, 0, 255});
            vm.eval("c.setAttribute('width','4px')");
            require(vm.eval("c.width===4") == "true", "content dimension parsing");
            vm.eval("c.setAttribute('width','invalid')");
            require(vm.eval("c.width===300") == "true", "invalid content dimension default");
            vm.eval("c.removeAttribute('width');c.width=0");
            require(vm.canvas_snapshot(identity).rgba->empty(), "zero bitmap dimensions");
            vm.eval("c.width=8;ctx.fillStyle='rebeccapurple';ctx.fillRect(0,0,8,8)");
            pixel(vm.canvas_snapshot(identity), 1, 1, {102, 51, 153, 255});
            require(
                vm.eval(
                    "(()=>{try{c.width=99999;return false}catch(e){return c.width===8 && ctx.fillStyle==='#663399'}})()") ==
                    "true",
                "size rejection partially reset bitmap/state");
            vm.eval(
                "globalThis.order=[];c.addEventListener('click',()=>order.push('A'));c.onclick=()=>order.push('old');c.addEventListener('click',()=>order.push('B'));c.onclick=function(){order.push(this===c?'H':'wrong');return false};");
            require(vm.eval("c.dispatchEvent(new Event('click',{bubbles:true,cancelable:true}))") == "false",
                    "property handler false cancellation");
            require(vm.eval("order.join(',')") == "A,H,B", "replacement changed event handler registration order");
            vm.eval("c.onclick=null;order=[];c.dispatchEvent(new Event('click')); ");
            require(vm.eval("order.join(',')") == "A,B", "null handler did not deactivate");
            auto retained = vm.canvas_snapshot(identity);
            vm.eval("document.body.removeChild(c);c=null");
            vm.collect_garbage();
            require(vm.stats().canvas_contexts == 1, "context did not retain detached canvas");
            vm.eval("ctx.fillStyle='hsl(120,100%,50%)';ctx.fillRect(0,0,8,8)");
            pixel(vm.canvas_snapshot(identity), 1, 1, {0, 255, 0, 255});
            vm.eval("ctx=null");
            vm.collect_garbage();
            require(vm.stats().canvas_contexts == 0 && vm.stats().canvas_backing_bytes == 0,
                    "native canvas/context cycle leaked");
            pixel(retained, 1, 1, {102, 51, 153, 255});
            bool stale = false;
            try {
                vm.canvas_snapshot(identity);
            } catch (...) {
                stale = true;
            }
            require(stale, "dead canvas snapshot accepted");
            vm.reset_document();
            vm.install_canvas_host(weft::make_d2d_canvas_factory());
            vm.eval(
                "globalThis.o=document.createElement('canvas');o.id='opaque';document.body.appendChild(o);o.width=4;o.height=4;globalThis.oc=o.getContext('2d',{alpha:false});oc.fillStyle='rgba(255,0,0,0.5)';oc.fillRect(0,0,4,4)");
            auto opaque = vm.canvas_snapshot(vm.node_identity("#opaque"));
            pixel(opaque, 1, 1, {128, 0, 0, 255});
            vm.eval("oc.clearRect(0,0,4,4)");
            pixel(vm.canvas_snapshot(opaque.identity), 1, 1, {0, 0, 0, 255});
            bool wrong = false;
            std::thread worker([&] {
                try {
                    vm.canvas_snapshot(opaque.identity);
                } catch (const std::logic_error&) {
                    wrong = true;
                }
            });
            worker.join();
            require(wrong, "wrong owner canvas snapshot");
            vm.reset_document();
            require(vm.stats().canvas_contexts == 0 && vm.stats().canvas_backing_bytes == 0,
                    "epoch reset retained raster");
            pixel(opaque, 1, 1, {128, 0, 0, 255});
            vm.install_canvas_host(weft::make_d2d_canvas_factory());
            vm.eval(
                "globalThis.many=Array.from({length:8},()=>{const c=document.createElement('canvas');c.width=0;return c.getContext('2d')});");
            require(
                vm.eval(
                    "(()=>{try{document.createElement('canvas').getContext('2d');return false}catch(e){return String(e).includes('CONTEXT_LIMIT')}})()") ==
                    "true",
                "live context quota");
            vm.eval("many=null");
            vm.collect_garbage();
            require(vm.stats().canvas_contexts == 0, "quota rejection leaked native context");
            vm.eval(
                "globalThis.a=document.createElement('canvas');a.width=2048;a.height=2048;globalThis.ac=a.getContext('2d');globalThis.b=document.createElement('canvas');b.width=2048;b.height=2048;globalThis.bc=b.getContext('2d');");
            require(vm.stats().canvas_backing_bytes ==
                        static_cast<unsigned>(weft::constants::canvas::max_backing_bytes),
                    "aggregate backing accounting");
            require(
                vm.eval(
                    "(()=>{try{a.width=2048;return false}catch(e){return a.width===2048 && String(e).includes('BYTES_LIMIT')}})()") ==
                    "true",
                "resize overlap exceeded bounded backing quota");
            require(vm.stats().peak_canvas_backing_bytes <=
                        static_cast<unsigned>(weft::constants::canvas::max_backing_bytes),
                    "resize overlap peak exceeded limit");
            vm.eval("a=b=ac=bc=null");
            vm.collect_garbage();
            require(vm.stats().canvas_backing_bytes == 0, "large detached cycles leaked");
            vm.reset_document();
            vm.install_canvas_host(weft::make_d2d_canvas_factory());
            vm.eval(
                "globalThis.p=document.createElement('div');p.id='p';document.body.appendChild(p);globalThis.c=document.createElement('canvas');c.id='intrinsic';p.appendChild(c);globalThis.ctx=c.getContext('2d');");
            weft::Font font(WEFT_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf");
            const auto css = "#p{width:800px;height:600px}canvas{display:block}";
            auto page = weft::build_positioned_page(vm.snapshot_subtree("#p"), css, 800, 600, font);
            require(page.elements[1].rect[2] == 300 && page.elements[1].rect[3] == 150,
                    "default intrinsic Canvas geometry");
            vm.eval("c.width=800;c.height=600");
            // This VM has no CSSOM bootstrap; use the real content attribute.
            vm.eval("c.setAttribute('style','width:400px')");
            page = weft::build_positioned_page(vm.snapshot_subtree("#p"), css, 800, 600, font);
            require(page.elements[1].rect[2] == 400 && page.elements[1].rect[3] == 300 &&
                        vm.eval("c.width===800 && c.height===600") == "true",
                    "CSS size changed backing or intrinsic ratio");
            vm.eval("c.setAttribute('style','position:absolute;left:10px;top:20px')");
            page = weft::build_positioned_page(vm.snapshot_subtree("#p"), css, 800, 600, font);
            require(page.elements[1].rect == std::array<double, 4>{10, 20, 800, 600},
                    "absolute auto replaced Canvas size");
            vm.eval("c.setAttribute('style','display:none')");
            page = weft::build_positioned_page(vm.snapshot_subtree("#p"), css, 800, 600, font);
            require(page.elements[1].client_rects.empty() && page.elements[1].rect[2] == 0, "hidden Canvas geometry");
            // Command batching must not change persistent pixels. Each side uses a
            // real raster; one snapshots each mutation, the other every 16 changes.
            auto budget = std::make_shared<weft::CanvasBudget>();
            auto factory = weft::make_d2d_canvas_factory();
            weft::CanvasState incremental(factory, budget, weft::test_constants::style_text::canvas_batching_size,
                                          weft::test_constants::style_text::canvas_batching_size, true);
            weft::CanvasState batched(factory, budget, weft::test_constants::style_text::canvas_batching_size,
                                      weft::test_constants::style_text::canvas_batching_size, true);
            std::uint32_t seed = weft::test_constants::style_text::canvas_batching_seed;
            for (unsigned i = 0; i < weft::test_constants::style_text::canvas_batching_iterations; ++i) {
                seed = seed * weft::constants::lcg_multiplier + weft::constants::lcg_increment;
                const unsigned x = seed % weft::test_constants::style_text::canvas_batching_origin_range;
                const unsigned y = (seed >> 8) % weft::test_constants::style_text::canvas_batching_origin_range;
                for (auto* state : {&incremental, &batched}) {
                    state->color = {float(seed & 255) / 255, float((seed >> 8) & 255) / 255,
                                    float((seed >> 16) & 255) / 255, .5f};
                    if (i % weft::test_constants::style_text::canvas_batching_resize_period == 0) {
                        state->resize(weft::test_constants::style_text::canvas_batching_size,
                                      weft::test_constants::style_text::canvas_batching_size);
                    } else if (i % weft::test_constants::style_text::canvas_batching_clear_period == 0) {
                        state->fill_rect(x, y, weft::test_constants::style_text::canvas_batching_rect_size,
                                         weft::test_constants::style_text::canvas_batching_rect_size, true);
                    } else {
                        state->fill_rect(x, y, weft::test_constants::style_text::canvas_batching_rect_size,
                                         weft::test_constants::style_text::canvas_batching_rect_size);
                    }
                }
                auto snapshot = incremental.pixels();
                if (i % weft::test_constants::style_text::canvas_batching_compare_period ==
                        weft::test_constants::style_text::canvas_batching_compare_period - 1 ||
                    i % weft::test_constants::style_text::canvas_batching_resize_period == 0) {
                    require(*snapshot == *batched.pixels(), "snapshot batching changed full persistent bitmap");
                }
            }
            for (unsigned cycle = 0; cycle < weft::test_constants::style_text::canvas_lifecycle_cycles; ++cycle) {
                vm.eval(
                    "(()=>{for(let n=0;n<4;n++){const c=document.createElement('canvas');c.width=8;c.height=8;const ctx=c.getContext('2d');ctx.canvas.onclick=()=>ctx.fillRect(0,0,1,1);ctx.fillRect(0,0,8,8)}})()");
                vm.collect_garbage();
                require(vm.stats().canvas_contexts == 1, "Canvas callback/context/detached lifecycle stress leaked");
            }
            incremental.resize(weft::test_constants::style_text::canvas_batching_size,
                               weft::test_constants::style_text::canvas_batching_size);
            for (unsigned n = 0; n < weft::constants::max_canvas_pending_commands; ++n) {
                incremental.fill_rect(0, 0, 1, 1);
            }
            bool quota = false;
            try {
                incremental.fill_rect(0, 0, 1, 1);
            } catch (const std::invalid_argument&) {
                quota = true;
            }
            require(quota, "command queue quota");
            incremental.resize(0, 0);
            require(incremental.pixels()->empty(), "resize did not discard bounded pending commands");
            const auto before_version = batched.version;
            batched.version = std::numeric_limits<std::uint64_t>::max();
            bool wrapped = false;
            try {
                batched.fill_rect(0, 0, 1, 1);
            } catch (const std::invalid_argument&) {
                wrapped = true;
            }
            require(wrapped && batched.version == std::numeric_limits<std::uint64_t>::max(), "bitmap version wrapped");
            batched.version = before_version;
        }
        {
            weft::ScriptDom vm;
            auto real = weft::make_d2d_canvas_factory();
            unsigned calls{};
            vm.install_canvas_host([&](std::uint32_t w, std::uint32_t h, bool alpha) {
                if (++calls == 2) {
                    throw std::runtime_error("injected raster allocation failure");
                }
                return real(w, h, alpha);
            });
            vm.eval(
                "globalThis.c=document.createElement('canvas');c.id='fault';c.width=8;c.height=8;document.body.appendChild(c);c.getContext('2d').fillRect(0,0,8,8)");
            const auto old = vm.canvas_snapshot(vm.node_identity("#fault"));
            bool failed = false;
            try {
                vm.eval("c.width=4");
            } catch (...) {
                failed = true;
            }
            require(failed && vm.stats().canvas_backing_bytes == 8 * 8 * weft::constants::rgba8_bytes_per_pixel,
                    "backend failure did not preserve old raster");
            failed = false;
            try {
                vm.eval("document.createElement('div')");
            } catch (...) {
                failed = true;
            }
            require(failed, "backend resource failure did not fault View");
            vm.reset_document();
            require(vm.stats().canvas_backing_bytes == 0, "fault reset leaked raster");
            pixel(old, 1, 1, {0, 0, 0, 255});
        }
        CoUninitialize();
        std::cout << "PASS native Canvas2D pixels/state/resize/identity/events/GC/epoch/immutable snapshots\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
