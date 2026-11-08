#include "view/view.hpp"
#include "support/check.hpp"
#include "support/files.hpp"
#include "support/json_value.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Touch input (View::touch). The scenarios of tests/fixtures/touch run in a View and match, event by event, what Chrome
// with touch emulation did on the same page (tools/browser_touch_reference.mjs): taps and their compatibility mouse
// events, prevented touchstart and touchend, the slop, drags where touch-action or the page keeps the browser from
// panning, pans of scroll containers (the slop taken off), a fling's end, two touches, a long press, a double tap and a
// cancelled touch; the state after each step (scroll positions, :hover, :active) too. Weft leaves out what Chrome adds
// beyond the scene and its own throttling: enter and leave events at document, html and body (a View's scene starts at
// the page's root), scroll events (Chrome sends one a frame; the positions are compared), and the uncancelable
// touchmoves of a pan (Chrome sends them at its own pace). Weft's pan follows the finger: Chrome's resamples a fast
// pan's moves and predicts up to 8 ms ahead (its compositor's latency), so its content leads a finger at 2.5 px/ms by
// 20 px; the fling after it is compared by how far it goes. Then what one page cannot show: a touch that stops a fling,
// a fling that meets its container's end, and touches the View refuses. Portable: no system fonts (the page has no
// text; Roboto stands in for its family).
namespace {
    using weft::TouchChange;
    using weft::TouchPhase;
    using weft::testing::JsonValue;
    using weft::testing::parse_json;
    using weft::testing::require;

    std::shared_ptr<const weft::FontCatalog> catalog() {
        auto c = std::make_shared<weft::FontCatalog>();
        for (const char* family : {"Roboto", "Arial"}) {
            weft::FontFaceEntry face;
            face.family = family;
            face.source.path = WEFT_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf";
            c->faces.push_back(face);
        }
        c->generic_families = {{"sans-serif", "Roboto"}, {"system-ui", "Roboto"}};
        return c;
    }

    struct Fixture {
        std::shared_ptr<const weft::FontCatalog> fonts = catalog();
        weft::SceneText text{*fonts};
        weft::View view{fonts, text, 800, 600, 1};
        // The View's clock and the touches' (the reference's touch timestamps advance by the steps' gaps alone).
        double time{};
        double touch_time{};

        void load() {
            using weft::testing::read_text_file;
            using weft::testing::source_path;
            weft::ViewPage page;
            page.scripts.push_back({"touch.js", read_text_file(source_path("tests/fixtures/touch/touch.js"))});
            page.stylesheet = read_text_file(source_path("tests/fixtures/touch/touch.css"));
            page.forgiving_css = true;
            view.load(page);
            run(16);
        }

        std::string eval(const std::string& source) {
            const auto [ok, result] = view.evaluate(source);
            require(ok, "evaluates: " + source + " (" + result + ")");
            return result;
        }

        // Frames of 60 per second for ms.
        void run(double ms) {
            const double end = time + ms;
            while (time < end) {
                time = std::min(end, time + 1000.0 / 60);
                view.advance(time);
            }
        }

        bool touch(std::vector<TouchChange> points) {
            return view.touch({std::move(points), 0, touch_time});
        }
    };

    std::string dump(const JsonValue& v) {
        std::ostringstream out;
        switch (v.kind) {
            case JsonValue::Kind::Null:
                out << "null";
                break;
            case JsonValue::Kind::Bool:
                out << (v.boolean ? "true" : "false");
                break;
            case JsonValue::Kind::Number:
                out << v.number;
                break;
            case JsonValue::Kind::String:
                out << '"' << v.string << '"';
                break;
            case JsonValue::Kind::Array:
                out << '[';
                for (std::size_t i = 0; i < v.array.size(); ++i) {
                    out << (i ? "," : "") << dump(v.array[i]);
                }
                out << ']';
                break;
            case JsonValue::Kind::Object:
                out << '{';
                for (std::size_t i = 0; i < v.object.size(); ++i) {
                    out << (i ? "," : "") << v.object[i].first << ':' << dump(v.object[i].second);
                }
                out << '}';
                break;
        }
        return out.str();
    }

    // Chrome's value against weft's: positions within 1 px, arrays and objects member by member (Chrome's members).
    bool alike(const JsonValue& chrome, const JsonValue& weft, const std::string& key = {}) {
        if (chrome.kind != weft.kind) {
            return false;
        }
        switch (chrome.kind) {
            case JsonValue::Kind::Number:
                return (key == "x" || key == "y") ? std::abs(chrome.number - weft.number) <= 1
                                                  : chrome.number == weft.number;
            case JsonValue::Kind::Array:
                if (chrome.size() != weft.size()) {
                    return false;
                }
                for (std::size_t i = 0; i < chrome.size(); ++i) {
                    if (!alike(chrome[i], weft[i], key)) {
                        return false;
                    }
                }
                return true;
            case JsonValue::Kind::Object:
                for (const auto& [k, v] : chrome.object) {
                    const auto* mine = weft.find(k);
                    if (!mine || !alike(v, *mine, k)) {
                        return false;
                    }
                }
                return true;
            default:
                return chrome.boolean == weft.boolean && chrome.string == weft.string;
        }
    }

    // The events compared: enter and leave beyond the scene, scroll events, and once a pan runs its uncancelable
    // touchmoves, left out.
    std::vector<JsonValue> comparable(const JsonValue& events, bool& panning) {
        std::vector<JsonValue> out;
        for (const auto& e : events.array) {
            const auto& type = e["type"].string;
            const auto& target = e["target"].kind == JsonValue::Kind::String ? e["target"].string : std::string();
            if (type == "pointercancel" && e["x"].number == 0 && e["y"].number == 0) {
                panning = true;
            }
            if ((type.ends_with("enter") || type.ends_with("leave")) &&
                (target == "document" || target == "html" || target == "body")) {
                continue;
            }
            if (type == "scroll" || (panning && type == "touchmove" && !e["cancelable"].boolean)) {
                continue;
            }
            out.push_back(e);
        }
        return out;
    }

    void compare_events(const std::vector<JsonValue>& chrome, const std::vector<JsonValue>& weft,
                        const std::string& where) {
        std::string listing = "\n  chrome:";
        for (const auto& e : chrome) {
            listing += "\n    " + dump(e);
        }
        listing += "\n  weft:";
        for (const auto& e : weft) {
            listing += "\n    " + dump(e);
        }
        require(chrome.size() == weft.size(), where + ": " + std::to_string(weft.size()) + " events, Chrome " +
                                                  std::to_string(chrome.size()) + listing);
        for (std::size_t i = 0; i < chrome.size(); ++i) {
            for (const auto& [key, value] : chrome[i].object) {
                const auto* mine = weft[i].find(key);
                require(mine != nullptr, where + ": event " + std::to_string(i) + " has no " + key + listing);
                require(alike(value, *mine, key), where + ": event " + std::to_string(i) + " " + key + " is " +
                                                      dump(*mine) + ", Chrome " + dump(value) + listing);
            }
        }
    }

    // Scroll positions within tolerance px (a fling's, mid-flight, within a frame's travel); the rest alike.
    void compare_state(const JsonValue& chrome, const JsonValue& weft, const std::string& where, double tolerance) {
        for (const auto& [key, value] : chrome.object) {
            const auto* mine = weft.find(key);
            require(mine != nullptr, where + ": no state " + key);
            if (value.kind == JsonValue::Kind::Number) {
                require(std::abs(value.number - mine->number) <= tolerance,
                        where + ": " + key + " " + dump(*mine) + ", Chrome " + dump(value));
            } else {
                require(alike(value, *mine), where + ": " + key + " " + dump(*mine) + ", Chrome " + dump(value));
            }
        }
    }

    // Plays the steps of scenarios.json as a host gives them to a View: new touches at an element's place, moves by
    // offsets, releases and a cancel, each after its gap; waits as frames.
    struct Player {
        Fixture& f;
        float radius{};
        std::map<std::uint64_t, std::pair<float, float>> active;

        TouchChange change(std::uint64_t id, TouchPhase phase) {
            const auto [x, y] = active.at(id);
            return {id, phase, x, y, radius, radius, 0};
        }

        void play(const JsonValue& step) {
            if (const auto* wait = step.find("wait")) {
                f.run(wait->number);
                return;
            }
            const double dt = step.find("dt") ? step["dt"].number : 16;
            f.run(dt);
            f.touch_time += dt;
            std::vector<TouchChange> points;
            if (const auto* start = step.find("start")) {
                for (const auto& s : start->array) {
                    const auto box = parse_json(f.eval("JSON.stringify(__touch.box('" + s["at"].string + "'))"));
                    const float x = float(box[0].number + (s.find("dx") ? s["dx"].number : box[2].number / 2));
                    const float y = float(box[1].number + (s.find("dy") ? s["dy"].number : box[3].number / 2));
                    const auto id = std::uint64_t(s["id"].number);
                    active[id] = {x, y};
                    points.push_back(change(id, TouchPhase::Began));
                }
            } else if (const auto* move = step.find("move")) {
                for (const auto& m : move->array) {
                    const auto id = std::uint64_t(m["id"].number);
                    auto& [x, y] = active.at(id);
                    x += float(m["by"][0].number);
                    y += float(m["by"][1].number);
                    points.push_back(change(id, TouchPhase::Moved));
                }
            } else if (const auto* end = step.find("end")) {
                for (const auto& id : end->array) {
                    points.push_back(change(std::uint64_t(id.number), TouchPhase::Ended));
                    active.erase(std::uint64_t(id.number));
                }
            } else if (step.find("cancel")) {
                for (const auto& [id, at] : active) {
                    points.push_back(change(id, TouchPhase::Cancelled));
                }
                active.clear();
            } else {
                throw std::invalid_argument("unknown step " + dump(step));
            }
            f.touch(points);
        }
    };

    void chrome_scenarios() {
        using weft::testing::read_text_file;
        using weft::testing::source_path;
        const auto spec = parse_json(read_text_file(source_path("tests/fixtures/touch/scenarios.json")));
        const auto chrome = parse_json(read_text_file(source_path("tests/fixtures/touch/chrome-results.json")));
        std::size_t steps{};
        for (const auto& scenario : spec["scenarios"].array) {
            const auto& name = scenario["name"].string;
            const auto& results = chrome["scenarios"][name];
            Fixture f;
            f.load();
            f.eval("__touch.take()");
            Player player{f, float(spec["radius"].number)};
            bool chrome_panning{};
            bool weft_panning{};
            // Where the list was when the touch lifted (Chrome's, weft's): a fling's distance is counted from there.
            double chrome_released{};
            double weft_released{};
            for (std::size_t i = 0; i < scenario["steps"].size(); ++i) {
                const auto& step = scenario["steps"][i];
                player.play(step);
                // The reference reads the page 30 ms after a step.
                f.run(30);
                const auto events = parse_json(f.eval("JSON.stringify(__touch.take())"));
                const auto state = parse_json(f.eval("JSON.stringify(__touch.state())"));
                const auto where = name + " step " + std::to_string(i) + " " + dump(step);
                compare_events(comparable(results[i]["events"], chrome_panning), comparable(events, weft_panning),
                               where);
                const auto& chrome_state = results[i]["state"];
                if (name == "list_fling") {
                    // Chrome's fast pan leads the finger by its prediction; its fling, mid-flight, is read some ms
                    // later than a frame's; how far the fling went is alike.
                    if (step.find("move")) {
                        chrome_released = chrome_state["list"].number;
                        weft_released = state["list"].number;
                    }
                    if (step.find("wait")) {
                        const double chrome_flung = chrome_state["list"].number - chrome_released;
                        const double weft_flung = state["list"].number - weft_released;
                        require(chrome_flung > 300 && std::abs(chrome_flung - weft_flung) <= 2,
                                where + ": the fling went " + std::to_string(weft_flung) + " px, Chrome's " +
                                    std::to_string(chrome_flung));
                    }
                    auto mine = state;
                    for (auto& [key, value] : mine.object) {
                        if (key == "list") {
                            value = chrome_state["list"];
                        }
                    }
                    compare_state(chrome_state, mine, where, 0);
                } else {
                    compare_state(chrome_state, state, where, 1);
                }
                ++steps;
            }
        }
        std::cout << "touch scenarios: " << steps << " steps alike\n";
        require(steps > 80, "the scenarios ran");
    }

    // A touch on a flinging list stops the fling, and is no tap; a fling stops where its container ends.
    void flings_stop() {
        Fixture f;
        f.load();
        const auto fling = [&](float from_y, float step) {
            f.touch({{1, TouchPhase::Began, 550, from_y, 5, 5, 0}});
            for (int k = 1; k <= 4; ++k) {
                f.run(16);
                f.touch_time += 16;
                f.touch({{1, TouchPhase::Moved, 550, from_y - step * float(k), 5, 5, 0}});
            }
            f.run(16);
            f.touch_time += 16;
            f.touch({{1, TouchPhase::Ended, 550, from_y - step * 4, 5, 5, 0}});
        };
        fling(350, 40);
        f.run(100);
        const double during = std::stod(f.eval("__touch.state().list"));
        f.eval("__touch.take()");
        f.touch({{2, TouchPhase::Began, 550, 300, 5, 5, 0}});
        f.run(16);
        f.touch({{2, TouchPhase::Ended, 550, 300, 5, 5, 0}});
        f.run(500);
        const double after = std::stod(f.eval("__touch.state().list"));
        const auto log = f.eval("JSON.stringify(__touch.take().map(e => e.type))");
        require(during > 100 && std::abs(after - during) <= 1 + 3000 * 0.016,
                "the touch stopped the fling at " + std::to_string(during) + ", not " + std::to_string(after));
        require(log.find("click") == std::string::npos, "the touch that stopped a fling is no tap: " + log);
        // Fast flings down the list stop at its end (20 rows of 60 px in 240).
        for (int k = 0; k < 3; ++k) {
            fling(370, 60);
            f.run(2000);
        }
        require(std::stod(f.eval("__touch.state().list")) == 960, "the fling stops at the end");
    }

    // Pointer capture, against what Chrome did on the same page: the mouse captured in pointerdown (its events go to
    // the capturing element across the page; on its release lostpointercapture comes before the click, then the hover
    // moves to what is under it), and a touch whose implicit capture its pointerdown released (its pointer goes over
    // what it moves onto, its touch events stay at their target).
    void pointer_capture() {
        Fixture f;
        weft::ViewPage page;
        page.scripts.push_back({"capture.js", R"JS(
          const root = document.getElementById('root');
          const box = (id, left) => {
            const d = document.createElement('div');
            d.id = id;
            d.style.cssText = 'position:absolute;left:' + left + 'px;top:0;width:100px;height:100px';
            root.appendChild(d);
            return d;
          };
          const a = box('a', 0);
          box('b', 200);
          globalThis.log = [];
          const n = t => t === window ? 'window' : t === document ? 'document' : t.id || t.localName;
          for (const t of ['pointerover', 'pointerout', 'pointerenter', 'pointerleave', 'pointerdown', 'pointermove',
                           'pointerup', 'pointercancel', 'gotpointercapture', 'lostpointercapture', 'mouseover', 'mouseout',
                           'mousedown', 'mousemove', 'mouseup', 'click', 'touchstart', 'touchmove', 'touchend'])
            window.addEventListener(t, e => log.push(e.type + '@' + n(e.target) + (e.pointerType ? ' ' + e.pointerType : '')),
                                    {capture: true});
          globalThis.mode = '';
          a.addEventListener('pointerdown', e => {
            if (mode === 'capture') e.target.setPointerCapture(e.pointerId);
            if (mode === 'release') {
              e.target.releasePointerCapture(e.pointerId);
              log.push('has ' + e.target.hasPointerCapture(e.pointerId));
            }
          });
          globalThis.take = () => { const o = log.join(', '); log.length = 0; return o; };
        )JS"});
        page.stylesheet = "#root{position:absolute;left:0;top:0;width:600px;height:400px}";
        page.forgiving_css = true;
        f.view.load(page);
        f.run(16);
        const auto expect = [&](const std::string& what, const std::string& chrome) {
            const auto weft = f.eval("take()");
            require(weft == chrome, what + ":\n  weft:   " + weft + "\n  chrome: " + chrome);
        };
        using weft::PointerKind;
        f.eval("mode = 'capture'");
        f.view.pointer({PointerKind::Move, 50, 50, 0});
        f.eval("take()");
        f.view.pointer({PointerKind::Down, 50, 50, 1});
        expect("mouse down", "pointerdown@a mouse, mousedown@a");
        require(f.eval("a.hasPointerCapture(1)") == "true", "a has the mouse's capture");
        f.view.pointer({PointerKind::Move, 250, 50, 1});
        expect("mouse move to b", "gotpointercapture@a mouse, pointermove@a mouse, mousemove@a");
        f.view.pointer({PointerKind::Up, 250, 50, 0});
        expect("mouse up over b",
               "pointerup@a mouse, mouseup@a, lostpointercapture@a mouse, click@a mouse, "
               "pointerout@a mouse, pointerleave@a mouse, pointerover@b mouse, pointerenter@b mouse, "
               "mouseout@a, mouseover@b");
        f.view.pointer({PointerKind::Move, 260, 50, 0});
        expect("mouse move after", "pointermove@b mouse, mousemove@b");
        f.eval("mode = 'release'");
        f.touch({{1, TouchPhase::Began, 50, 50, 0, 0, 0}});
        expect("touch down", "pointerover@a touch, pointerenter@root touch, pointerenter@a touch, pointerdown@a touch, "
                             "has false, touchstart@a");
        f.touch({{1, TouchPhase::Moved, 60, 50, 0, 0, 0}});
        expect("touch move in a", "pointermove@a touch");
        f.touch({{1, TouchPhase::Moved, 250, 50, 0, 0, 0}});
        expect("touch move to b",
               "pointerout@a touch, pointerleave@a touch, pointerover@b touch, pointerenter@b touch, "
               "pointermove@b touch, touchmove@a, pointercancel@b touch, pointerout@b touch, "
               "pointerleave@b touch, pointerleave@root touch");
        f.touch({{1, TouchPhase::Ended, 250, 50, 0, 0, 0}});
        expect("touch up", "touchend@a");
        // A pointer that is not active: NotFoundError.
        require(f.eval("try { a.setPointerCapture(42); 'none' } catch (e) { e.name }") == "NotFoundError",
                "an inactive pointer's capture throws NotFoundError");
    }

    // The host's input devices as the page sees them, against what Chrome 156 answered on a desktop and with touch
    // emulation (matchMedia of pointer, any-pointer, hover and any-hover, navigator.maxTouchPoints, 'ontouchstart' in
    // window): @media rules that follow, change events of matchMedia lists, a touch laptop's mix, no pointer at all,
    // an ontouchstart handler that returns false, devices the View refuses, and the devices kept across loads.
    void input_devices() {
        Fixture f;
        weft::ViewPage page;
        page.scripts.push_back({"devices.js", R"JS(
          const probe = document.createElement('div');
          probe.id = 'probe';
          document.getElementById('root').appendChild(probe);
          globalThis.changes = [];
          matchMedia('(pointer: coarse)').addListener(e => changes.push('coarse ' + e.matches));
          matchMedia('(hover: none)').onchange = e => changes.push('no hover ' + e.matches);
          globalThis.answers = () => {
            const q = ['(pointer: fine)', '(pointer: coarse)', '(pointer: none)', '(pointer)', '(any-pointer: fine)',
                       '(any-pointer: coarse)', '(hover: hover)', '(hover: none)', '(hover)', '(any-hover: hover)',
                       '(any-hover: none)'];
            return [navigator.maxTouchPoints, 'ontouchstart' in window, 'ontouchstart' in probe]
              .concat(q.map(x => matchMedia(x).matches)).join(' ');
          };
          globalThis.look = () => { const s = getComputedStyle(probe); return s.color + ' ' + s.width + ' ' + s.height; };
        )JS"});
        page.stylesheet = "#root{position:relative;width:800px;height:600px}"
                          "#probe{width:10px;height:10px;color:rgb(0,0,255)}"
                          "@media (hover: hover){#probe{color:rgb(255,0,0)}}"
                          "@media (hover: none){#probe{color:rgb(0,128,0)}}"
                          "@media (pointer: coarse){#probe{width:44px}}"
                          "@media (any-pointer: coarse) and (any-hover: hover){#probe{height:20px}}";
        page.forgiving_css = true;
        f.view.load(page);
        f.run(16);
        const auto expect = [&](const std::string& what, const std::string& script, const std::string& wanted) {
            const auto got = f.eval(script);
            require(got == wanted, what + ": " + got + ", not " + wanted);
        };
        // Chrome on a desktop: a mouse.
        expect("a mouse", "answers()", "0 false false true false false true true false true false true true false");
        expect("a mouse's rules", "look()", "#ff0000 10px 10px");
        // Chrome with touch emulation: a touch screen of 5 points.
        f.view.set_input_devices({weft::InputDevices::touch, weft::InputDevices::touch}, 5);
        f.run(16);
        expect("a touch screen", "answers()",
               "5 true true false true false true false true false true false false true");
        expect("a touch screen's rules", "look()", "#008000 44px 10px");
        expect("change events", "changes.join(', ')", "coarse true, no hover true");
        // A handler of the touch screen's: it returns false, so the tap makes no click.
        f.eval("globalThis.hits = 0; probe.ontouchstart = e => { hits++; return false; }; "
               "globalThis.clicks = 0; probe.addEventListener('click', () => clicks++);");
        require(f.touch({{1, TouchPhase::Began, 5, 5, 0, 0, 0}}), "ontouchstart prevented the touchstart");
        f.touch({{1, TouchPhase::Ended, 5, 5, 0, 0, 0}});
        expect("ontouchstart", "hits + ' ' + clicks + ' ' + (typeof probe.ontouchstart)", "1 0 function");
        // A touch laptop: a mouse first, a touch screen too.
        f.view.set_input_devices({weft::InputDevices::mouse, weft::InputDevices::mouse | weft::InputDevices::touch},
                                 10);
        f.run(16);
        expect("a touch laptop", "answers()",
               "10 true true true false false true true true true false true true false");
        expect("a touch laptop's rules", "look()", "#ff0000 10px 20px");
        // No pointer at all (a gamepad's): neither hover nor touch handlers.
        f.view.set_input_devices({0, 0}, 0);
        f.run(16);
        expect("no pointer", "answers()",
               "0 false false false false true false false false false true false false true");
        expect("no pointer's rules", "look()", "#008000 10px 10px");
        // Devices the View refuses.
        const auto refuses = [&](weft::InputDevices devices, unsigned points, const std::string& what) {
            bool threw{};
            try {
                f.view.set_input_devices(devices, points);
            } catch (const std::invalid_argument&) {
                threw = true;
            }
            require(threw, "refuses " + what);
        };
        refuses({weft::InputDevices::touch, weft::InputDevices::mouse}, 0, "a primary device not available");
        refuses({weft::InputDevices::touch, weft::InputDevices::touch}, 0, "a touch screen of no points");
        refuses({weft::InputDevices::mouse, weft::InputDevices::mouse}, 2, "touch points without a touch screen");
        refuses({4, 4}, 0, "an unknown device");
        // Kept across loads.
        f.view.set_input_devices({weft::InputDevices::touch, weft::InputDevices::touch}, 5);
        f.view.load(page);
        f.run(16);
        expect("after a load", "answers()", "5 true true false true false true false true false true false false true");
    }

    // Touches the View refuses: one that begins twice, one that changes before it began, a point not finite.
    void refused() {
        Fixture f;
        f.load();
        f.touch({{7, TouchPhase::Began, 100, 100, 0, 0, 0}});
        const auto refuses = [&](std::vector<TouchChange> points, const std::string& what) {
            bool threw{};
            try {
                f.touch(std::move(points));
            } catch (const std::invalid_argument&) {
                threw = true;
            }
            require(threw, "refuses " + what);
        };
        refuses({{7, TouchPhase::Began, 100, 100, 0, 0, 0}}, "a touch that begins twice");
        refuses({{8, TouchPhase::Moved, 100, 100, 0, 0, 0}}, "a move before the touch began");
        refuses({{8, TouchPhase::Began, NAN, 100, 0, 0, 0}}, "a position not finite");
        refuses({{8, TouchPhase::Began, 1, 1, 0, 0, 2}}, "a force past 1");
        refuses({{8, TouchPhase::Began, 1, 1, 0, 0, 0}, {8, TouchPhase::Moved, 1, 1, 0, 0, 0}},
                "a touch twice at once");
        f.touch({{7, TouchPhase::Cancelled, 100, 100, 0, 0, 0}});
    }
} // namespace

int main() {
    try {
        chrome_scenarios();
        flings_stop();
        pointer_capture();
        input_devices();
        refused();
        std::cout << "PASS touch: Chrome's touch events, taps, pans and flings\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
