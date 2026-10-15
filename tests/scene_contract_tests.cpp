#include "base/diagnostic.hpp"
#include "scene/scene.hpp"
#include "scene/scene_geometry.hpp"
#include "support/scene_fonts.hpp"
#include "script/script_platform.hpp"
#include "support/check.hpp"
#include "support/native_scene.hpp"
#include "../probes/gallery_scene_session.hpp"
#include "constants/scene.hpp"
#include "dom/tree.hpp"
#include "test_constants/scene.hpp"
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <windows.h>
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>

using weft::testing::require;

namespace {
    namespace dom = weft::dom;
    using weft::testing::NativeScene;
    constexpr int element_type = weft::constants::element_node_type;

    // The reference painter's GPU-target mode, which paints through retained paint programs, on a size x size CPU
    // bitmap: straight RGBA, valid for opaque pixels.
    std::vector<std::uint8_t> paint_target(weft::SceneRaster& raster, const weft::SceneFrame& frame, unsigned size) {
        using Microsoft::WRL::ComPtr;
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICBitmap> bitmap;
        ComPtr<ID2D1Factory> factory;
        ComPtr<ID2D1RenderTarget> target;
        require(
            SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
                SUCCEEDED(
                    wic->CreateBitmap(size, size, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)) &&
                SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf())) &&
                SUCCEEDED(factory->CreateWicBitmapRenderTarget(bitmap.Get(), D2D1::RenderTargetProperties(), &target)),
            "WIC render target");
        raster.paint_d2d(frame, target.Get(), 1);
        std::vector<std::uint8_t> out(std::size_t(size) * size * 4);
        require(SUCCEEDED(bitmap->CopyPixels(nullptr, size * 4, UINT(out.size()), out.data())), "WIC readback");
        for (std::size_t i = 0; i < out.size(); i += 4) {
            std::swap(out[i], out[i + 2]);
        }
        return out;
    }

    bool same_elements(const std::vector<weft::ElementGeometry>& a, const std::vector<weft::ElementGeometry>& b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            const auto& x = a[i];
            const auto& y = b[i];
            if (x.identity != y.identity || x.rect != y.rect || x.client_rects != y.client_rects ||
                x.client_width != y.client_width || x.client_height != y.client_height ||
                x.scroll_width != y.scroll_width || x.scroll_height != y.scroll_height) {
                return false;
            }
        }
        return true;
    }

    // Retained solvers, whose style input, previous frame and previous computed styles follow only what changed,
    // equal a full solve after random mutations: block heights that move unchanged siblings, paint-only changes the
    // layout reuses, SVG attributes the cascade does not read, selector context changes above unchanged elements,
    // custom properties nothing reads, hover, moves and viewport changes.
    void retained_frames_differential(weft::SceneRaster& raster) {
        NativeScene s;
        s.set(*s.root, "style", "position:relative;width:180px;height:220px;background-color:#000000");
        std::vector<dom::Node*> blocks;
        std::vector<dom::Node*> boxes; // Blocks and the boxes inside them.
        for (int i = 0; i < 4; ++i) {
            auto& block = s.html("div");
            s.set(block, "style", "height:" + std::to_string(10 + i) + "px;background-color:#202020");
            s.tree.insert(*s.root, block, nullptr);
            blocks.push_back(&block);
            boxes.push_back(&block);
            for (int j = 0; j < 2; ++j) {
                auto& inner = s.html("div");
                s.set(inner, "style",
                      "width:" + std::to_string(20 + 10 * j) + "px;height:4px;background-color:#404040");
                s.tree.insert(block, inner, nullptr);
                boxes.push_back(&inner);
            }
        }
        auto& vars = s.html("div");
        s.set(vars, "style", "--k:0;height:4px");
        s.tree.insert(*s.root, vars, nullptr);
        auto& chart = s.svg("svg");
        s.set(chart, "width", "120");
        s.set(chart, "height", "80");
        s.tree.insert(*s.root, chart, nullptr);
        std::vector<dom::Node*> groups;
        std::vector<dom::Node*> marks;
        for (int g = 0; g < 3; ++g) {
            auto& group = s.svg("g");
            s.tree.insert(chart, group, nullptr);
            groups.push_back(&group);
            for (int m = 0; m < 4; ++m) {
                auto& mark = s.svg(m == 3 ? "line" : "rect");
                if (m == 3) {
                    s.set(mark, "x1", "0");
                    s.set(mark, "y1", std::to_string(10 * g));
                    s.set(mark, "x2", "100");
                    s.set(mark, "y2", std::to_string(10 * g));
                    s.set(mark, "stroke", "#808080");
                } else {
                    s.set(mark, "x", std::to_string(30 * m));
                    s.set(mark, "y", std::to_string(20 * g));
                    s.set(mark, "width", "12");
                    s.set(mark, "height", "8");
                    s.set(mark, "fill", "#0000ff");
                }
                s.tree.insert(group, mark, nullptr);
                marks.push_back(&mark);
            }
        }
        const std::string sheet =
            ".on rect{fill:#ff0000}.on line{stroke:#00ff00}.hot{background-color:#336699}div:hover{background-color:#00ff00}.dim{opacity:.5}";
        auto& scene_text = weft::testing::thread_scene_text();
        weft::SceneSolver retained(sheet, "", scene_text);
        weft::SceneSolver owning(sheet, "", scene_text);
        weft::SceneSolver reference(sheet, "", scene_text, false);
        weft::SceneFrame retained_frame;
        std::uint32_t seed = 0x3c1;
        const auto next = [&] {
            seed = seed * weft::constants::lcg_multiplier + weft::constants::lcg_increment;
            return seed >> 8;
        };
        float width = 200;
        constexpr int steps = 600;
        for (int step = 0; step < steps; ++step) {
            switch (next() % 10) {
                case 0: {
                    auto& block = *blocks[next() % blocks.size()];
                    s.set(block, "style", "height:" + std::to_string(8 + next() % 9) + "px;background-color:#202020");
                    break;
                }
                case 1:
                    s.toggle_class(*boxes[next() % boxes.size()], "hot");
                    break;
                case 2: {
                    auto& box = *boxes[next() % boxes.size()];
                    box.hovered = !box.hovered;
                    s.tree.changed(box);
                    break;
                }
                case 3:
                    s.set(vars, "style", "--k:" + std::to_string(next() % 4) + ";height:4px");
                    break;
                case 4:
                    s.toggle_class(*groups[next() % groups.size()], "on");
                    break;
                case 5: {
                    auto& mark = *marks[next() % marks.size()];
                    s.set(mark, mark.local == "line" ? "x2" : "x", std::to_string(next() % 100));
                    break;
                }
                case 6: {
                    auto& mark = *marks[next() % marks.size()];
                    s.set(mark, mark.local == "line" ? "stroke" : "fill", next() % 2 ? "#0000ff" : "#ffff00");
                    break;
                }
                case 7:
                    s.toggle_class(*marks[next() % marks.size()], "dim");
                    break;
                case 8:
                    if (next() % 2) {
                        s.tree.insert(*groups[next() % groups.size()], *marks[next() % marks.size()], nullptr);
                    } else {
                        auto& block = *blocks[next() % blocks.size()];
                        s.tree.insert(*s.root, block, &s.tree.at(s.root->children[next() % s.root->children.size()]));
                    }
                    break;
                default:
                    width = next() % 4 ? 200.f : 190.f;
                    break;
            }
            const auto dom = s.tree.snapshot(*s.root);
            const auto at = " at step " + std::to_string(step);
            const double time = step * 16;
            retained.update(dom, retained_frame, width, weft::test_constants::scene::contract_view_size, time);
            const auto expected = reference.frame(dom, width, weft::test_constants::scene::contract_view_size, time);
            const auto owned = owning.frame(dom, width, weft::test_constants::scene::contract_view_size, time);
            require(retained_frame.root == expected.root && same_elements(retained_frame.elements, expected.elements),
                    "retained frame differs from a full solve" + at);
            require(weft::scene_paint_source_equal(owned.root, retained_frame.root) &&
                        same_elements(owned.elements, expected.elements),
                    "owned frame differs from the retained frame" + at);
            require(raster.raster(retained_frame).rgba == raster.raster(expected).rgba,
                    "retained pixels differ from a full solve" + at);
            // A first frame computes every numeric paint value, which == and the CPU raster leave out, from styles.
            weft::SceneSolver first(sheet, "", scene_text);
            require(weft::scene_paint_source_equal(
                        first.frame(dom, width, weft::test_constants::scene::contract_view_size, time).root,
                        retained_frame.root),
                    "retained paint values differ from a first frame" + at);
        }
        const auto work = retained.work();
        require(work.static_subtree_reuses > steps && work.layout_reuses > steps / 2 &&
                    work.layout_evaluations > steps / 8 && work.style_reuses > steps / 4,
                "random mutations did not exercise reuse and its invalidation");
    }

    bool same_geometry(const std::vector<weft::ElementGeometry>& a, const std::vector<weft::ElementGeometry>& b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            const auto& x = a[i];
            const auto& y = b[i];
            if (x.identity != y.identity || x.rect != y.rect || x.client_rects != y.client_rects ||
                x.client_width != y.client_width || x.client_height != y.client_height ||
                x.scroll_width != y.scroll_width || x.scroll_height != y.scroll_height || x.offset != y.offset ||
                x.matrix != y.matrix || x.offset_parent != y.offset_parent) {
                return false;
            }
        }
        return true;
    }

    // Hits of both maps agree on a grid over the frame.
    bool same_hits(const weft::SceneGeometry::HitMap& a, const weft::SceneGeometry::HitMap& b, float width,
                   float height) {
        constexpr float step = 5;
        for (float y = 0; y < height; y += step) {
            for (float x = 0; x < width; x += step) {
                if (a.hit(x, y) != b.hit(x, y)) {
                    return false;
                }
            }
        }
        return true;
    }

    // A long-lived SceneGeometry, which keeps each node's shape with the attributes it was built from and parsed
    // transforms by text, measures and hits a frame as a fresh one does after random changes of geometry attributes,
    // tags, fill rules, transforms, identities and parents.
    void geometry_cache_differential() {
        constexpr float width = 200;
        constexpr float height = 160;
        weft::SceneFrame frame;
        frame.root.identity = {1, 1, 1};
        frame.root.tag = "div";
        frame.root.space = weft::constants::xhtml_namespace_uri;
        frame.root.box = {0, 0, width, height};
        weft::SceneNode chart;
        chart.identity = {2, 1, 1};
        chart.tag = "svg";
        chart.space = weft::constants::svg_namespace_uri;
        chart.box = {10, 10, 180, 140};
        chart.attributes = {{"width", "180"}, {"height", "140"}};
        std::uint32_t slot = 3;
        for (int g = 0; g < 2; ++g) {
            weft::SceneNode group;
            group.identity = {slot++, 1, 1};
            group.tag = "g";
            group.space = chart.space;
            for (int m = 0; m < 6; ++m) {
                weft::SceneNode mark;
                mark.identity = {slot++, 1, 1};
                mark.tag = "rect";
                mark.space = chart.space;
                mark.attributes = {
                    {"x", std::to_string(25 * m)}, {"y", std::to_string(40 * g)}, {"width", "20"}, {"height", "12"}};
                mark.style["stroke"] = "#ffffff";
                group.children.push_back(std::move(mark));
            }
            chart.children.push_back(std::move(group));
        }
        frame.root.children.push_back(std::move(chart));
        static const std::array<const char*, 7> tags{"rect", "circle",  "ellipse", "line",
                                                     "path", "polygon", "polyline"};
        static const std::array<const char*, 13> numbers{"x",  "y",  "width", "height", "r",  "cx", "cy",
                                                         "rx", "ry", "x1",    "y1",     "x2", "y2"};
        static const std::array<const char*, 3> paths{"M0 0 L30 0 L30 20 Z", "M5 5 C20 0 40 30 10 25",
                                                      "M0 0 A10 10 0 0 1 20 20"};
        // The last is a pentagram, whose center the fill rule decides.
        static const std::array<const char*, 3> points{"0,0 30,0 15,25", "0,0 20,10 0,20 10,30",
                                                       "50,0 79,90 2,35 98,35 21,90"};
        static const std::array<const char*, 4> transforms{"translate(10,5)", "scale(1.5)", "rotate(30 20 20)",
                                                           "translate(3,4) scale(0.5)"};
        std::uint32_t seed = 0x6e0;
        const auto next = [&] {
            seed = seed * weft::constants::lcg_multiplier + weft::constants::lcg_increment;
            return seed >> 8;
        };
        weft::SceneGeometry cached{weft::testing::thread_scene_text()};
        constexpr int steps = 300;
        for (int step = 0; step < steps; ++step) {
            // Taken anew each step: frames the geometry keeps may share the groups since the last.
            auto& groups = frame.root.children.edit(0).children;
            auto& group = groups.edit(next() % groups.size());
            if (group.children.empty()) {
                continue;
            }
            const auto mark_index = next() % group.children.size();
            auto& mark = group.children.edit(mark_index);
            switch (next() % 9) {
                case 0:
                case 1:
                    mark.attributes[numbers[next() % numbers.size()]] = std::to_string(next() % 60);
                    break;
                case 2:
                    mark.attributes["d"] = paths[next() % paths.size()];
                    mark.attributes["points"] = points[next() % points.size()];
                    break;
                case 3:
                    if (!mark.attributes.empty()) {
                        auto it = mark.attributes.begin();
                        std::advance(it, next() % mark.attributes.size());
                        mark.attributes.erase(it);
                    }
                    break;
                case 4:
                    mark.tag = tags[next() % tags.size()];
                    break;
                case 5:
                    if (next() % 2) {
                        mark.style["fill-rule"] = "evenodd";
                    } else {
                        mark.style.erase(weft::PropertyId::FillRule);
                    }
                    break;
                case 6: {
                    auto& target = next() % 2 ? group : mark;
                    if (next() % 3) {
                        target.attributes["transform"] = transforms[next() % transforms.size()];
                    } else {
                        target.attributes.erase("transform");
                    }
                    break;
                }
                case 7: {
                    // Two nodes trade identities, as a reused slot would give a node another's.
                    auto& other = groups.edit(next() % groups.size());
                    if (!other.children.empty()) {
                        std::swap(mark.identity, other.children.edit(next() % other.children.size()).identity);
                    }
                    break;
                }
                default: {
                    auto moved = mark;
                    group.children.erase(group.children.begin() + std::ptrdiff_t(mark_index));
                    groups.edit(next() % groups.size()).children.push_back(std::move(moved));
                    break;
                }
            }
            const auto at = " at step " + std::to_string(step);
            weft::SceneGeometry fresh{weft::testing::thread_scene_text()};
            require(same_geometry(cached.geometry(frame, true), fresh.geometry(frame, true)),
                    "kept shapes measured differently" + at);
            require(same_hits(cached.hit_map(frame), fresh.hit_map(frame), width, height),
                    "kept shapes hit differently" + at);
        }
        // The fill rule decides a pentagram's center: kept shapes follow it.
        auto& groups = frame.root.children.edit(0).children;
        auto kept = groups[0].children.empty() ? groups[1].children[0] : groups[0].children[0];
        for (auto& group : groups.edit_all()) {
            group.attributes.erase("transform");
            group.children.clear();
        }
        kept.tag = "polygon";
        kept.attributes = {{"points", points[2]}};
        groups.edit(0).children.push_back(std::move(kept));
        auto& star = groups.edit(0).children.edit(0);
        const auto& base = frame.root.children[0].box;
        const float cx = base[0] + 50;
        const float cy = base[1] + 50;
        std::array<weft::NodeIdentity, 2> center{};
        for (const auto rule : {"evenodd", "nonzero"}) {
            star.style["fill-rule"] = rule;
            weft::SceneGeometry fresh{weft::testing::thread_scene_text()};
            const auto expected = fresh.hit_map(frame).hit(cx, cy);
            require(cached.hit_map(frame).hit(cx, cy) == expected, std::string("pentagram center under ") + rule);
            center[rule[0] == 'n'] = expected;
        }
        require(center[0] != center[1] && center[1] == star.identity, "fill rule changed no hit");
    }
} // namespace

int main() {
    try {
        struct ComScope {
            ComScope() {
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            }

            ~ComScope() {
                CoUninitialize();
            }
        } com;

        weft::ScriptDom vm;
        weft::install_script_platform(vm, weft::ScriptPlatformModule::WebHost);
        weft::install_script_platform(vm, weft::ScriptPlatformModule::AnimationFrame);
        require(
            vm.eval(
                R"JS((()=>{const root=document.getElementById('root');const parent=document.createElementNS('http://www.w3.org/2000/svg','g');parent.setAttribute('class','tick w-[65%]');const a=document.createElementNS(parent.namespaceURI,'line'),b=document.createElementNS(parent.namespaceURI,'text');a.setAttribute('x1','4');a.setAttribute('data-kind','axis-major');parent.appendChild(a);parent.appendChild(b);root.appendChild(parent);const code=s=>{try{root.querySelectorAll(s)}catch(e){return e.message.split(':')[0]}return ''};const errors=[' ','line,','[x1=4]'].filter(s=>code(s)==='WEFT_SELECTOR_SYNTAX').length+['.tick + line','g ~ text','line:focus-visible','[x1="4" i]','text::before'].filter(s=>code(s)==='WEFT_UNSUPPORTED_SELECTOR').length;return root.querySelectorAll('.tick line').length===1&&root.querySelectorAll('line,.tick line').length===1&&a.matches('g line')&&root.querySelector('.tick text')===b&&!b.matches('.tick line')&&root.querySelectorAll('.tick > line').length===1&&!root.querySelector('#root > line')&&root.querySelector('[x1]')===a&&root.querySelector('line[x1="4"]')===a&&!root.querySelector('[x1="5"]')&&root.querySelector('g[class~=tick] > text')===b&&root.querySelector('[data-kind|=axis]')===a&&root.querySelector('[data-kind^=axis][data-kind$=major][data-kind*=s-m]')===a&&root.querySelector('.w-\\[65\\%\\]')===parent&&!root.querySelector('LINE')&&document.querySelector('DIV#root')===root&&errors===8})())JS") == "true",
            "shared DOM selector grammar: combinators, attributes, escapes, case and diagnostics");
        // closest: the element itself or its nearest matching ancestor element (as the CRPG page's pinned windows
        // ask of a mousedown target), never the document; null without one; the selector list's diagnostics.
        require(
            vm.eval(
                R"JS((()=>{const root=document.getElementById('root');const pinned=document.createElement('div');pinned.setAttribute('data-pinned-tooltip','');const body=document.createElement('section');const span=document.createElement('span');span.setAttribute('data-keyword-span','');const b=document.createElement('b');span.appendChild(b);body.appendChild(span);pinned.appendChild(body);root.appendChild(pinned);const code=s=>{try{b.closest(s)}catch(e){return e.message.split(':')[0]}return ''};return b.closest('[data-pinned-tooltip]')===pinned&&b.closest('[data-keyword-span]')===span&&span.closest('[data-keyword-span]')===span&&b.closest('b')===b&&b.closest('section > span')===span&&b.closest('nav,section')===body&&b.closest('[data-missing]')===null&&root.closest('*')===root&&code('b,')==='WEFT_SELECTOR_SYNTAX'&&code('b ~ i')==='WEFT_UNSUPPORTED_SELECTOR'})())JS") ==
                "true",
            "closest: itself or the nearest matching ancestor element, null without one, selector diagnostics");
        require(
            vm.eval(
                R"JS((()=>{globalThis.timer=[];let id=setInterval(function(a){timer.push([a,this===globalThis,performance.now()]);if(timer.length===2)clearTimeout(id)},5,'arg');__ceHost.pump(4);__ceHost.pump(5);__ceHost.pump(100);__ceHost.pump(101);return JSON.stringify(timer)==='[["arg",true,5],["arg",true,100]]'&&__ceHost.pending()===0})())JS") ==
                "true",
            "interval pause coalescing/self cancellation/shared ids");
        require(
            vm.eval(
                R"JS((()=>{let id=setInterval(()=>{throw Error('timer failure')},1);__ceHost.pump(102);let error=false;try{__ceHost.pump(102)}catch(e){error=String(e).includes('timer failure')}clearInterval(id);return error&&__ceHost.pending()===0})())JS") ==
                "true",
            "interval error/cancel leaked task");
        vm.reset_document();
        weft::install_script_platform(vm, weft::ScriptPlatformModule::WebHost);
        weft::SceneRaster raster{weft::testing::thread_scene_text()};
        weft::SceneGeometry geometry{weft::testing::thread_scene_text()};
        weft::SceneSolver scene(
            ".bg-gold\\/20{background-color:rgba(212,168,67,.2)}.parent{transition-property:color;transition-duration:1s;transition-timing-function:linear}.explicit{color:#ff0000}",
            "@keyframes k{from{transform:translateX(0);opacity:0}to{transform:translateX(20px);opacity:1}}",
            weft::testing::thread_scene_text());
        vm.eval(
            "document.getElementById('root').setAttribute('style','background:#000000;position:relative');const p=document.createElement('div');p.setAttribute('class','parent');p.setAttribute('style','color:#000000');const a=document.createElement('span');a.textContent='A';const b=document.createElement('span');b.setAttribute('class','explicit');b.textContent='B';p.appendChild(a);p.appendChild(b);document.getElementById('root').appendChild(p)");
        scene.frame(vm.snapshot_subtree("#root"), weft::test_constants::scene::timeline_view_size,
                    weft::test_constants::scene::timeline_view_size, 0);
        vm.eval("p.style.color='#ffffff'");
        scene.frame(vm.snapshot_subtree("#root"), weft::test_constants::scene::timeline_view_size,
                    weft::test_constants::scene::timeline_view_size, 10);
        auto f = scene.frame(vm.snapshot_subtree("#root"), weft::test_constants::scene::timeline_view_size,
                             weft::test_constants::scene::timeline_view_size, 510);
        auto c = weft::parse_css_color(f.root.children[0].children[0].style.at("color"));
        require(std::abs(c[0] - .5f) < weft::test_constants::scene::paint_sample_tolerance,
                "animated inherited color jumped to target");
        require(f.root.children[0].children[1].style.at("color") == "#ff0000",
                "animation replaced explicitly specified child color");
        vm.eval(
            "document.getElementById('root').textContent='';const n=document.createElement('div');n.setAttribute('class','bg-gold/20');n.setAttribute('style','position:absolute;left:10px;top:10px;width:20px;height:20px;animation:k 1s linear forwards;animation-delay:.5s');document.getElementById('root').appendChild(n)");
        auto a = scene.frame(vm.snapshot_subtree("#root"), weft::test_constants::scene::timeline_view_size,
                             weft::test_constants::scene::timeline_view_size, 600);
        require(!a.root.children[0].style.contains("transform"), "animation delay applied without backwards fill");
        auto mid = scene.frame(vm.snapshot_subtree("#root"), weft::test_constants::scene::timeline_view_size,
                               weft::test_constants::scene::timeline_view_size, 1600);
        require(mid.root.children[0].style.at("transform") == "translateX(10px)",
                "unitless zero transform did not interpolate");
        require(geometry.hit_test(mid, 35, 15) == mid.root.children[0].identity &&
                    geometry.hit_test(mid, 15, 15) != mid.root.children[0].identity,
                "transformed hit region used stale box");
        auto bitmap = raster.raster(mid);
        require(bitmap.rgba[(15 * weft::test_constants::scene::timeline_view_size + 35) * 4] > 10 &&
                    bitmap.rgba[(15 * weft::test_constants::scene::timeline_view_size + 15) * 4] == 0,
                "transform/opacity/escaped utility raster");
        auto end = scene.frame(vm.snapshot_subtree("#root"), weft::test_constants::scene::timeline_view_size,
                               weft::test_constants::scene::timeline_view_size, 2100);
        require(end.root.children[0].style.at("transform") == "translateX(20px)", "forwards end fill");
        vm.eval("document.getElementById('root').removeChild(n)");
        scene.frame(vm.snapshot_subtree("#root"), weft::test_constants::scene::timeline_view_size,
                    weft::test_constants::scene::timeline_view_size, 2200);
        vm.eval("document.getElementById('root').appendChild(n)");
        auto reinsert = scene.frame(vm.snapshot_subtree("#root"), weft::test_constants::scene::timeline_view_size,
                                    weft::test_constants::scene::timeline_view_size, 2300);
        require(!reinsert.root.children[0].style.contains("transform"), "removed animation state crossed reinsertion");
        bool invalid = false;
        try {
            scene.frame(vm.snapshot_subtree("#root"), weft::test_constants::scene::timeline_view_size,
                        weft::test_constants::scene::timeline_view_size, 2299);
        } catch (const std::exception&) {
            invalid = true;
        }
        require(invalid, "backwards scene clock");
        invalid = false;
        try {
            weft::compute_style(vm.snapshot_subtree("#root"), ".bg-gold\\/20{background-color:#ffffff}",
                                weft::document_style_profile());
        } catch (const std::exception&) {
            invalid = true;
        }
        require(invalid, "legacy CSS profile silently gained escaped selectors");

        require(
            vm.eval(
                R"JS((()=>{let rejected=0;for(const value of [1n,Symbol('delay')]){try{setTimeout(()=>{},value)}catch(e){rejected++}try{clearInterval(value)}catch(e){rejected++}}let value=0;const id=setTimeout(()=>value++,2.9);__ceHost.pump(1);const before=value;__ceHost.pump(2);clearInterval(id);return rejected===4&&before===0&&value===1})())JS") ==
                "true",
            "timer WebIDL coercion / fractional delay");
        // Hit testing stacks siblings as paint does: z-index auto at 0, under a later sibling's positive z-index.
        vm.reset_document();
        weft::install_script_platform(vm, weft::ScriptPlatformModule::WebHost);
        vm.eval(
            "const s=document.getElementById('root');s.setAttribute('style','position:relative');const raised=document.createElement('div');raised.setAttribute('style','position:absolute;left:0;top:0;width:20px;height:20px;z-index:1');const flat=document.createElement('div');flat.setAttribute('style','position:absolute;left:0;top:0;width:20px;height:20px;z-index:auto');s.appendChild(raised);s.appendChild(flat)");
        weft::SceneSolver stacking("", "", weft::testing::thread_scene_text());
        const auto stacked =
            stacking.frame(vm.snapshot_subtree("#root"), weft::test_constants::scene::contract_view_size,
                           weft::test_constants::scene::contract_view_size, 0);
        require(geometry.hit_test(stacked, 10, 10) == stacked.root.children[0].identity,
                "hit testing ignored z-index or refused z-index auto");
        // Hits follow the painted corners: a disc's bounding-box corner is not the disc, and a rounded overflow clip
        // clips its children's hits too.
        vm.reset_document();
        weft::install_script_platform(vm, weft::ScriptPlatformModule::WebHost);
        vm.eval(
            "const c=document.getElementById('root');c.setAttribute('style','position:relative');const disc=document.createElement('div');disc.setAttribute('style','position:absolute;left:0;top:0;width:40px;height:40px;border-radius:20px');const ring=document.createElement('div');ring.setAttribute('style','position:absolute;left:50px;top:0;width:40px;height:40px;border-radius:20px;overflow:hidden');const fill=document.createElement('div');fill.setAttribute('style','width:40px;height:40px');ring.appendChild(fill);c.appendChild(disc);c.appendChild(ring)");
        weft::SceneSolver rounding("", "", weft::testing::thread_scene_text());
        const auto corners =
            rounding.frame(vm.snapshot_subtree("#root"), weft::test_constants::scene::contract_view_size,
                           weft::test_constants::scene::contract_view_size, 0);
        const auto disc = corners.root.children[0].identity;
        const auto fill = corners.root.children[1].children[0].identity;
        require(geometry.hit_test(corners, 20, 20) == disc && geometry.hit_test(corners, 2, 2) != disc,
                "hit outside a rounded corner");
        require(geometry.hit_test(corners, 70, 20) == fill && geometry.hit_test(corners, 52, 2) != fill &&
                    geometry.hit_test(corners, 52, 2) != corners.root.children[1].identity,
                "hit outside a rounded overflow clip");
        // An overflow clip is the padding box: a child covering its parent's border box shows and hits only inside
        // the borders, and the inner corner follows the inner radius.
        vm.reset_document();
        weft::install_script_platform(vm, weft::ScriptPlatformModule::WebHost);
        vm.eval(
            "const o=document.getElementById('root');o.setAttribute('style','position:relative;background-color:#000000');const frame=document.createElement('div');frame.setAttribute('style','position:absolute;left:10px;top:10px;width:60px;height:60px;border:8px solid #ffffff;border-radius:16px;overflow:hidden');const cover=document.createElement('div');cover.setAttribute('style','position:absolute;left:-20px;top:-20px;width:100px;height:100px;background-color:#ff0000');frame.appendChild(cover);o.appendChild(frame)");
        weft::SceneSolver bordered("", "", weft::testing::thread_scene_text());
        const auto padded =
            bordered.frame(vm.snapshot_subtree("#root"), weft::test_constants::scene::contract_view_size,
                           weft::test_constants::scene::contract_view_size, 0);
        const auto border_owner = padded.root.children[0].identity;
        const auto cover = padded.root.children[0].children[0].identity;
        // (40, 13) is in the top border, (13, 40) in the left one, (40, 40) inside; (19, 19) is inside the outer
        // corner but outside the inner one (radius 8 around (26, 26)).
        for (const auto [x, y] : std::array<std::array<float, 2>, 3>{{{40, 13}, {13, 40}, {19, 19}}}) {
            require(geometry.hit_test(padded, x, y) == border_owner && raster.hit_test(padded, x, y) == border_owner,
                    "overflow clip hit outside the padding box");
        }
        require(geometry.hit_test(padded, 40, 40) == cover && raster.hit_test(padded, 40, 40) == cover,
                "hit inside the padding box");
        const auto view = weft::test_constants::scene::contract_view_size;
        // The CPU raster, and the GPU-target mode that paints through retained paint programs.
        for (const auto& pixels : {raster.raster(padded).rgba, paint_target(raster, padded, view)}) {
            const auto red = [&](unsigned x, unsigned y) {
                const auto* p = pixels.data() + (std::size_t(y) * view + x) * 4;
                return p[0] > 200 && p[1] < 60 && p[2] < 60;
            };
            require(!red(40, 13) && !red(13, 40) && !red(19, 19) && red(40, 40) && red(27, 27),
                    "overflow clip paints outside the padding box");
        }
        vm.reset_document();
        weft::install_script_platform(vm, weft::ScriptPlatformModule::WebHost);
        vm.eval(
            "const r=document.getElementById('root');r.style.backgroundColor='#000000';const parent=document.createElement('div');parent.setAttribute('style','width:30px;height:30px;transform:scale(1)');const inner=document.createElement('div');inner.setAttribute('style','position:absolute;inset:0;background-color:#ffffff');parent.appendChild(inner);r.appendChild(parent)");
        weft::SceneSolver containing("", "", weft::testing::thread_scene_text());
        auto contained = containing.frame(vm.snapshot_subtree("#root"), weft::test_constants::scene::contract_view_size,
                                          weft::test_constants::scene::contract_view_size, 0);
        require(contained.root.children[0].children[0].box[2] == 30 &&
                    contained.root.children[0].children[0].box[3] == 30,
                "transform failed to establish absolute containing block");
        auto frozen = contained;
        bool wrong_owner = false;
        bool wrong_raster = false;
        auto owner_snapshot = vm.snapshot_subtree("#root");
        std::thread outsider([&] {
            try {
                containing.frame(owner_snapshot, weft::test_constants::scene::contract_view_size,
                                 weft::test_constants::scene::contract_view_size, 0);
            } catch (const std::logic_error&) {
                wrong_owner = true;
            }
            try {
                raster.raster(frozen);
            } catch (const std::logic_error&) {
                wrong_raster = true;
            }
        });
        outsider.join();
        require(wrong_owner && wrong_raster, "scene adapter permitted foreign-thread access");
        vm.reset_document();
        vm.collect_garbage();
        require(raster.raster(frozen).rgba[(5 * weft::test_constants::scene::contract_view_size + 5) * 4] ==
                    weft::constants::color_channel_max,
                "owned scene lost pixels after DOM teardown");
        weft::install_script_platform(vm, weft::ScriptPlatformModule::WebHost);
        vm.eval(
            "const root=document.getElementById('root');const n=document.createElement('div');n.setAttribute('style','width:20px;height:20px;color:#0000ff;border-width:2px;animation:chase 1s linear infinite');root.appendChild(n)");
        weft::SceneSolver chase("", "@keyframes chase{75%{border-color:#ff0000}}", weft::testing::thread_scene_text());
        chase.frame(vm.snapshot_subtree("#root"), weft::test_constants::scene::contract_view_size,
                    weft::test_constants::scene::contract_view_size, 0);
        auto implicit = chase.frame(vm.snapshot_subtree("#root"), weft::test_constants::scene::contract_view_size,
                                    weft::test_constants::scene::contract_view_size, 875);
        auto border = weft::parse_css_color(implicit.root.children[0].style.at("border-top-color"));
        require(std::abs(border[0] - .5f) < weft::test_constants::scene::paint_sample_tolerance &&
                    std::abs(border[2] - .5f) < weft::test_constants::scene::paint_sample_tolerance,
                "implicit border keyframe end did not use underlying currentcolor");
        vm.eval("n.setAttribute('style','width:20px;height:20px;animation:fade 1s linear infinite alternate-reverse')");
        weft::SceneSolver reverse("", "@keyframes fade{from{opacity:0}to{opacity:1}}",
                                  weft::testing::thread_scene_text());
        reverse.frame(vm.snapshot_subtree("#root"), weft::test_constants::scene::contract_view_size,
                      weft::test_constants::scene::contract_view_size, 0);
        require(
            std::abs(std::stof(reverse
                                   .frame(vm.snapshot_subtree("#root"), weft::test_constants::scene::contract_view_size,
                                          weft::test_constants::scene::contract_view_size, 250)
                                   .root.children[0]
                                   .style.at("opacity")) -
                     .75f) < weft::test_constants::scene::paint_sample_tolerance,
            "alternate-reverse first iteration");
        require(
            std::abs(std::stof(reverse
                                   .frame(vm.snapshot_subtree("#root"), weft::test_constants::scene::contract_view_size,
                                          weft::test_constants::scene::contract_view_size, 1250)
                                   .root.children[0]
                                   .style.at("opacity")) -
                     .25f) < weft::test_constants::scene::paint_sample_tolerance,
            "alternate-reverse second iteration");
        // Keyframe declarations name registered properties; anything else is a scene profile diagnostic.
        bool unknown_keyframe{};
        try {
            weft::SceneSolver bad("", "@keyframes fade{from{shimmer:1}to{opacity:1}}",
                                  weft::testing::thread_scene_text());
            bad.frame(vm.snapshot_subtree("#root"), weft::test_constants::scene::contract_view_size,
                      weft::test_constants::scene::contract_view_size, 0);
        } catch (const weft::DiagnosticError& error) {
            unknown_keyframe =
                error.code() == weft::Diag::SceneProfile &&
                std::string(error.what()).find("unsupported keyframe property shimmer") != std::string::npos;
        }
        require(unknown_keyframe, "unregistered keyframe property not diagnosed");
        vm.eval(
            "const svg=document.createElementNS('http://www.w3.org/2000/svg','svg');svg.setAttribute('viewBox','0 0 10 10');document.getElementById('root').appendChild(svg)");
        invalid = false;
        try {
            reverse.frame(vm.snapshot_subtree("#root"), weft::test_constants::scene::contract_view_size,
                          weft::test_constants::scene::contract_view_size, 1300);
        } catch (const std::invalid_argument&) {
            invalid = true;
        }
        require(!invalid, "supported SVG viewBox rejected");
        vm.reset_document();
        weft::install_script_platform(vm, weft::ScriptPlatformModule::WebHost);
        const std::string static_sheet = ".shade{background-color:#d4a843}.other{background-color:#225577}";
        weft::SceneSolver persisted(static_sheet, "", weft::testing::thread_scene_text());
        vm.eval(
            "const root=document.getElementById('root');root.setAttribute('style','background-color:#000000;position:relative;overflow:hidden');globalThis.boxes=[];globalThis.parents=[];for(let i=0;i<2;i++){let p=document.createElement('div');p.setAttribute('style','position:absolute;left:'+i*40+'px;top:0px;width:40px;height:80px');root.appendChild(p);parents.push(p)}for(let i=0;i<24;i++){let b=document.createElement('div');b.setAttribute('class','shade');b.setAttribute('style','position:absolute;left:'+i%6*5+'px;top:'+Math.floor(i/6)*15+'px;width:8px;height:8px');parents[i%2].appendChild(b);boxes.push(b)}let seed=0x9e3779b9;globalThis.mutate=()=>{seed=(Math.imul(seed,1664525)+1013904223)>>>0;let b=boxes[seed%24],op=(seed>>>8)%4;if(op===0){b.style.transform='translateX('+((seed>>>16)%8)+'px)'}else if(op===1)b.setAttribute('class',(seed&1)?'shade':'other');else if(op===2){if(b.parentNode)b.parentNode.removeChild(b);else parents[seed%2].appendChild(b)}else parents[seed%2].appendChild(b)}");
        for (unsigned iteration = 0; iteration < weft::test_constants::scene::mutation_iterations; ++iteration) {
            vm.eval("mutate()");
            auto snapshot = vm.snapshot_subtree("#root");
            auto actual = persisted.frame(snapshot, weft::test_constants::scene::contract_view_size,
                                          weft::test_constants::scene::contract_view_size, iteration);
            weft::SceneSolver fresh(static_sheet, "", weft::testing::thread_scene_text(), false);
            auto expected = fresh.frame(snapshot, weft::test_constants::scene::contract_view_size,
                                        weft::test_constants::scene::contract_view_size, iteration);
            auto a_geometry = geometry.geometry(actual);
            auto b_geometry = geometry.geometry(expected);
            require(a_geometry.size() == b_geometry.size(), "mutation changed retained scene node count");
            for (std::size_t i = 0; i < a_geometry.size(); ++i) {
                require(a_geometry[i].identity == b_geometry[i].identity && a_geometry[i].rect == b_geometry[i].rect,
                        "retained mutation geometry differs from fresh full scene");
            }
            require(raster.raster(actual).rgba == raster.raster(expected).rgba,
                    "retained mutation pixels differ from fresh full scene");
            if (iteration % weft::test_constants::scene::collect_garbage_interval == 0) {
                vm.collect_garbage();
            }
        }
        require(persisted.work().layout_reuses > 0, "mutation did not exercise layout cache");
        vm.reset_document();
        weft::install_script_platform(vm, weft::ScriptPlatformModule::WebHost);
        vm.eval(
            "const root=document.getElementById('root');root.setAttribute('style','position:relative;background-color:#000000');let n=document.createElement('div');n.setAttribute('style','position:relative;width:30px;height:30px;color:#fff;animation:move 1s linear infinite');n.textContent='x';root.appendChild(n)");
        const std::string keyframes =
            "@keyframes move{from{transform:translateX(0);color:#000000}to{transform:translateX(20px);color:#ffffff}}";
        weft::SceneSolver retained("", keyframes, weft::testing::thread_scene_text());
        weft::SceneSolver full("", keyframes, weft::testing::thread_scene_text(), false);
        for (unsigned i = 0; i < 40; ++i) {
            if (i == 10) {
                vm.eval("n.style.width='40px'");
            }
            if (i == 15) {
                vm.eval("n.style.fontSize='20px'");
            }
            if (i == 20) {
                vm.eval("n.textContent='中文x'");
            }
            if (i == 25) {
                vm.eval("n.style.transform='none'");
            }
            if (i == 30) {
                vm.eval("n.style.display='none'");
            }
            auto d = vm.snapshot_subtree("#root");
            auto w = i >= 35 ? 90.f : static_cast<float>(weft::test_constants::scene::contract_view_size);
            auto optimized_frame = retained.frame(d, w, weft::test_constants::scene::contract_view_size, i * 25);
            auto reference_frame = full.frame(d, w, weft::test_constants::scene::contract_view_size, i * 25);
            auto ag = geometry.geometry(optimized_frame);
            auto bg = geometry.geometry(reference_frame);
            require(ag.size() == bg.size(), "timeline cache geometry count");
            for (std::size_t j = 0; j < ag.size(); ++j) {
                require(ag[j].identity == bg[j].identity && ag[j].rect == bg[j].rect, "timeline cache geometry");
            }
            require(raster.raster(optimized_frame).rgba == raster.raster(reference_frame).rgba,
                    "timeline cache pixels");
        }
        require(retained.work().style_reuses > 0 && retained.work().layout_reuses > 0 &&
                    retained.work().layout_evaluations < full.work().layout_evaluations,
                "cache reference path was not independent");
        vm.reset_document();
        weft::install_script_platform(vm, weft::ScriptPlatformModule::WebHost);
        vm.eval(
            "let root=document.getElementById('root');let svg=document.createElementNS('http://www.w3.org/2000/svg','svg');svg.setAttribute('width','20');svg.setAttribute('height','20');let rect=document.createElementNS(svg.namespaceURI,'rect');rect.setAttribute('width','5');rect.setAttribute('height','10');rect.setAttribute('fill','#ff0000');svg.appendChild(rect);root.appendChild(svg)");
        weft::SceneSolver keyed("rect[width='10']{fill:#00ff00}", "", weft::testing::thread_scene_text());
        keyed.frame(vm.snapshot_subtree("#root"), weft::test_constants::scene::contract_view_size,
                    weft::test_constants::scene::contract_view_size, 0);
        vm.eval("rect.setAttribute('x','5')");
        keyed.frame(vm.snapshot_subtree("#root"), weft::test_constants::scene::contract_view_size,
                    weft::test_constants::scene::contract_view_size, 1);
        require(keyed.work().style_reuses == 1 && keyed.work().layout_reuses == 1,
                "SVG paint attribute invalidated cascade/layout");
        vm.eval("rect.setAttribute('width','10')");
        auto selected = keyed.frame(vm.snapshot_subtree("#root"), weft::test_constants::scene::contract_view_size,
                                    weft::test_constants::scene::contract_view_size, 2);
        require(selected.root.children[0].children[0].style.at("fill") == "#00ff00",
                "attribute selector dependency was omitted from style key");
        vm.eval("rect.setAttribute('font-size','30px')");
        keyed.frame(vm.snapshot_subtree("#root"), weft::test_constants::scene::contract_view_size,
                    weft::test_constants::scene::contract_view_size, 3);
        require(keyed.work().style_evaluations == 3, "SVG presentation attribute did not invalidate cascade");
        keyed.reset();
        require(keyed.work().style_evaluations == 0 && keyed.work().layout_reuses == 0,
                "scene reset retained cache evidence");
        {
            auto dom = vm.snapshot_subtree("#root");
            dom.attributes["class"] = "theme";
            dom.children[0].attributes["class"] = "box";
            const std::string css =
                ".theme:hover .box{color:#ffffff}.box:active{opacity:.5}[style] > .box{width:inherit}rect{fill:#335577}";
            weft::CompiledStyle styles(css, weft::scene_style_profile());
            weft::StyleMemo memo;
            weft::ComputedStyle values;
            for (unsigned i = 0; i < 128; ++i) {
                dom.hovered = bool(i & 1);
                dom.children[0].active = bool(i & 2);
                dom.children[0].hovered = bool(i & 4);
                dom.attributes["style"] = "width:" + std::to_string(60 + i % 17) +
                                          "px;color:#112233;background-color:#" +
                                          std::string(i & 8 ? "445566" : "667788");
                dom.children[0].children[0].attributes["fill"] = i & 16 ? "#ff0000" : "#00ff00";
                styles.update(dom, values, memo);
                const auto expected = weft::compute_style(dom, css, weft::scene_style_profile());
                auto same = [](auto&& self, const auto& a, const auto& b) -> bool {
                    if (a.identity != b.identity || a.values != b.values ||
                        a.custom_properties != b.custom_properties ||
                        a.specified_properties != b.specified_properties ||
                        a.invalid_at_computed_value != b.invalid_at_computed_value ||
                        a.children.size() != b.children.size()) {
                        return false;
                    }
                    for (std::size_t j = 0; j < a.children.size(); ++j) {
                        if (!self(self, a.children[j], b.children[j])) {
                            return false;
                        }
                    }
                    return true;
                };
                require(same(same, values, expected),
                        "state, ancestor style selector, explicit inheritance or SVG cascade memo differed from full");
                auto normalized = dom;
                styles.normalize_selector_state(normalized);
                require(weft::compute_style(normalized, css, weft::scene_style_profile()).children[0].values ==
                            expected.children[0].values,
                        "irrelevant state normalization changed selector matching");
            }
        }
        vm.reset_document();
        weft::install_script_platform(vm, weft::ScriptPlatformModule::WebHost);
        vm.eval(
            "const root=document.getElementById('root');root.style.backgroundColor='#000000';let parent=document.createElement('div');parent.style.color='#0000ff';let n=document.createElement('div');n.setAttribute('style','width:20px;height:20px;background-color:#445566;border-width:2px;animation:motion 1s linear infinite alternate-reverse, shade 800ms steps(4,end) infinite;animation-delay:100ms,0ms');n.textContent='x';parent.appendChild(n);root.appendChild(parent)");
        const std::string tracks =
            "@keyframes motion{25%{transform:translateX(0)}75%{transform:translateX(20px);border-color:#ff0000}}@keyframes shade{from{opacity:.2}to{opacity:.8}}";
        weft::SceneSolver planned("", tracks, weft::testing::thread_scene_text());
        weft::SceneSolver unplanned("", tracks, weft::testing::thread_scene_text(), false);
        weft::SceneSolver retained_solver("", tracks, weft::testing::thread_scene_text());
        weft::SceneFrame retained_frame;
        for (unsigned i = 0; i < 160; ++i) {
            if (i == 12) {
                vm.eval("parent.style.color='#00ff00'");
            }
            if (i == 25) {
                vm.eval("n.style.animationDuration='2s,300ms'");
            }
            if (i == 37) {
                vm.eval("n.style.animationDelay='-200ms,50ms'");
            }
            if (i == 50) {
                vm.eval("n.style.animationTimingFunction='cubic-bezier(.42,0,.58,1),linear'");
            }
            if (i == 63) {
                vm.eval("n.style.width='30px';n.style.borderColor='#ffffff'");
            }
            if (i == 76) {
                vm.eval("parent.removeChild(n)");
            }
            if (i == 78) {
                vm.eval("parent.appendChild(n)");
            }
            if (i == 91) {
                vm.eval("n.style.animation='shade 500ms linear infinite, motion 1s ease infinite alternate'");
            }
            if (i == 104) {
                vm.eval("n.style.fontSize='20px';n.textContent='中文'");
            }
            if (i == 117) {
                vm.eval("n.style.animation='none'");
            }
            if (i == 130) {
                vm.eval("n.style.animation='motion 1s linear infinite'");
            }
            if (i >= 133) {
                vm.eval("n.style.transform='translateX(" + std::to_string(i) + "px)'");
            }
            auto dom = vm.snapshot_subtree("#root");
            auto sampled = planned.frame(dom, weft::test_constants::scene::contract_view_size,
                                         weft::test_constants::scene::contract_view_size, i * 25);
            auto uncached = unplanned.frame(dom, weft::test_constants::scene::contract_view_size,
                                            weft::test_constants::scene::contract_view_size, i * 25);
            retained_solver.update(dom, retained_frame, weft::test_constants::scene::contract_view_size,
                                   weft::test_constants::scene::contract_view_size, i * 25);
            require(retained_frame.root == uncached.root &&
                        weft::scene_paint_source_equal(retained_frame.root, sampled.root) &&
                        retained_frame.animated == uncached.animated,
                    "retained owner scene differed from full mutation/animation solve");
            require(sampled.root == uncached.root && sampled.animated == uncached.animated,
                    "compiled animation target/underlying/duration/order invalidation differs from full sampling");
            require(raster.raster(sampled).rgba == raster.raster(uncached).rgba,
                    "compiled animation mutation raster differs from full sampling");
        }
        require(planned.work().animation_reuses > 100 &&
                    planned.work().animation_compilations < unplanned.work().animation_compilations,
                "compiled animation plans did not reuse unchanged targets");
        vm.eval("n.style.animation='missing 1s linear infinite'");
        for (unsigned i = 0; i < 2; ++i) {
            bool rejected = false;
            try {
                planned.frame(vm.snapshot_subtree("#root"), weft::test_constants::scene::contract_view_size,
                              weft::test_constants::scene::contract_view_size, 4100 + i);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            require(rejected, "failed animation compilation reused a stale valid plan");
        }
        vm.eval("n.style.animation='shade 1s linear infinite'");
        require(planned
                    .frame(vm.snapshot_subtree("#root"), weft::test_constants::scene::contract_view_size,
                           weft::test_constants::scene::contract_view_size, 4200)
                    .animated,
                "valid animation did not recover after compile failure");
        planned.reset();
        require(planned.work().animation_reuses == 0 && planned.work().animation_compilations == 0,
                "animation plan/reset statistics survived epoch reset");
        vm.reset_document();
        weft::install_script_platform(vm, weft::ScriptPlatformModule::WebHost);
        vm.eval(
            "const root=document.getElementById('root');root.style.position='relative';root.style.backgroundColor='#000000';let p=document.createElement('div');p.setAttribute('style','width:30px;height:24px;color:#225577;transform:scale(1);transition-property:color;transition-duration:200ms;transition-timing-function:linear');let n=document.createElement('div');n.setAttribute('style','position:absolute;inset:0;background:#884422;border-radius:4px');p.appendChild(n);root.appendChild(p)");
        weft::SceneSolver layout_retained("", "", weft::testing::thread_scene_text());
        weft::SceneSolver layout_reference("", "", weft::testing::thread_scene_text(), false);
        weft::SceneSolver layout_updated("", "", weft::testing::thread_scene_text());
        weft::SceneFrame layout_output;
        std::optional<weft::SceneFrame> independent;
        for (unsigned i = 0; i < 64; ++i) {
            if (i % 7 == 1) {
                vm.eval("p.style.transform='translateX(" + std::to_string(i % 13) + "px)'");
            }
            if (i % 11 == 3) {
                vm.eval("p.style.transform='none'");
            }
            if (i % 11 == 4) {
                vm.eval("p.style.transform='scale(1)'");
            }
            if (i % 8 == 2) {
                vm.eval("p.style.color='" + std::string(i % 16 < 8 ? "#ff0000" : "#00ff00") + "';n.style.background='" +
                        std::string(i % 16 < 8 ? "#335599" : "#dd8844") + "'");
            }
            if (i % 9 == 5) {
                vm.eval("p.style.width='" + std::to_string(30 + i % 19) + "px';n.style.borderRadius='" +
                        std::to_string(i % 9) + "px'");
            }
            const auto dom = vm.snapshot_subtree("#root");
            const float width = i < 48 ? static_cast<float>(weft::test_constants::scene::contract_view_size) : 90.f;
            auto actual = layout_retained.frame(dom, width, weft::test_constants::scene::contract_view_size, i * 17);
            auto expected = layout_reference.frame(dom, width, weft::test_constants::scene::contract_view_size, i * 17);
            layout_updated.update(dom, layout_output, width, weft::test_constants::scene::contract_view_size, i * 17);
            require(layout_output.root == expected.root &&
                        weft::scene_paint_source_equal(layout_output.root, actual.root),
                    "in-place target/transition/layout differed from owning full frame");
            require(actual.root == expected.root && actual.elements.size() == expected.elements.size(),
                    "unchanged layout reference became stale after paint, containing-block or viewport mutation");
            for (std::size_t j = 0; j < actual.elements.size(); ++j) {
                const auto& x = actual.elements[j];
                const auto& y = expected.elements[j];
                require(x.identity == y.identity && x.rect == y.rect && x.client_rects == y.client_rects &&
                            x.client_width == y.client_width && x.client_height == y.client_height &&
                            x.scroll_width == y.scroll_width && x.scroll_height == y.scroll_height,
                        "retained layout element geometry differs from full layout");
            }
            require(raster.raster(actual).rgba == raster.raster(expected).rgba,
                    "preexpanded shorthand/transition inheritance differs from full reference");
            if (i == 0) {
                independent = actual;
                actual.root.children.clear();
            }
        }
        require(independent->root.children.size() == 1 && independent->root.children[0].box[2] == 30,
                "returned scene ownership changed during retained layout reuse");
        require(layout_retained.work().layout_reuses > 20, "layout reference reuse counterexample did not reuse");
        vm.reset_document();
        vm.collect_garbage();
        require(vm.stats().live_nodes == weft::test_constants::scene::empty_document_live_nodes,
                "random mutation/reset leaked detached nodes");
        weft::SceneFrame previous;
        previous.root.identity = {1, 1, 1};
        previous.root.box = {0, 0, weft::test_constants::scene::contract_view_size,
                             weft::test_constants::scene::contract_view_size};
        previous.root.space = weft::constants::xhtml_namespace_uri;
        auto child = previous.root;
        child.identity.slot = 2;
        child.box = {10, 10, 20, 20};
        child.style = {{"background-color", "#ffffff"}, {"opacity", ".500000"}};
        child.paint = weft::compile_scene_paint_values(child.style);
        previous.root.children.push_back(child);
        previous.elements = geometry.geometry(previous);
        auto changed = previous;
        changed.root.children.edit(0).paint->values[weft::constants::scene::paint_slot_opacity]->scalar.absolute =
            .4999999;
        require(previous.root == changed.root && !weft::scene_paint_source_equal(previous.root, changed.root),
                "rounded diagnostic spelling hid a different numeric source");
        const auto damaged = weft::scene_damage(previous, changed, 2);
        require(damaged.changed && !damaged.full && damaged.rect[0] <= 10 && damaged.rect[2] >= 30,
                "numeric paint mutation did not invalidate its retained surface");
        GallerySceneSession fixed(false, 0, weft::test_constants::scene::contract_view_size);
        const auto copies = fixed.snapshots;
        fixed.step(1);
        require(fixed.snapshots == copies && fixed.snapshot_reuses > 0, "idle scene copied unchanged DOM");
        fixed.width = 90;
        fixed.step(2);
        require(fixed.current.root.box[2] == 90 && fixed.snapshots == copies,
                "viewport resize reused stale static scene or copied unchanged DOM");
        const auto geometry_builds = fixed.profile.geometry_builds;
        fixed.vm.eval("document.getElementById('root').style.backgroundColor='#aabbcc'");
        fixed.step(3);
        require(fixed.profile.geometry_builds == geometry_builds, "paint-only style forced geometry solve");
        fixed.vm.eval("document.getElementById('root').style.transform='translateX(2px)'");
        fixed.step(4);
        require(fixed.profile.geometry_builds > geometry_builds, "transform failed to update geometry");
        fixed.set_profiling(false);
        const auto times = fixed.scene.work();
        fixed.vm.eval("document.getElementById('root').style.backgroundColor='#334455'");
        fixed.step(5);
        const auto after_times = fixed.scene.work();
        require(times.style_ms == after_times.style_ms && times.build_ms == after_times.build_ms &&
                    times.layout_ms == after_times.layout_ms,
                "disabled profiler still collected phase clocks");
        fixed.unmount();
        retained_frames_differential(raster);
        geometry_cache_differential();
        std::cout
            << "PASS scene contracts: timer lifecycle, selector diagnostics, cascade inheritance, timeline, transform hit/pixels, detach/reinsert, owner isolation, implicit endpoints, 120 mutation raster/layout differentials\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
