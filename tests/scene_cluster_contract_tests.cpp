#define NOMINMAX
#include "../probes/gallery_cluster_input.hpp"
#include "support/check.hpp"
#include "support/host_time.hpp"
#include "scene_test_image.hpp"
#include "cluster_visual_compare.hpp"
#include "caseelephant/scene_native_probe.hpp"
#include "test_constants/scene.hpp"
#include <thread>
#include <cstring>

using weft::testing::format_host_time;
using weft::testing::require;

int main() {
    try {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        struct ComLifetime {
            ~ComLifetime() {
                CoUninitialize();
            }
        } com;

        GallerySceneSession s("ICON_TESTS", 0, 788, 288, "cluster-utilities.css");
        s.vm.eval("__ceGallery.unmount()");
        s.pump();
        {
            weft::SceneFrame frame;
            auto& n = frame.root;
            n.identity = {1, 1, 99};
            n.tag = "div";
            n.text = "🔥";
            n.box = {0, 0, 40, 40};
            n.style = {
                {"font-size", "24px"}, {"color", "#d4a843"}, {"transform", "translateY(20px)"}, {"opacity", "0.2"}};
            n.attributes["style"] = "transform:translateY(20px);opacity:0.2";
            auto source = [&] {
                auto plan = weft::compile_native_scene(frame);
                require(plan.fallback.empty() && plan.paints.size() == 1 &&
                            plan.paints[0].kind == weft::NativePaint::Text,
                        "Unicode source primitive");
                return plan.paints[0];
            };
            const auto first = source();
            n.style["transform"] = "translateY(3px)";
            n.style["opacity"] = "0.8";
            n.attributes["style"] = "transform:translateY(3px);opacity:0.8";
            const auto moved = source();
            require(weft::scene_paint_source_equal(*first.text, *moved.text) && first.matrix != moved.matrix &&
                        first.opacity != moved.opacity,
                    "Unicode motion must reuse source while changing composite");
            n.style["color"] = "#ff0000";
            require(!weft::scene_paint_source_equal(*first.text, *source().text),
                    "Unicode color must invalidate source");
            n.style["color"] = "#d4a843";
            n.style["font-size"] = "28px";
            require(!weft::scene_paint_source_equal(*first.text, *source().text),
                    "Unicode font must invalidate source");
            n.style["font-size"] = "24px";
            n.text = "⭐";
            require(!weft::scene_paint_source_equal(*first.text, *source().text),
                    "Unicode content must invalidate source");
        }
        {
            weft::ScriptDom cached;
            cached.eval(
                "globalThis.box=document.createElement('div');box.id='cachebox';box.setAttribute('data-width','10');document.appendChild(box)");
            std::uint64_t epoch = 1;
            unsigned reads{};
            unsigned styles{};
            double external{};
            auto width = [](const auto& document) {
                auto find = [&](auto&& self, const auto& n) -> double {
                    if (n.attributes.contains("id") && n.attributes.at("id") == "cachebox") {
                        return std::stod(n.attributes.at("data-width"));
                    }
                    for (const auto& child : n.children) {
                        if (auto value = self(self, child); value >= 0) {
                            return value;
                        }
                    }
                    return -1;
                };
                return find(find, document);
            };
            cached.install_layout_cache_host([&] {
                return epoch;
            });
            cached.install_layout_host([&](const auto& document) {
                ++reads;
                auto find = [&](auto&& self, const auto& n) -> weft::NodeIdentity {
                    if (n.attributes.contains("id") && n.attributes.at("id") == "cachebox") {
                        return n.identity;
                    }
                    for (const auto& child : n.children) {
                        if (auto id = self(self, child); id.generation) {
                            return id;
                        }
                    }
                    return {};
                };
                auto id = find(find, document);
                std::vector<weft::ElementGeometry> result;
                if (id.generation) {
                    weft::ElementGeometry g;
                    g.identity = id;
                    g.rect = {0, 0, width(document) + external, 10};
                    g.client_width = g.scroll_width = g.rect[2];
                    g.client_height = g.scroll_height = 10;
                    g.offset = g.rect;
                    g.client_rects = {g.rect};
                    result.push_back(g);
                }
                return result;
            });
            cached.install_computed_style_host([&](const auto& document, auto) {
                ++styles;
                return std::map<std::string, std::string>{
                    {"width", format_host_time(width(document) + external) + "px"}};
            });
            require(cached.eval("box.getBoundingClientRect().width+box.clientWidth+box.offsetWidth") == "30" &&
                        reads == 1,
                    "geometry cache unchanged reads");
            require(cached.eval("box.__ceComputedStyle().width+','+box.__ceComputedStyle().width") == "10px,10px" &&
                        styles == 1,
                    "computed cache unchanged reads");
            require(cached.eval("box.setAttribute('data-width','20');box.getBoundingClientRect().width") == "20" &&
                        reads == 2,
                    "geometry cache ignored DOM mutation");
            require(cached.eval("box.__ceComputedStyle().width") == "20px" && styles == 2,
                    "style cache ignored DOM mutation");
            external = 5;
            ++epoch;
            require(cached.eval("box.getBoundingClientRect().width") == "25" &&
                        cached.eval("box.__ceComputedStyle().width") == "25px",
                    "cache ignored external owner epoch");
            for (unsigned n = 0; n < 128; ++n) {
                auto w = 3 + (n * 73) % 181;
                auto text = std::to_string(w);
                require(cached.eval("box.setAttribute('data-width','" + text +
                                    "');box.getBoundingClientRect().width") == std::to_string(w + 5),
                        "synchronous mutation/read stale geometry");
            }
            cached.eval("document.removeChild(box)");
            require(cached.eval("box.getBoundingClientRect().width") == "0", "detached geometry survived cache");
            cached.eval("box=null");
            cached.collect_garbage();
            require(cached.stats().live_nodes == weft::test_constants::scene::empty_document_live_nodes,
                    "owned layout cache retained native nodes");
            cached.reset_document();
            cached.collect_garbage();
            require(cached.stats().live_nodes == weft::test_constants::scene::empty_document_live_nodes,
                    "layout cache reset epoch leak");
        }
        require(
            s.vm.eval(
                R"JS((()=>{const n=document.createElement('button'),svg=document.createElementNS('http://www.w3.org/2000/svg','svg');return n.constructor!==Object&&n instanceof Element&&n instanceof EventTarget&&n instanceof HTMLElement&&n instanceof HTMLButtonElement&&!('ownerSVGElement' in n)&&svg instanceof SVGElement&&'ownerSVGElement' in svg&&'opacity' in n.style&&'backgroundColor' in n.style&&!('translateX' in n.style)})())JS") ==
                "true",
            "library brand/property detection");
        require(
            s.vm.eval(
                R"JS((()=>{const root=document.getElementById('root');root.innerHTML='<div id="fragment">A &amp; <b>B</b></div>';const before=root.innerHTML;let rejected=0;for(const value of ['bad<','<script>x</script>','<div><span></div>','<div onclick="x">bad</div>'])try{root.innerHTML=value}catch(e){++rejected}if(root.innerHTML!==before)throw Error('non-atomic fragment');document.getElementById('fragment').children[0].textContent='C';return rejected===4&&root.innerHTML.includes('<b>C</b>')&&root.innerHTML.includes('A &amp;')})())JS") ==
                "true",
            "bounded fragment atomicity/live serialization");
        require(
            s.vm.eval(
                R"JS((()=>{const n=document.getElementById('fragment');n.classList.add('a','b','a');let rejected=false;try{n.classList.add('c','bad token')}catch(e){rejected=true}return rejected&&n.className==='a b'&&!n.classList.contains('c')&&n.classList.toggle('a')===false&&n.classList.value==='b'&&n.classList.toggle('a',true)===true})())JS") ==
                "true",
            "class token reflection/atomic validation");
        require(
            s.vm.eval(
                R"JS((()=>{const n=document.getElementById('fragment'),controller=new AbortController();let calls=[];const handler={handleEvent(e){if(this!==handler)throw Error('wrong receiver');calls.push('one')}};n.addEventListener('check',handler);n.dispatchEvent(new Event('check'));handler.handleEvent=()=>calls.push('two');n.dispatchEvent(new Event('check'));n.removeEventListener('check',handler);n.addEventListener('check',()=>calls.push('aborted'),{signal:controller.signal});controller.abort();n.dispatchEvent(new Event('check'));let passive=new Event('passive',{cancelable:true});n.addEventListener('passive',e=>e.preventDefault(),{passive:true});n.dispatchEvent(passive);return calls.join(',')==='one,two'&&!passive.defaultPrevented})())JS") ==
                "true",
            "live handleEvent/abort/passive");
        s.vm.eval(
            "globalThis.events=[];globalThis.eventController=new AbortController();window.addEventListener('pointerdown',e=>events.push('capture:'+e.eventPhase+':'+(e instanceof PointerEvent)),{capture:true,signal:eventController.signal});window.addEventListener('pointerdown',e=>events.push('bubble:'+e.eventPhase),{signal:eventController.signal})");
        weft::MouseEvent event;
        event.type = weft::MouseType::PointerDown;
        s.vm.dispatch_mouse(s.vm.node_identity("#fragment"), event);
        require(s.vm.eval("events.join(',')") == "capture:1:true,bubble:3", "window pointer capture/brand/bubble");
        s.vm.eval("eventController.abort();events=[]");
        s.vm.dispatch_mouse(s.vm.node_identity("#fragment"), event);
        require(s.vm.eval("events.length") == "0", "window abort listeners survived");
        s.vm.eval(
            "document.getElementById('root').innerHTML='<div id=\"box\" style=\"position:relative;width:100px;height:50px;padding:4px;border:2px solid #ffffff\"></div>';globalThis.observations=[];globalThis.observer=new ResizeObserver(es=>observations.push(es.map(e=>[e.contentRect.width,e.contentRect.height,e.borderBoxSize[0].inlineSize])));observer.observe(document.getElementById('box'))");
        s.step(1);
        require(s.vm.eval("JSON.stringify(observations)") == "[[[88,38,100]]]", "observer initial box");
        s.step(2);
        require(s.vm.eval("observations.length") == "1", "observer repeated unchanged box");
        s.vm.eval("document.getElementById('box').style.width='120px'");
        s.step(3);
        require(s.vm.eval("JSON.stringify(observations[1])") == "[[108,38,120]]", "observer fresh mutation box");
        s.vm.eval("observer.disconnect();document.getElementById('box').style.width='140px'");
        s.step(4);
        require(s.vm.eval("observations.length") == "2", "observer disconnect");
        require(s.vm.eval("(1234567).toLocaleString()==='1,234,567'&&(-1234).toLocaleString()==='-1,234'") == "true",
                "host integer locale formatting");
        require(
            s.vm.eval(
                "(()=>{let errors=0;try{(1.25).toLocaleString()}catch(e){++errors}try{matchMedia('(min-width: 10px)')}catch(e){++errors}return errors===2&&matchMedia('(pointer:fine)').matches&&!matchMedia('(prefers-reduced-motion: reduce)').matches})()") ==
                "true",
            "unsupported platform profile visible");
        s.vm.eval(
            "document.getElementById('box').style.transform='translateX(25px)';globalThis.geometry=[document.getElementById('box').offsetLeft,document.getElementById('box').getBoundingClientRect().x];document.getElementById('box').style.width='160px';globalThis.freshWidth=document.getElementById('box').offsetWidth");
        require(s.vm.eval("JSON.stringify(geometry)") == "[0,25]" && s.vm.eval("freshWidth") == "160",
                "offset excludes transforms / synchronous fresh geometry");
        s.vm.eval(
            "document.getElementById('box').style.cssText='width:100px;height:20px;overflow:auto';document.getElementById('box').innerHTML='<div style=\"width:100px;height:40px\"></div>'");
        bool scroll_rejected = false;
        try {
            s.flush_scene();
        } catch (const std::invalid_argument& e) {
            scroll_rejected = std::string(e.what()).find("scene scrolling") != std::string::npos;
        }
        require(scroll_rejected, "overflow auto silently omitted scrolling");
        s.vm.eval("document.getElementById('box').innerHTML='<div style=\"width:80px;height:10px\"></div>'");
        s.flush_scene();
        s.vm.eval(
            "document.getElementById('box').style.cssText='width:100px;height:20px;min-width:50%';document.getElementById('box').textContent=''");
        s.flush_scene();
        require(s.vm.eval("document.getElementById('box').offsetWidth") == "394",
                "percentage min-width uses containing width");
        // Same pinned, unmodified Anime package, with an independently valid
        // axis transform. The original throw case also remains unchanged/compared.
        s.vm.eval(scene_read(WEFT_SOURCE_DIR
                             "/third_party/gallery-runtime/source/animejs-4.3.6-3c7dc1c4/dist/bundles/anime.umd.js"));
        s.vm.eval(
            "document.getElementById('root').innerHTML='<div id=\"arena\" style=\"position:relative;width:500px;height:240px;overflow:hidden\"><div id=\"ball\" style=\"position:absolute;left:200px;top:100px;width:40px;height:40px;background:#ff0000;transform:translateX(0px) translateY(0px)\"></div></div>';globalThis.drag=anime.createDraggable(document.getElementById('ball'),{container:document.getElementById('arena'),containerFriction:0.5,releaseEase:'outExpo',dragSpeed:0.5});globalThis.ballX=()=>document.getElementById('ball').getBoundingClientRect().x");
        s.step(5);
        const auto initial = std::stof(s.vm.eval("ballX()"));
        s.input_value(weft::MouseType::Down, 220, 120, 1);
        for (unsigned i = 1; i <= 5; ++i) {
            s.step(5 + i * weft::test_constants::scene::drag_frame_interval_ms);
            s.input_value(weft::MouseType::Move, 220 + i * 20.f, 120, 1);
        }
        s.step(105);
        const auto held = std::stof(s.vm.eval("ballX()"));
        s.input_value(weft::MouseType::Up, 320, 120);
        s.step(121.6666667);
        const auto release = std::stof(s.vm.eval("ballX()"));
        for (unsigned i = 1; i <= 90; ++i) {
            s.step(121.6666667 + i * weft::test_constants::scene::drag_frame_interval_ms);
        }
        const auto settled = std::stof(s.vm.eval("ballX()"));
        std::cout << "inertia positions " << initial << " / " << held << " / " << release << " / " << settled << '\n';
        require(held > initial + 10 && settled > release + 5 && settled <= 460.1f,
                "unmodified Anime held movement / release inertia / container bounds");
        s.vm.eval("drag.revert();globalThis.drag=null;globalThis.ballX=null");
        // Renderer must preserve actual shadow paint and reject it from the native
        // subset until that backend supports it. A white base makes missing shadow
        // visible, unlike the gallery's nearly black backdrop.
        s.vm.eval(
            "document.getElementById('root').innerHTML='<div id=\"shadow\" style=\"position:absolute;left:40px;top:40px;width:40px;height:40px;background:#ff0000;border-radius:5px;box-shadow:0px 8px 12px 0px rgba(0,0,0,0.8)\"></div>';document.getElementById('root').style.backgroundColor='#ffffff'");
        s.step(1700);
        const auto with = s.bitmap(weft::test_constants::scene::gallery_raster_scale);
        require(!weft::compile_native_scene(s.current).fallback.empty(), "shadow silently dropped by native subset");
        s.vm.eval("document.getElementById('shadow').style.boxShadow='none'");
        s.step(1701);
        const auto without = s.bitmap(weft::test_constants::scene::gallery_raster_scale);
        require(with.rgba != without.rgba, "shadow emitted no pixels");
        // Differential mutation/reordering/detachment with actual shaping/layout.
        s.vm.eval(
            "document.getElementById('root').textContent='';globalThis.nodes=[];for(let i=0;i<24;++i){let n=document.createElement('div');n.style.cssText='position:absolute;left:'+i*10+'px;top:40px;width:12px;height:12px;background:#d4a843';nodes.push(n);document.getElementById('root').appendChild(n)}let seed=123;globalThis.mutate=()=>{seed=(Math.imul(seed,1664525)+1013904223)>>>0;let n=nodes[seed%nodes.length];switch(seed%4){case 0:n.style.transform='translateX('+seed%13+'px)';break;case 1:n.style.backgroundColor=seed%2?'#225577':'#ff7777';break;case 2:if(n.parentNode)n.remove();else document.getElementById('root').appendChild(n);break;case 3:document.getElementById('root').appendChild(n)}}");
        weft::SceneSolver full(
            scene_read(WEFT_SOURCE_DIR "/examples/crpg-gallery/cluster-utilities.css"),
            scene_read(WEFT_SOURCE_DIR "/examples/crpg-gallery/cluster-keyframes.css"),
            [&](auto text, const auto& style) {
                return s.raster.measure(text, style);
            },
            false);
        weft::SceneRaster oracle;
        for (unsigned i = 0; i < weft::test_constants::scene::cluster_mutation_iterations; ++i) {
            s.vm.eval("mutate()");
            s.step(1702 + i);
            const auto fresh = full.frame(s.vm.snapshot_subtree("#root"), s.width, s.height, s.time);
            require(s.bitmap().rgba == oracle.raster(fresh, weft::test_constants::scene::gallery_raster_scale).rgba,
                    "incremental/full mutation pixel mismatch");
        }
        const auto sparse = scene_reference(std::filesystem::path(WEFT_SOURCE_DIR) /
                                            "tests/fixtures/gallery-cluster-reference/anime-05-24.png");
        auto blank = sparse;
        for (std::size_t i = 0; i < blank.rgba.size(); i += 4) {
            blank.rgba[i] = weft::test_constants::scene::backdrop_red;
            blank.rgba[i + 1] = weft::test_constants::scene::backdrop_green;
            blank.rgba[i + 2] = weft::test_constants::scene::backdrop_blue;
            blank.rgba[i + 3] = weft::constants::color_channel_max;
        }
        require(!cluster_image_match(blank, sparse), "sparse oracle accepted blank");
        auto displaced = blank;
        for (unsigned y = 0; y < sparse.height; ++y) {
            for (unsigned x = 8; x < sparse.width; ++x) {
                std::memcpy(displaced.rgba.data() + (std::size_t(y) * sparse.width + x) * 4,
                            sparse.rgba.data() + (std::size_t(y) * sparse.width + x - 8) * 4, 4);
            }
        }
        require(!cluster_image_match(displaced, sparse), "sparse oracle accepted displaced particles");
        s.unmount();
        require(s.vm.stats().live_nodes == weft::test_constants::scene::empty_document_live_nodes,
                "shared platform owner teardown");
        std::cout << "PASS shared cluster platform / 64 full-path mutation comparisons / oracle negatives\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
