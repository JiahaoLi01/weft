#define NOMINMAX
#include "bridge/bridge_value.hpp"
#include "view/view.hpp"
#include "support/check.hpp"
#include "support/files.hpp"
#include "support/scene_fonts.hpp"
#include "../probes/gallery_scene_session.hpp"

#include <atomic>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// The public View (src/view, docs/engine-communication.md section 8): a bridge-driven HUD page loads, takes engine
// events, models and anchors, answers pointer input and hit queries, records the page's errors, and publishes frames
// other threads read; the same page makes the same scene as the probes' gallery session.
namespace {
    namespace bridge = weft::bridge;
    using weft::PointerKind;
    using weft::ViewDiagnostic;
    using weft::testing::require;

    std::string source(const std::string& relative) {
        return weft::testing::read_file(std::string(WEFT_SOURCE_DIR) + "/" + relative);
    }

    weft::ViewPage hud_page() {
        weft::ViewPage page;
        page.scripts.push_back({"hud.js", source("examples/engine-host/hud.js")});
        page.stylesheet = source("examples/engine-host/hud.css");
        page.keyframes = source("examples/engine-host/hud-keyframes.css");
        return page;
    }

    weft::ViewPage inline_page(std::string script, std::string stylesheet = {}) {
        weft::ViewPage page;
        page.scripts.push_back({"inline.js", std::move(script)});
        page.stylesheet = "#root{position:relative;width:100%;height:100%}" + stylesheet;
        return page;
    }

    struct Fixture {
        std::shared_ptr<const weft::FontCatalog> fonts =
            std::make_shared<weft::FontCatalog>(weft::testing::chrome_windows_font_catalog());
        weft::SceneText text{*fonts};
        weft::View view{fonts, text, 1280, 720, 1};

        bridge::EngineChannel& engine() {
            return *view.shared()->engine();
        }
    };

    std::string json(const std::vector<std::uint8_t>& value) {
        return bridge::value_to_json(value);
    }

    bool has_class(const weft::SceneNode& n, std::string_view cls) {
        const auto it = n.attributes.find("class");
        return it != n.attributes.end() && it->second == cls;
    }

    void collect(const weft::SceneNode& n, std::string_view cls, std::vector<const weft::SceneNode*>& out) {
        if (has_class(n, cls)) {
            out.push_back(&n);
        }
        for (const auto& c : n.children) {
            collect(c, cls, out);
        }
    }

    std::vector<const weft::SceneNode*> nodes(const weft::View& view, std::string_view cls) {
        std::vector<const weft::SceneNode*> out;
        collect(view.scene().root, cls, out);
        return out;
    }

    // The text of a node and its descendants, in order (an element of only text holds it itself).
    std::string text_of(const weft::SceneNode& n) {
        std::string out = n.text;
        for (const auto& c : n.children) {
            out += text_of(c);
        }
        return out;
    }

    std::size_t count(const weft::SceneNode& n) {
        std::size_t total = 1;
        for (const auto& c : n.children) {
            total += count(c);
        }
        return total;
    }

    std::string diagnostics_text(weft::View& view, ViewDiagnostic::Kind kind) {
        std::string out;
        for (const auto& d : view.take_diagnostics()) {
            if (d.kind == kind) {
                out += d.message + "\n";
            }
        }
        return out;
    }

    void loads() {
        Fixture f;
        f.view.install_function("loc.get", [](std::span<const std::uint8_t> argument) {
            bridge::ValueWriter out;
            out.string(bridge::value_to_json({argument.begin(), argument.end()}) == R"({"key":"hud.title"})" ? "Battle"
                                                                                                             : "?");
            return out.take();
        });
        require(!f.view.shared()->frame(), "no frame before a page");
        f.view.load(hud_page());
        require(f.view.loaded() && !f.view.faulted(), "loaded");
        const auto messages = f.view.take_messages();
        require(messages.size() == 1 && messages[0].name == "hud.ready" &&
                    json(messages[0].value) == R"({"title":"Battle"})",
                "the page's first message, with a sync function's answer");
        const auto frame = f.view.shared()->frame();
        require(frame && frame->scene && frame->width == 1280 && frame->height == 720 && frame->scale == 1 &&
                    frame->fonts == f.fonts && frame->anchors == f.view.shared()->anchors(),
                "the first frame");
        require(frame->scene->root.box == std::array<float, 4>{0, 0, 1280, 720}, "the root fills the View");
        require(nodes(f.view, "hud-title").size() == 1 && text_of(*nodes(f.view, "hud-title")[0]) == "Battle",
                "the title");
        // Functions stay across loads.
        f.view.load(hud_page());
        require(json(f.view.take_messages().at(0).value) == R"({"title":"Battle"})", "a function after a load");
    }

    // A frame's engine input sees the frame's time, as its tasks and animation callbacks do (an animation a handler
    // starts begins at the frame, not at the one before).
    void frame_time() {
        Fixture f;
        f.view.load(inline_page(R"JS(
            globalThis.seen = [];
            weft.engine.model('m').subscribe(() => { seen.push('model ' + performance.now()); });
            weft.engine.on('tick', () => {
              seen.push('event ' + performance.now());
              requestAnimationFrame(time => { seen.push('frame ' + time); });
            });
          )JS"));
        f.view.advance(10);
        f.engine().set_model_json("m", R"({"a":1})");
        f.engine().emit_json("tick", "{}");
        f.view.advance(26.5);
        const auto seen = f.view.script().eval("seen.join(', ')");
        require(seen == "model 26.5, event 26.5, frame 26.5", "the engine's input of a frame reads its time: " + seen);
    }

    void events_and_models() {
        Fixture f;
        f.view.load(hud_page());
        f.engine().emit_json("hud.spawn", R"({"id":1,"name":"Goblin"})");
        f.engine().emit_json("hud.spawn", R"({"id":2,"name":"Orc"})");
        f.engine().set_model_json("health", R"({"1":0.5})");
        f.view.advance(16);
        auto units = nodes(f.view, "hud-unit");
        require(units.size() == 2 && text_of(*units[0]) == "Goblin" && text_of(*units[1]) == "Orc", "spawned units");
        require(units[0]->anchoring && units[0]->anchoring->key == 1 && units[1]->anchoring->key == 2,
                "nameplates follow their units' anchors");
        auto fills = nodes(f.view, "hud-fill");
        require(fills.size() == 2 && fills[0]->style.text(weft::PropertyId::Transform) == "scaleX(0.5)" &&
                    fills[1]->style.text(weft::PropertyId::Transform).empty(),
                "health from the view model");
        // Damage numbers rise for a second.
        f.engine().emit_json("hud.damage", R"({"id":1,"amount":12})");
        f.view.advance(32);
        auto damage = nodes(f.view, "hud-damage");
        require(damage.size() == 1 && text_of(*damage[0]) == "12" && damage[0]->anchoring->key == 1 &&
                    !damage[0]->anchoring->scale,
                "a damage number");
        f.view.advance(500);
        require(nodes(f.view, "hud-damage").size() == 1 && f.view.scene().animated, "still rising");
        f.view.advance(1100);
        require(nodes(f.view, "hud-damage").empty(), "gone after a second");
        f.engine().emit_json("hud.despawn", R"({"id":1})");
        f.view.advance(1116);
        require(nodes(f.view, "hud-unit").size() == 1, "despawned");
        require(f.view.take_diagnostics().empty(), "no diagnostics");
    }

    void hits_and_pointer() {
        Fixture f;
        f.view.load(hud_page());
        f.view.take_messages();
        f.engine().emit_json("hud.spawn", R"({"id":1,"name":"Goblin"})");
        const weft::SceneAnchor anchor{1, 300, 400, 1, 0, 0, 0};
        require(f.view.shared()->write_anchors({&anchor, 1}) == weft::SceneAnchorWrite::Written, "anchors");
        f.view.advance(16);
        // The panel's button: right 24, bottom 24, 120 x 32 at the panel's end.
        auto hit = f.view.hit(1200, 680);
        require(hit.element.generation && hit.cursor == "pointer", "the button, a pointer cursor");
        hit = f.view.hit(1200, 620);
        require(hit.element.generation && hit.cursor == "auto", "the title, no cursor");
        hit = f.view.hit(300, 395);
        require(!hit.element.generation, "a nameplate takes no pointer events");
        hit = f.view.hit(10, 10);
        require(!hit.element.generation && hit.cursor == "auto", "the root is the page, not something on it");
        // A click calls the engine; its completion shows at the next advance.
        f.view.pointer({PointerKind::Move, 1200, 680});
        f.view.pointer({PointerKind::Down, 1200, 680, 1});
        f.view.pointer({PointerKind::Up, 1200, 680, 0});
        auto messages = f.view.take_messages();
        require(messages.size() == 1 && messages[0].call && messages[0].name == "hud.endTurn" &&
                    json(messages[0].value) == R"({"turn":1})",
                "the click's call");
        require(f.engine().complete_json(messages[0].call, true, R"({"turn":2})") == bridge::ChannelStatus::Ok,
                "complete");
        f.view.advance(32);
        require(text_of(*nodes(f.view, "hud-turn")[0]) == "Turn 2", "the call's answer");
        // Up somewhere else is no click, nor up on the button after a press elsewhere.
        f.view.pointer({PointerKind::Down, 1200, 680, 1});
        f.view.pointer({PointerKind::Up, 10, 10, 0});
        f.view.pointer({PointerKind::Down, 10, 10, 1});
        f.view.pointer({PointerKind::Up, 1200, 680, 0});
        require(f.view.take_messages().empty(), "no click off the button");

        // Anchored elements are hit where their anchors are.
        f.view.load(inline_page(R"JS(
            const pick = document.createElement('div');
            pick.className = 'pick';
            pick.setAttribute('data-anchor', '7');
            document.getElementById('root').appendChild(pick);
            const inner = document.createElement('div');
            inner.className = 'inner';
            pick.appendChild(inner);
            globalThis.where = () => { const r = pick.getBoundingClientRect(); return r.left + ',' + r.top; };
            pick.addEventListener('mouseenter', () => weft.engine.send('enter'));
            pick.addEventListener('mouseleave', () => weft.engine.send('leave'));
            pick.addEventListener('wheel', e => { weft.engine.send('wheel', {y: e.deltaY}); e.preventDefault(); });
          )JS",
                                ".pick{position:absolute;left:0;top:0;width:40px;height:40px;cursor:move}"
                                ".inner{position:absolute;left:10px;top:10px;width:20px;height:20px}"));
        weft::SceneAnchor at{7, 100, 100, 1, 0, 0, 0};
        f.view.shared()->write_anchors({&at, 1});
        require(f.view.hit(100, 100).cursor == "move" && !f.view.hit(300, 300).element.generation, "at its anchor");
        at.x = at.y = 300;
        f.view.shared()->write_anchors({&at, 1});
        require(f.view.hit(300, 300).cursor == "move" && !f.view.hit(100, 100).element.generation,
                "the latest anchors");
        require(f.view.hit(295, 295).element != f.view.hit(285, 285).element, "the inner element, then the pick");
        // The page's layout reads take the anchors the advance took.
        at.x = at.y = 500;
        f.view.shared()->write_anchors({&at, 1});
        f.view.advance(48);
        require(f.view.script().eval("where()") == "480,480", "JS reads where the anchor is");
        f.view.pointer({PointerKind::Move, 500, 500});
        const bool prevented = f.view.pointer({PointerKind::Wheel, 500, 500, 0, 0, 120});
        f.view.pointer({PointerKind::Leave, 0, 0});
        messages = f.view.take_messages();
        require(messages.size() == 3 && messages[0].name == "enter" && messages[1].name == "wheel" &&
                    json(messages[1].value) == R"({"y":120})" && messages[2].name == "leave" && prevented,
                "enter, a wheel whose default is prevented, leave");
    }

    // Hits between advances follow the page: the View keeps a hit map for the scene's revision and the anchors' serial
    // alone, and asks the whole scene about a point off the View.
    void hit_cache() {
        Fixture f;
        f.view.load(inline_page(R"JS(
            const box = document.createElement('div');
            box.className = 'box';
            document.getElementById('root').appendChild(box);
            globalThis.box = box;
            box.addEventListener('pointerdown', () => { box.style.top = '100px'; });
          )JS",
                                ".box{position:absolute;left:0;top:0;width:40px;height:40px;cursor:move}"));
        require(f.view.hit(20, 20).cursor == "move" && f.view.hit(220, 20).cursor == "auto", "the box");
        f.view.script().eval("box.style.left = '200px'");
        require(f.view.hit(220, 20).cursor == "move" && f.view.hit(20, 20).cursor == "auto",
                "the box where a script moved it, before an advance");
        f.view.pointer({PointerKind::Move, 220, 20});
        f.view.pointer({PointerKind::Down, 220, 20, 1});
        require(f.view.hit(220, 120).cursor == "move" && f.view.hit(220, 20).cursor == "auto",
                "the box where its handler moved it");
        f.view.pointer({PointerKind::Up, 220, 120, 0});
        f.view.script().eval("box.style.left = '-30px'; box.style.top = '0px'");
        require(f.view.hit(5, 5).cursor == "move" && f.view.hit(-10, 5).cursor == "move",
                "a point off the View hits what lies there");
    }

    void diagnostics() {
        Fixture f;
        bool threw = false;
        try {
            f.view.load(inline_page("throw new Error('broken page')"));
        } catch (const weft::ViewError& e) {
            threw = std::string_view(e.what()).find("broken page") != std::string_view::npos;
        }
        require(threw && !f.view.loaded(), "a page that throws leaves an empty View");
        require(diagnostics_text(f.view, ViewDiagnostic::Kind::Script).find("broken page") != std::string::npos,
                "its error a diagnostic");
        f.view.load(inline_page(R"JS(
            weft.engine.on('boom', () => { throw new Error('handler boom'); });
            requestAnimationFrame(() => { throw new Error('frame boom'); });
            setTimeout(() => { throw new Error('task boom'); }, 0);
            const box = document.createElement('div');
            box.className = 'box';
            document.getElementById('root').appendChild(box);
            globalThis.box = box;
          )JS",
                                ".box{position:absolute;width:10px;height:10px}"));
        f.engine().emit_json("boom", "{}");
        f.view.advance(16);
        const auto taken = f.view.take_diagnostics();
        const auto has = [&](ViewDiagnostic::Kind kind, std::string_view text) {
            return std::any_of(taken.begin(), taken.end(), [&](const ViewDiagnostic& d) {
                return d.kind == kind && d.message.find(text) != std::string::npos;
            });
        };
        require(has(ViewDiagnostic::Kind::Handler, "handler boom") && has(ViewDiagnostic::Kind::Frame, "frame boom") &&
                    has(ViewDiagnostic::Kind::Script, "task boom"),
                "handler, frame and task errors are diagnostics");
        // A scene the styles cannot make keeps the last frame.
        const auto before = f.view.shared()->frame();
        f.view.script().eval("box.setAttribute('data-anchor', 'not a key')");
        f.view.advance(32);
        require(f.view.shared()->frame() == before && !diagnostics_text(f.view, ViewDiagnostic::Kind::Scene).empty(),
                "a scene diagnostic keeps the frame");
        f.view.script().eval("box.removeAttribute('data-anchor')");
        f.view.advance(48);
        require(f.view.shared()->frame() != before, "the scene again");
    }

    void frames() {
        Fixture f;
        f.view.load(hud_page());
        const auto first = f.view.shared()->frame();
        f.view.advance(16);
        require(f.view.shared()->frame() == first, "an unchanged scene publishes nothing new");
        f.engine().emit_json("hud.spawn", R"({"id":1,"name":"Goblin"})");
        f.view.advance(32);
        const auto second = f.view.shared()->frame();
        require(second != first && second->scene->revision != first->scene->revision &&
                    count(second->scene->root) > count(first->scene->root),
                "a change publishes a new frame");
        const auto before_scale = f.view.shared()->frame();
        f.view.resize(1280, 720, 2);
        f.view.advance(40);
        require(f.view.shared()->frame() != before_scale && f.view.shared()->frame()->scale == 2 &&
                    f.view.shared()->frame()->scene->revision == before_scale->scene->revision,
                "a new scale publishes the same scene");
        f.view.resize(800, 600, 2);
        f.view.advance(48);
        const auto resized = f.view.shared()->frame();
        require(resized->width == 800 && resized->height == 600 && resized->scale == 2 &&
                    resized->scene->root.box == std::array<float, 4>{0, 0, 800, 600} &&
                    f.view.script().eval("innerWidth + 'x' + innerHeight + '@' + devicePixelRatio") == "800x600@2",
                "a resize");

        bool regressed = false;
        try {
            f.view.advance(47);
        } catch (const std::invalid_argument&) {
            regressed = true;
        }
        require(regressed, "a View's time does not go back");

        // Another thread takes frames while the owner changes the page every frame: each frame stays as published.
        std::atomic<bool> done{};
        std::atomic<std::size_t> reads{};
        std::string failure;
        std::thread reader([&] {
            while (!done) {
                const auto frame = f.view.shared()->frame();
                const auto revision = frame->scene->revision;
                const auto nodes_now = count(frame->scene->root);
                std::this_thread::yield();
                if (frame->scene->revision != revision || count(frame->scene->root) != nodes_now) {
                    failure = "a published frame changed";
                }
                ++reads;
            }
        });
        for (int i = 0; i < 200; ++i) {
            f.engine().emit_json(i % 2 ? "hud.despawn" : "hud.spawn",
                                 "{\"id\":" + std::to_string(10 + i / 2) + ",\"name\":\"U\"}");
            f.view.advance(64.0 + i * 16);
        }
        done = true;
        reader.join();
        require(failure.empty() && reads > 0, failure);
    }

    // ResizeObserver and window's resize event follow the View's size.
    void observers() {
        Fixture f;
        f.view.load(inline_page(R"JS(
            new ResizeObserver(entries => weft.engine.send('observed', entries[0].contentRect.width))
              .observe(document.getElementById('root'));
            window.addEventListener('resize', () => weft.engine.send('resized', innerWidth));
          )JS"));
        auto messages = f.view.take_messages();
        require(messages.size() == 1 && messages[0].name == "observed" && json(messages[0].value) == "1280",
                "observed as it loads");
        f.view.resize(800, 600, 1);
        f.view.advance(16);
        messages = f.view.take_messages();
        require(messages.size() == 2 && messages[0].name == "resized" && json(messages[0].value) == "800" &&
                    messages[1].name == "observed" && json(messages[1].value) == "800",
                "a resize, then its observation");
    }

    // A box whose content overflows scrolls: a wheel over it moves its scroll position within its content (a scroll
    // event follows), its rows move in the scene, and what is under a point follows them.
    void scrolling() {
        Fixture f;
        f.view.load(inline_page(R"JS(
            const box = document.createElement('div');
            box.id = 'box';
            box.setAttribute('style', 'position:absolute;left:0;top:0;width:100px;height:50px;overflow:auto');
            for (let i = 0; i < 5; ++i) {
              const row = document.createElement('div');
              row.id = 'row' + i;
              row.setAttribute('style', 'height:30px;cursor:pointer');
              box.appendChild(row);
            }
            document.getElementById('root').appendChild(box);
            box.addEventListener('scroll', () => weft.engine.send('scrolled', box.scrollTop));
          )JS"));
        f.view.advance(16);
        const auto row = [&](const char* id) {
            const weft::SceneNode* found{};
            const auto visit = [&](auto&& self, const weft::SceneNode& n) -> void {
                if (const auto it = n.attributes.find("id"); it != n.attributes.end() && it->second == id) {
                    found = &n;
                }
                for (const auto& c : n.children) {
                    self(self, c);
                }
            };
            visit(visit, f.view.scene().root);
            return found;
        };
        require(row("row0") && row("row0")->box[1] == 0, "unscrolled");
        f.view.pointer({PointerKind::Wheel, 50, 25, 0, 0, 40});
        f.view.advance(32);
        const auto messages = f.view.take_messages();
        require(messages.size() == 1 && messages[0].name == "scrolled" && json(messages[0].value) == "40",
                "a wheel scrolls the box, and a scroll event says so");
        require(row("row0")->box[1] == -40 && row("row2")->box[1] == 20, "the rows move by the scroll position");
        const auto under = f.view.hit(50, 25);
        require(row("row2") && under.element == row("row2")->identity, "the row under the point after the scroll");
        // Past the content's end the position stops there.
        f.view.pointer({PointerKind::Wheel, 50, 25, 0, 0, 500});
        f.view.advance(48);
        require(row("row0")->box[1] == -100, "clamped to the content");
    }

    // SVG text's geometry follows each font value it is measured with (scene_font): a text inside an svg, where no
    // layout runs, reads at once as a text made with its new font does.
    void svg_text_geometry() {
        Fixture f;
        f.view.load(inline_page(R"JS(
            const NS = 'http://www.w3.org/2000/svg';
            const svg = document.createElementNS(NS, 'svg');
            svg.setAttribute('width', '400');
            svg.setAttribute('height', '200');
            document.getElementById('root').appendChild(svg);
            const font = 'font-family:Segoe UI Light;font-size:28px;';
            const make = (y, style) => {
              const t = document.createElementNS(NS, 'text');
              t.setAttribute('x', '10');
              t.setAttribute('y', String(y));
              t.setAttribute('style', font + style);
              t.textContent = '1111';
              svg.appendChild(t);
              return t;
            };
            const size = t => { const r = t.getBoundingClientRect(); return r.width + 'x' + r.height; };
            // 'follows' when a text read before it takes value reads after as a text made with value does, a size
            // value changes; otherwise the three sizes.
            globalThis.follows = value => {
              const changed = make(50, '');
              const before = size(changed);
              changed.setAttribute('style', font + value);
              const after = size(changed);
              const made = make(150, value);
              const wanted = size(made);
              changed.remove();
              made.remove();
              return after === wanted && before !== wanted ? 'follows'
                                                           : before + ' then ' + after + ', made with it ' + wanted;
            };
          )JS"));
        for (const auto* value : {"font-size:40px", "letter-spacing:2px", "font-style:italic",
                                  "font-variant-numeric:tabular-nums", "line-height:60px"}) {
            const auto got = f.view.script().eval(std::string("follows('") + value + "')");
            require(got == "follows", std::string("svg text geometry after ") + value + ": " + got);
        }
    }

    // Equal scenes but for node identities, which each VM numbers by its own epoch.
    bool same_scene(const weft::SceneNode& a, const weft::SceneNode& b) {
        if (a.tag != b.tag || a.space != b.space || a.text != b.text || a.attributes != b.attributes ||
            !(a.style == b.style) || a.box != b.box || a.lines != b.lines || a.fragments != b.fragments ||
            !weft::scene_effect_equal(a.effect, b.effect) || a.particles != b.particles || a.anchoring != b.anchoring ||
            a.children.size() != b.children.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.children.size(); ++i) {
            if (!same_scene(a.children[i], b.children[i])) {
                return false;
            }
        }
        return true;
    }

    // The same page in the probes' gallery session and in a View makes the same scene.
    void like_the_session() {
        GallerySceneSession session("HUD_TESTS", 0, 1920, 1080,
                                    GallerySceneOptions{"hud-utilities.css", "HUD_TESTS.js"});
        session.set_profiling(false);
        auto fonts = std::make_shared<weft::FontCatalog>(weft::testing::chrome_windows_font_catalog());
        weft::View view(fonts, weft::testing::thread_scene_text(), 1920, 1080, 1);
        weft::ViewPage page;
        page.scripts = {
            {"platform.js", source("examples/crpg-gallery/native/platform.js")},
            {"random.js", "(()=>{let seed=0xce202610;Math.random=()=>((seed=(Math.imul(seed,1664525)+1013904223)>>>0)/"
                          "4294967296)})()"},
            {"HUD_TESTS.js", source("examples/crpg-gallery/native/HUD_TESTS.js")},
            {"setup.js",
             "document.getElementById('root').setAttribute('style','position:relative;overflow:hidden;background-"
             "color:#050403;color:#f5e6c8;font-family:system-ui;font-size:16px;line-height:1.5;');"
             "__ceGallery.mount('HUD_TESTS',0)"}};
        page.stylesheet = source("examples/crpg-gallery/hud-utilities.css");
        page.keyframes = source("examples/crpg-gallery/hud-keyframes.css");
        view.load(page);
        for (const auto* script : {"__ceHud.setCount(40)", "__ceHud.simulate(0.5, 3)"}) {
            session.vm.eval(script);
            view.script().eval(script);
        }
        session.step(16);
        view.advance(16);
        require(same_scene(session.current.root, view.scene().root) && count(view.scene().root) > 100,
                "the session's scene");
        for (const auto* script : {"__ceHud.anchor(true)", "__ceHud.health('[[0,0.25],[3,0.5]]', true)"}) {
            session.vm.eval(script);
            view.script().eval(script);
        }
        session.step(32);
        view.advance(32);
        require(same_scene(session.current.root, view.scene().root), "the session's anchored scene");
    }
} // namespace

int main() {
    try {
        // The gallery session's reference raster is Direct2D.
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
            throw std::runtime_error("COM");
        }
        loads();
        frame_time();
        events_and_models();
        hits_and_pointer();
        hit_cache();
        diagnostics();
        frames();
        observers();
        scrolling();
        svg_text_geometry();
        like_the_session();
        std::cout << "view tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "view tests failed: " << e.what() << '\n';
        return 1;
    }
}
