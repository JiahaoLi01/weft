#define NOMINMAX
#include "view/view.hpp"
#include "support/check.hpp"
#include "support/files.hpp"
#include "support/json_value.hpp"
#include "support/scene_fonts.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

// Spatial navigation and gamepads (WEFT_PAGE_SPATIAL_NAVIGATION, View::gamepad). The scenarios of
// tests/fixtures/spatial_navigation run in a View of a page with spatial navigation and match, event by event, what
// Chrome with --enable-spatial-navigation did on the same page (tools/browser_spatial_navigation_reference.mjs). Then
// what Chrome cannot show: the page without spatial navigation, what lets an element take the focus with it,
// navigator.getGamepads() and its events from a host's states, and a gamepad's keys (the D-pad and the left stick, A
// and B, a held direction's repeats) and when it has none.
namespace {
    using weft::GamepadState;
    using weft::testing::JsonValue;
    using weft::testing::parse_json;
    using weft::testing::require;

    struct Fixture {
        std::shared_ptr<const weft::FontCatalog> fonts =
            std::make_shared<weft::FontCatalog>(weft::testing::chrome_windows_font_catalog());
        weft::SceneText text{*fonts};
        weft::View view{fonts, text, 800, 600, 1};
        double time{};

        void load(std::string script, std::string stylesheet, bool spatial = true) {
            weft::ViewPage page;
            page.scripts.push_back({"page.js", std::move(script)});
            page.stylesheet = std::move(stylesheet);
            page.forgiving_css = true;
            page.spatial_navigation = spatial;
            view.load(page);
            advance();
        }

        // The page of the scenarios.
        void fixture(bool spatial = true) {
            using weft::testing::read_text_file;
            using weft::testing::source_path;
            load(read_text_file(source_path("tests/fixtures/spatial_navigation/spatnav.js")),
                 read_text_file(source_path("tests/fixtures/spatial_navigation/spatnav.css")), spatial);
        }

        std::string eval(const std::string& source) {
            const auto [ok, result] = view.evaluate(source);
            require(ok, "evaluates: " + source + " (" + result + ")");
            return result;
        }

        void advance(double ms = 16) {
            time += ms;
            view.advance(time);
        }

        // A key that types nothing going down and up: the bits of its keydown.
        unsigned press(const std::string& key, unsigned key_code) {
            const auto down = view.key({true, key, key, 0, false, false, key_code});
            view.key({false, key, key, 0, false, false, key_code});
            return down;
        }

        // A click of its own: past the system's double-click time after the last one (the reference's clicks each have
        // a click count of one).
        void click(float x, float y) {
            advance(600);
            view.pointer({weft::PointerKind::Move, x, y, 0});
            view.pointer({weft::PointerKind::Down, x, y, 1});
            view.pointer({weft::PointerKind::Up, x, y, 0});
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

    bool same(const JsonValue& a, const JsonValue& b) {
        return a.kind == b.kind && a.boolean == b.boolean && a.number == b.number && a.string == b.string;
    }

    // Chrome's events of a step against the View's, field by field.
    void compare_events(const JsonValue& chrome, const JsonValue& weft, const std::string& where) {
        std::string listing = "\n  chrome:";
        for (const auto& e : chrome.array) {
            listing += "\n    " + dump(e);
        }
        listing += "\n  weft:";
        for (const auto& e : weft.array) {
            listing += "\n    " + dump(e);
        }
        require(chrome.size() == weft.size(), where + ": " + std::to_string(weft.size()) + " events, Chrome " +
                                                  std::to_string(chrome.size()) + listing);
        for (std::size_t i = 0; i < chrome.size(); ++i) {
            for (const auto& [key, value] : chrome[i].object) {
                const auto* mine = weft[i].find(key);
                require(mine != nullptr, where + ": event " + std::to_string(i) + " has no " + key + listing);
                require(same(value, *mine), where + ": event " + std::to_string(i) + " " + key + " is " + dump(*mine) +
                                                ", Chrome " + dump(value) + listing);
            }
        }
    }

    // Chrome's state after a step against the View's; the scroll container's position within 1 px.
    void compare_state(const JsonValue& chrome, const JsonValue& weft, const std::string& where) {
        for (const auto& [key, value] : chrome.object) {
            const auto* mine = weft.find(key);
            require(mine != nullptr, where + ": no state " + key);
            if (key == "scroll") {
                require(std::abs(value.number - mine->number) <= 1,
                        where + ": " + key + " " + dump(*mine) + ", Chrome " + dump(value));
            } else {
                require(same(value, *mine), where + ": " + key + " " + dump(*mine) + ", Chrome " + dump(value));
            }
        }
    }

    // Plays a step of scenarios.json as a host gives it to a View: a key's keydown, its text, its keyup (with the step's
    // modifiers); a click at an element's centre (a move, a press, a release); focus() and blur().
    void play(Fixture& f, const JsonValue& keys, const JsonValue& step) {
        unsigned modifiers{};
        if (const auto* mods = step.find("mods")) {
            for (const char m : mods->string) {
                modifiers |= m == 'S'   ? weft::key_modifier::shift
                             : m == 'C' ? weft::key_modifier::control
                             : m == 'A' ? weft::key_modifier::alt
                                        : weft::key_modifier::meta;
            }
        }
        const auto key = [&](bool down, const std::string& name) {
            const auto& k = keys[name];
            f.view.key({down, k["key"].string, k["code"].string, modifiers, false, false, unsigned(k["vk"].number)});
            // A host gives every character, Enter's '\r' too; weft takes Enter's keypress from its keydown.
            if (const auto* text = k.find("text"); down && text) {
                f.view.text(text->string);
            }
        };
        if (const auto* name = step.find("press")) {
            key(true, name->string);
            key(false, name->string);
        } else if (const auto* down = step.find("down")) {
            key(true, down->string);
        } else if (const auto* up = step.find("up")) {
            key(false, up->string);
        } else if (const auto* id = step.find("click")) {
            const auto centre = parse_json(f.eval("JSON.stringify((r => [r.left + r.width / 2, r.top + r.height / 2])("
                                                  "document.getElementById('" +
                                                  id->string + "').getBoundingClientRect()))"));
            f.click(float(centre[0].number), float(centre[1].number));
        } else if (const auto* focus = step.find("focus")) {
            f.eval("document.getElementById('" + focus->string + "').focus()");
        } else if (step.find("blur")) {
            f.eval("document.activeElement && document.activeElement.blur()");
        } else {
            throw std::invalid_argument("unknown step " + dump(step));
        }
    }

    // Each scenario in a new View, each step's events and state against Chrome's.
    void chrome_scenarios() {
        using weft::testing::read_text_file;
        using weft::testing::source_path;
        const auto spec = parse_json(read_text_file(source_path("tests/fixtures/spatial_navigation/scenarios.json")));
        const auto chrome =
            parse_json(read_text_file(source_path("tests/fixtures/spatial_navigation/chrome-results.json")));
        std::size_t steps{};
        for (const auto& scenario : spec["scenarios"].array) {
            const auto& name = scenario["name"].string;
            const auto& results = chrome["scenarios"][name];
            require(results.size() == scenario["steps"].size(), name + ": Chrome's steps");
            Fixture f;
            f.fixture();
            f.eval("__spatnav.take()");
            for (std::size_t i = 0; i < scenario["steps"].size(); ++i) {
                const auto& step = scenario["steps"][i];
                play(f, spec["keys"], step);
                f.advance();
                const auto events = parse_json(f.eval("JSON.stringify(__spatnav.take())"));
                const auto state = parse_json(f.eval("JSON.stringify(__spatnav.state())"));
                const auto where = name + " step " + std::to_string(i) + " " + dump(step);
                compare_events(results[i]["events"], events, where);
                compare_state(results[i]["state"], state, where);
                ++steps;
            }
        }
        require(steps > 120, "the scenarios ran");
    }

    // A standard gamepad in a slot, nothing pressed.
    GamepadState pad(unsigned index = 0) {
        GamepadState s;
        s.index = index;
        s.connected = true;
        s.id = "Xbox 360 Controller (XInput STANDARD GAMEPAD)";
        return s;
    }

    // The gamepad with a button pressed all the way.
    GamepadState pressing(GamepadState s, unsigned button) {
        s.buttons[button] = 1;
        s.pressed |= 1u << button;
        s.touched |= 1u << button;
        return s;
    }

    // The page of the scenarios without spatial navigation: the arrow keys leave the focus where it is (nothing on the
    // page scrolls) and the pointer off the page, Escape keeps the focus; a gamepad shows, but has no keys.
    void without_spatial_navigation() {
        Fixture f;
        f.fixture(false);
        f.eval("document.getElementById('g00').focus(); __spatnav.take()");
        require(f.press("ArrowRight", 39) == 0 && f.press("ArrowDown", 40) == 0 && f.press("Escape", 27) == 0,
                "the keys do nothing");
        require(f.view.gamepad(pressing(pad(), 13)) == 0, "nor the D-pad");
        const auto state = parse_json(f.eval("JSON.stringify(__spatnav.state())"));
        require(state["active"].string == "g00" && state["hover"].kind == JsonValue::Kind::Null,
                "g00 keeps the focus, nothing is under the pointer: " + dump(state));
        require(f.eval("__spatnav.take().map(e => e.type + '@' + e.target).join(' ')") ==
                    "keydown@g00 keyup@g00 keydown@g00 keyup@g00 keydown@g00 keyup@g00",
                "the keys' events alone");
        require(f.eval("navigator.getGamepads()[0].buttons[13].pressed") == "true", "the gamepad shows");
        // The elements with listeners and the card take no focus: focus() leaves it, Tab passes them (from Jump round to
        // the first button); Enter on the panel neither makes it :active nor clicks it.
        f.eval("document.getElementById('clicky').focus(); document.getElementById('card').focus()");
        require(f.eval("document.activeElement.id") == "g00", "clicky and the card take no focus");
        f.eval("document.getElementById('jump').focus()");
        f.press("Tab", 9);
        require(f.eval("document.activeElement.id") == "g00", "Tab passes them");
        f.eval("document.getElementById('panel').focus(); __spatnav.take()");
        f.view.key({true, "Enter", "Enter", 0, false, false, 13});
        require(f.eval("String(__spatnav.state().pressed)") == "null", "Enter makes nothing :active");
        f.view.key({false, "Enter", "Enter", 0, false, false, 13});
        require(f.eval("__spatnav.take().map(e => e.type + '@' + e.target).join(' ')") ==
                    "keydown@panel keypress@panel keyup@panel",
                "Enter clicks no panel");
    }

    // What lets an element take the focus with spatial navigation, past Chrome's scenarios: a listener of the events
    // (keyup here) but not one removed again; cursor: pointer set on the element, not inherited (cursor: initial in
    // between), and as the keyword after a list of images; an SVG element's focus listener. Without spatial navigation
    // none of them. (An explicit cursor: inherit counts as set: the cascade keeps the parent's value, not the keyword.)
    void spatial_focus_rules() {
        for (const bool spatial : {true, false}) {
            Fixture f;
            f.load(R"JS(
                const root = document.getElementById('root');
                const add = (id, parent, tag) => {
                  const n = tag ? document.createElementNS('http://www.w3.org/2000/svg', tag)
                                : document.createElement('div');
                  n.setAttribute('id', id);
                  (parent || root).appendChild(n);
                  return n;
                };
                const handler = () => {};
                const gone = add('gone');
                gone.addEventListener('click', handler);
                gone.removeEventListener('click', handler);
                add('keys').addEventListener('keyup', () => {});
                add('images', add('initial', add('plain', add('hand'))));
                const shape = add('shape', add('svg', null, 'svg'), 'rect');
                shape.setAttribute('width', '20');
                shape.setAttribute('height', '20');
                shape.addEventListener('focus', () => {});
              )JS",
                   "#root{position:relative;width:100%;height:100%}"
                   "div{width:40px;height:20px;margin:4px}"
                   "#hand{cursor:pointer}#initial{cursor:initial}#images{cursor:url(hand.png) 4 4,Pointer}"
                   "svg{display:block;width:40px;height:40px}",
                   spatial);
            const std::string where = spatial ? "with spatial navigation: " : "without: ";
            std::string taken;
            for (const std::string id : {"gone", "keys", "hand", "plain", "initial", "images", "shape"}) {
                f.eval("document.activeElement.blur(); document.getElementById('" + id + "').focus()");
                if (f.eval("document.activeElement.id") == id) {
                    taken += id + " ";
                }
            }
            require(taken == (spatial ? "keys hand images shape " : ""), where + "focus() focuses " + taken);
            f.eval("document.activeElement.blur()");
            std::string order;
            for (int k = 0; k < 4; ++k) {
                f.press("Tab", 9);
                order += f.eval("document.activeElement.id") + " ";
            }
            require(order == (spatial ? "keys hand images shape " : "    "), where + "Tab: " + order);
        }
    }

    // The log of a page made by log_script: the key, click and focus events window sees in its capture phase, as
    // type@target, with a key's value (+ for a repeat); the gamepad events as type:index (! when the event is not a
    // trusted GamepadEvent whose gamepad is connected as its type says).
    const char* log_script = R"JS(
        globalThis.log = [];
        const id = n => !n ? 'null' : n === window ? 'window' : n === document.body ? 'body' : n.id || n.localName;
        for (const type of ['keydown', 'keypress', 'keyup', 'click', 'focus', 'blur'])
          window.addEventListener(type, e => log.push(type + '@' + id(e.target) +
            (/^key/.test(type) ? ':' + e.key + (e.repeat ? '+' : '') : '')), true);
        for (const type of ['gamepadconnected', 'gamepaddisconnected'])
          window.addEventListener(type, e => log.push(type + ':' + e.gamepad.index + (e.isTrusted &&
            e instanceof GamepadEvent && e.gamepad instanceof Gamepad &&
            e.gamepad.connected === (type === 'gamepadconnected') ? '' : '!')));
        globalThis.take = () => { const out = log.join(' '); log.length = 0; return out; };
      )JS";

    // A page of buttons a, b and c, one under the other, logged; a has the focus.
    void column(Fixture& f, bool spatial = true) {
        f.load(std::string(log_script) + R"JS(
            const root = document.getElementById('root');
            for (const name of ['a', 'b', 'c']) {
              const b = document.createElement('button');
              b.setAttribute('id', name);
              b.textContent = name;
              root.appendChild(b);
            }
            document.getElementById('a').focus();
          )JS",
               "#root{position:relative;width:100%;height:100%}"
               "button{display:block;width:80px;height:30px;margin:10px}",
               spatial);
        f.eval("take()");
    }

    std::string milliseconds(double time) {
        return std::to_string(std::llround(time));
    }

    // navigator.getGamepads() and the gamepad events, as Chrome's: four slots, nothing until a gesture (a button
    // pressed, an axis past half) and then gamepadconnected for each gamepad; snapshots that keep the time their state
    // last changed; gamepaddisconnected and gamepadconnected later; the interfaces; a page loaded again; what
    // View::gamepad refuses.
    void gamepad_api() {
        Fixture f;
        column(f, false);
        require(f.eval("JSON.stringify(navigator.getGamepads())") == "[null,null,null,null]", "four empty slots");
        // Before a gesture: a trigger touched short of pressed, the axes at half; a second gamepad.
        auto s = pad();
        s.buttons[6] = 0.125f;
        s.touched = 1u << 6;
        s.axes = {0.5f, -0.5f, 0.25f, 0};
        require(f.view.gamepad(s) == 0, "no keys without spatial navigation");
        auto second = pad(2);
        f.view.gamepad(second);
        require(f.eval("JSON.stringify(navigator.getGamepads())") == "[null,null,null,null]" &&
                    f.eval("take()").empty(),
                "nothing before a gesture");
        // The gesture: gamepadconnected for both.
        f.advance();
        s.axes[1] = -0.75f;
        f.view.gamepad(s);
        require(f.eval("take()") == "gamepadconnected:0 gamepadconnected:2", "both connected after the gesture");
        const auto gamepad = parse_json(f.eval(R"JS(JSON.stringify((g => ({id: g.id, index: g.index,
            connected: g.connected, mapping: g.mapping, timestamp: g.timestamp, axes: g.axes, buttons: g.buttons.length,
            trigger: [g.buttons[6].pressed, g.buttons[6].touched, g.buttons[6].value], haptics: g.vibrationActuator,
            frozen: Object.isFrozen(g.axes) && Object.isFrozen(g.buttons)}))(navigator.getGamepads()[0])))JS"));
        require(gamepad["id"].string == "Xbox 360 Controller (XInput STANDARD GAMEPAD)" &&
                    gamepad["index"].number == 0 && gamepad["connected"].boolean &&
                    gamepad["mapping"].string == "standard" && gamepad["timestamp"].number == f.time &&
                    dump(gamepad["axes"]) == "[0.5,-0.75,0.25,0]" && gamepad["buttons"].number == 17 &&
                    dump(gamepad["trigger"]) == "[false,true,0.125]" &&
                    gamepad["haptics"].kind == JsonValue::Kind::Null && gamepad["frozen"].boolean,
                "the gamepad: " + dump(gamepad));
        require(f.eval("navigator.getGamepads()[2].timestamp") == milliseconds(f.time - 16),
                "the second gamepad's time is its state's");
        // Snapshots: one taken before keeps its state; the same state keeps its time, a new one has the page's clock.
        f.eval("globalThis.before = navigator.getGamepads()[0]");
        const auto changed = f.time;
        f.advance();
        f.view.gamepad(s);
        require(f.eval("navigator.getGamepads()[0].timestamp") == milliseconds(changed), "the same state, its time");
        f.advance();
        s = pressing(s, 0);
        f.view.gamepad(s);
        require(f.eval("(g => [g.timestamp, g.buttons[0].pressed, g.buttons[0].value, before.buttons[0].pressed, "
                       "before.timestamp].join())(navigator.getGamepads()[0])") ==
                    milliseconds(f.time) + ",true,1,false," + milliseconds(changed),
                "a new state; the snapshot before it");
        // Touched as Blink has it: a button pressed, or above 0, is touched without its bit.
        s.buttons[7] = 0.25f;
        s.pressed |= 1u << 9;
        s.buttons[9] = 1;
        s.touched = 0;
        f.view.gamepad(s);
        require(f.eval("(b => [b[7].touched, b[7].pressed, b[9].touched, b[1].touched].join())"
                       "(navigator.getGamepads()[0].buttons)") == "true,false,true,false",
                "touched");
        // A gamepad that goes, and comes again.
        second.connected = false;
        f.view.gamepad(second);
        require(f.eval("take()") == "gamepaddisconnected:2" && f.eval("navigator.getGamepads()[2]") == "null",
                "the second gamepad goes");
        second.connected = true;
        f.view.gamepad(second);
        require(f.eval("take()") == "gamepadconnected:2", "and comes again");
        // The interfaces: no constructor for a Gamepad, a GamepadEvent needs one.
        require(f.eval("(() => { try { new Gamepad(); } catch (e) { return e instanceof TypeError; } })()") == "true" &&
                    f.eval("(() => { try { new GamepadEvent('gamepadconnected'); } catch (e) { "
                           "return e instanceof TypeError; } })()") == "true" &&
                    f.eval("navigator.getGamepads()[0] instanceof Gamepad") == "true",
                "Gamepad and GamepadEvent");

        // A page loaded again shows no gamepad until a gesture there; one that goes before it fires nothing.
        column(f, false);
        auto other = pad(1);
        f.view.gamepad(other);
        other.connected = false;
        f.view.gamepad(other);
        require(f.eval("JSON.stringify(navigator.getGamepads())") == "[null,null,null,null]" &&
                    f.eval("take()").empty(),
                "a new page waits for a gesture");
        f.view.gamepad(s);
        require(f.eval("take()") == "gamepadconnected:0" && f.eval("navigator.getGamepads()[1]") == "null",
                "its gesture");

        const auto refuses = [&](const GamepadState& bad, const std::string& what) {
            try {
                f.view.gamepad(bad);
            } catch (const std::invalid_argument&) {
                return;
            }
            throw std::runtime_error("View::gamepad takes " + what);
        };
        auto bad = pad();
        bad.index = 4;
        refuses(bad, "a fifth slot");
        bad = pad();
        bad.buttons[2] = 1.5f;
        refuses(bad, "a button past 1");
        bad = pad();
        bad.buttons[2] = -0.25f;
        refuses(bad, "a button under 0");
        bad = pad();
        bad.axes[3] = std::numeric_limits<float>::quiet_NaN();
        refuses(bad, "an axis of NaN");
        bad = pad();
        bad.pressed = 1u << 17;
        refuses(bad, "an eighteenth button");
        bad = pad();
        bad.id = std::string(129, 'x');
        refuses(bad, "an id past 128 bytes");
        bad = pad();
        bad.id = "\xff";
        refuses(bad, "an id of no UTF-8");
    }

    // A gamepad's keys on a page with spatial navigation: the D-pad and the left stick are the arrow keys, A Enter and
    // B Escape; a held direction repeats after 0.5 s every 0.1 s (from the frame's time after a long frame); what comes
    // up goes before what goes down; none without the host's focus; keyups as the gamepad goes.
    void gamepad_keys() {
        Fixture f;
        column(f);
        // The D-pad's down: ArrowDown's keydown, which moves the focus; nothing more while it is held.
        const auto down = pressing(pad(), 13);
        const auto at = f.time;
        require(f.view.gamepad(down) == weft::input_default, "the D-pad's down moved the focus");
        require(f.eval("take()") == "gamepadconnected:0 keydown@a:ArrowDown blur@a focus@b", "ArrowDown");
        f.advance();
        require(f.view.gamepad(down) == 0 && f.eval("take()").empty(), "held");
        // Its repeats.
        f.advance(at + 499 - f.time);
        require(f.eval("take()").empty(), "no repeat before 0.5 s");
        f.advance(1);
        require(f.eval("take()") == "keydown@b:ArrowDown+ blur@b focus@c", "the first repeat");
        f.advance(99);
        require(f.eval("take()").empty(), "none before 0.1 s more");
        f.advance(1);
        require(f.eval("take()") == "keydown@c:ArrowDown+", "the next");
        f.advance(1000);
        require(f.eval("take()") == "keydown@c:ArrowDown+", "one after a long frame");
        f.advance(16);
        require(f.eval("take()").empty(), "none right after it");
        f.advance(84);
        require(f.eval("take()") == "keydown@c:ArrowDown+", "0.1 s after it");
        // Up: its keyup, and no more repeats.
        require(f.view.gamepad(pad()) == 0 && f.eval("take()") == "keyup@c:ArrowDown", "the D-pad comes up");
        f.advance(1000);
        require(f.eval("take()").empty(), "no repeats after it");

        // The left stick past half, by its larger axis; the right stick has no keys.
        auto stick = pad();
        stick.axes = {0, -0.75f, 0, 0};
        require(f.view.gamepad(stick) == weft::input_default && f.eval("take()") == "keydown@c:ArrowUp blur@c focus@b",
                "the stick up");
        stick.axes = {0.75f, -0.5f, 0, 0};
        f.view.gamepad(stick);
        require(f.eval("take()") == "keyup@b:ArrowUp keydown@b:ArrowRight", "turned right: up comes up first");
        stick.axes = {0.25f, 0.5f, 0, 0};
        f.view.gamepad(stick);
        require(f.eval("take()") == "keyup@b:ArrowRight", "back within half");
        stick.axes = {0, 0, 0.75f, 1};
        require(f.view.gamepad(stick) == 0 && f.eval("take()").empty(), "the right stick");

        // A: Enter, which clicks the focused button. B: Escape, which blurs it.
        require(f.view.gamepad(pressing(pad(), 0)) == weft::input_default, "A clicks");
        f.view.gamepad(pad());
        require(f.eval("take()") == "keydown@b:Enter keypress@b:Enter click@b keyup@b:Enter", "A");
        require(f.view.gamepad(pressing(pad(), 1)) == weft::input_default, "B blurs");
        f.view.gamepad(pad());
        require(f.eval("take()") == "keydown@b:Escape blur@b keyup@body:Escape", "B");

        // Without the host's focus a gamepad has no keys; a direction still held when the focus comes back goes down.
        f.eval("document.getElementById('a').focus()");
        f.view.set_focus(false);
        f.eval("take()");
        require(f.view.gamepad(down) == 0 && f.eval("take()").empty() && f.eval("document.activeElement.id") == "a",
                "no keys without the host's focus");
        f.advance(1000);
        require(f.eval("take()").empty(), "nor repeats");
        f.view.set_focus(true);
        f.eval("take()");
        require(f.view.gamepad(down) == weft::input_default && f.eval("take()") == "keydown@a:ArrowDown blur@a focus@b",
                "the held direction with the focus back");
        // A gamepad that goes while a direction is held: the direction's keyup, no more repeats.
        auto gone = down;
        gone.connected = false;
        require(f.view.gamepad(gone) == 0 && f.eval("take()") == "gamepaddisconnected:0 keyup@b:ArrowDown",
                "the gamepad goes");
        f.advance(1000);
        require(f.eval("take()").empty(), "no repeats after it");
    }
} // namespace

int main() {
    try {
        chrome_scenarios();
        without_spatial_navigation();
        spatial_focus_rules();
        gamepad_api();
        gamepad_keys();
        std::cout << "spatial_navigation_contract passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
