#include "base/diagnostic.hpp"
#include "scene/scene.hpp"
#include "scene/scene_geometry.hpp"
#include "support/scene_fonts.hpp"
#include "script/script_platform.hpp"
#include "support/check.hpp"
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

using weft::testing::require;

namespace {
    namespace dom = weft::dom;
    constexpr int element_type = weft::constants::element_node_type;

    // A scene DOM without a script engine: document > div, the scene root.
    struct NativeScene {
        dom::Tree tree{1};
        dom::Node* document{};
        dom::Node* root{};

        NativeScene() {
            document = &tree.create(weft::constants::document_node_type, "#document", "", {});
            root = &html("div");
            tree.insert(*document, *root, nullptr);
        }

        dom::Node& html(std::string local) {
            return tree.create(element_type, std::move(local), weft::constants::xhtml_namespace_uri,
                               document->identity);
        }

        dom::Node& svg(std::string local) {
            return tree.create(element_type, std::move(local), weft::constants::svg_namespace_uri, document->identity);
        }

        void set(dom::Node& n, const std::string& name, std::string value) {
            auto it = std::find_if(n.attributes.begin(), n.attributes.end(), [&](const dom::Attribute& a) {
                return a.name == name;
            });
            if (it == n.attributes.end()) {
                n.attributes.push_back({name, name, "", std::move(value)});
            } else {
                it->value = std::move(value);
            }
            tree.changed(n);
        }

        void toggle_class(dom::Node& n, const std::string& name) {
            auto it = std::find_if(n.attributes.begin(), n.attributes.end(), [](const dom::Attribute& a) {
                return a.name == "class";
            });
            if (it != n.attributes.end() && it->value == name) {
                n.attributes.erase(it);
                tree.changed(n);
            } else {
                set(n, "class", name);
            }
        }
    };

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
        const auto measure = [](auto text, const auto& style) {
            return weft::testing::thread_scene_text().measure(text, style);
        };
        weft::SceneSolver retained(sheet, "", measure);
        weft::SceneSolver owning(sheet, "", measure);
        weft::SceneSolver reference(sheet, "", measure, false);
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
            weft::SceneSolver first(sheet, "", measure);
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
            [&](auto t, const auto& s) {
                return weft::testing::thread_scene_text().measure(t, s);
            });
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
        weft::SceneSolver stacking("", "", [&](auto t, const auto& st) {
            return weft::testing::thread_scene_text().measure(t, st);
        });
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
        weft::SceneSolver rounding("", "", [&](auto t, const auto& st) {
            return weft::testing::thread_scene_text().measure(t, st);
        });
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
        vm.reset_document();
        weft::install_script_platform(vm, weft::ScriptPlatformModule::WebHost);
        vm.eval(
            "const r=document.getElementById('root');r.style.backgroundColor='#000000';const parent=document.createElement('div');parent.setAttribute('style','width:30px;height:30px;transform:scale(1)');const inner=document.createElement('div');inner.setAttribute('style','position:absolute;inset:0;background-color:#ffffff');parent.appendChild(inner);r.appendChild(parent)");
        weft::SceneSolver containing("", "", [&](auto t, const auto& st) {
            return weft::testing::thread_scene_text().measure(t, st);
        });
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
        weft::SceneSolver chase("", "@keyframes chase{75%{border-color:#ff0000}}", [&](auto t, const auto& st) {
            return weft::testing::thread_scene_text().measure(t, st);
        });
        chase.frame(vm.snapshot_subtree("#root"), weft::test_constants::scene::contract_view_size,
                    weft::test_constants::scene::contract_view_size, 0);
        auto implicit = chase.frame(vm.snapshot_subtree("#root"), weft::test_constants::scene::contract_view_size,
                                    weft::test_constants::scene::contract_view_size, 875);
        auto border = weft::parse_css_color(implicit.root.children[0].style.at("border-top-color"));
        require(std::abs(border[0] - .5f) < weft::test_constants::scene::paint_sample_tolerance &&
                    std::abs(border[2] - .5f) < weft::test_constants::scene::paint_sample_tolerance,
                "implicit border keyframe end did not use underlying currentcolor");
        vm.eval("n.setAttribute('style','width:20px;height:20px;animation:fade 1s linear infinite alternate-reverse')");
        weft::SceneSolver reverse("", "@keyframes fade{from{opacity:0}to{opacity:1}}", [&](auto t, const auto& st) {
            return weft::testing::thread_scene_text().measure(t, st);
        });
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
            weft::SceneSolver bad("", "@keyframes fade{from{shimmer:1}to{opacity:1}}", [&](auto t, const auto& st) {
                return weft::testing::thread_scene_text().measure(t, st);
            });
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
        weft::SceneSolver persisted(static_sheet, "", [&](auto t, const auto& st) {
            return weft::testing::thread_scene_text().measure(t, st);
        });
        vm.eval(
            "const root=document.getElementById('root');root.setAttribute('style','background-color:#000000;position:relative;overflow:hidden');globalThis.boxes=[];globalThis.parents=[];for(let i=0;i<2;i++){let p=document.createElement('div');p.setAttribute('style','position:absolute;left:'+i*40+'px;top:0px;width:40px;height:80px');root.appendChild(p);parents.push(p)}for(let i=0;i<24;i++){let b=document.createElement('div');b.setAttribute('class','shade');b.setAttribute('style','position:absolute;left:'+i%6*5+'px;top:'+Math.floor(i/6)*15+'px;width:8px;height:8px');parents[i%2].appendChild(b);boxes.push(b)}let seed=0x9e3779b9;globalThis.mutate=()=>{seed=(Math.imul(seed,1664525)+1013904223)>>>0;let b=boxes[seed%24],op=(seed>>>8)%4;if(op===0){b.style.transform='translateX('+((seed>>>16)%8)+'px)'}else if(op===1)b.setAttribute('class',(seed&1)?'shade':'other');else if(op===2){if(b.parentNode)b.parentNode.removeChild(b);else parents[seed%2].appendChild(b)}else parents[seed%2].appendChild(b)}");
        for (unsigned iteration = 0; iteration < weft::test_constants::scene::mutation_iterations; ++iteration) {
            vm.eval("mutate()");
            auto snapshot = vm.snapshot_subtree("#root");
            auto actual = persisted.frame(snapshot, weft::test_constants::scene::contract_view_size,
                                          weft::test_constants::scene::contract_view_size, iteration);
            weft::SceneSolver fresh(
                static_sheet, "",
                [&](auto t, const auto& st) {
                    return weft::testing::thread_scene_text().measure(t, st);
                },
                false);
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
        weft::SceneSolver retained("", keyframes, [&](auto t, const auto& st) {
            return weft::testing::thread_scene_text().measure(t, st);
        });
        weft::SceneSolver full(
            "", keyframes,
            [&](auto t, const auto& st) {
                return weft::testing::thread_scene_text().measure(t, st);
            },
            false);
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
        weft::SceneSolver keyed("rect[width='10']{fill:#00ff00}", "", [&](auto t, const auto& st) {
            return weft::testing::thread_scene_text().measure(t, st);
        });
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
        weft::SceneSolver planned("", tracks, [&](auto text, const auto& st) {
            return weft::testing::thread_scene_text().measure(text, st);
        });
        weft::SceneSolver unplanned(
            "", tracks,
            [&](auto text, const auto& st) {
                return weft::testing::thread_scene_text().measure(text, st);
            },
            false);
        weft::SceneSolver retained_solver("", tracks, [&](auto text, const auto& st) {
            return weft::testing::thread_scene_text().measure(text, st);
        });
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
        weft::SceneSolver layout_retained("", "", [&](auto text, const auto& st) {
            return weft::testing::thread_scene_text().measure(text, st);
        });
        weft::SceneSolver layout_reference(
            "", "",
            [&](auto text, const auto& st) {
                return weft::testing::thread_scene_text().measure(text, st);
            },
            false);
        weft::SceneSolver layout_updated("", "", [&](auto text, const auto& st) {
            return weft::testing::thread_scene_text().measure(text, st);
        });
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
        changed.root.children[0].paint->values[weft::constants::scene::paint_slot_opacity]->scalar.absolute = .4999999;
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
        std::cout
            << "PASS scene contracts: timer lifecycle, selector diagnostics, cascade inheritance, timeline, transform hit/pixels, detach/reinsert, owner isolation, implicit endpoints, 120 mutation raster/layout differentials\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
