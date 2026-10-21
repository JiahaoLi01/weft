#define NOMINMAX
#include <windows.h>
#include "scene/scene.hpp"
#include "view/view.hpp"
#include "view/view_keys.hpp"
#include "support/check.hpp"
#include "support/files.hpp"
#include "support/json_value.hpp"
#include "support/scene_fonts.hpp"

#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Keyboard input and the focus (View::key, View::text, View::set_focus). The scenarios of tests/fixtures/keyboard run
// in a View and match, event by event, what Chrome did on the same page (tools/browser_keyboard_reference.mjs); where
// weft chooses otherwise (Tab comes round from the last element to the first; Shift+Tab after a click on text starts at
// the clicked element) the test says what weft does instead. Then what one page in Chrome cannot show: the host's
// focus, text after a key the page used, keys an input method took, keyCodes from codes, reentrant listeners, and pages
// without the web platform.
namespace {
    using weft::KeyInput;
    using weft::PointerKind;
    using weft::testing::JsonValue;
    using weft::testing::parse_json;
    using weft::testing::require;

    struct Fixture {
        std::shared_ptr<const weft::FontCatalog> fonts =
            std::make_shared<weft::FontCatalog>(weft::testing::chrome_windows_font_catalog());
        weft::SceneText text{*fonts};
        weft::View view{fonts, text, 800, 600, 1};
        double time{};

        void load(std::string script, std::string stylesheet, bool forgiving = true, bool web = false) {
            weft::ViewPage page;
            page.scripts.push_back({"page.js", std::move(script)});
            page.stylesheet = std::move(stylesheet);
            page.forgiving_css = forgiving;
            page.web_platform = web;
            view.load(page);
        }

        void fixture() {
            using weft::testing::read_text_file;
            using weft::testing::source_path;
            load(read_text_file(source_path("tests/fixtures/keyboard/keyboard.js")),
                 read_text_file(source_path("tests/fixtures/keyboard/keyboard.css")));
        }

        std::string eval(const std::string& source) {
            const auto [ok, result] = view.evaluate(source);
            require(ok, "evaluates: " + source + " (" + result + ")");
            return result;
        }

        void advance() {
            time += 16;
            view.advance(time);
        }

        unsigned key(bool down, std::string key, std::string code, unsigned modifiers = 0, unsigned vk = 0) {
            return view.key({down, std::move(key), std::move(code), modifiers, false, false, vk});
        }

        // A key going down, its text, and up: the bits of the keydown and of the text.
        std::pair<unsigned, unsigned> press(const std::string& key, const std::string& code, unsigned modifiers = 0,
                                            std::string typed_text = {}) {
            const auto down = this->key(true, key, code, modifiers);
            const auto typed = typed_text.empty() ? 0u : view.text(typed_text);
            this->key(false, key, code, modifiers);
            return {down, typed};
        }

        // A click of its own: past the system's double-click time after the last one (the reference's clicks each have
        // a click count of one).
        void click(float x, float y) {
            time += 600;
            view.advance(time);
            view.pointer({PointerKind::Move, x, y, 0});
            view.pointer({PointerKind::Down, x, y, 1});
            view.pointer({PointerKind::Up, x, y, 0});
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

    // Chrome's events of a step against the View's, field by field; positions within 1 px (the boxes come from two
    // layouts, and mouse events floor them).
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
                if (key == "x" || key == "y") {
                    require(std::abs(value.number - mine->number) <= 1,
                            where + ": event " + std::to_string(i) + " " + key + listing);
                } else {
                    require(same(value, *mine), where + ": event " + std::to_string(i) + " " + key + " is " +
                                                    dump(*mine) + ", Chrome " + dump(value) + listing);
                }
            }
        }
    }

    void compare_state(const JsonValue& chrome, const JsonValue& weft, const std::string& where) {
        for (const auto& [key, value] : chrome.object) {
            const auto* mine = weft.find(key);
            require(mine != nullptr, where + ": no state " + key);
            if (key == "scroll" || key == "scroll2") {
                require(std::abs(value.number - mine->number) <= 1,
                        where + ": " + key + " " + dump(*mine) + ", Chrome " + dump(value));
            } else {
                require(same(value, *mine), where + ": " + key + " " + dump(*mine) + ", Chrome " + dump(value));
            }
        }
    }

    // Plays the steps of scenarios.json as a host gives them to a View: a key's keydown with the modifiers held, its
    // text, its keyup; a click at an element's centre (a move, a press, a release); focus(), blur() and scripts.
    struct Player {
        Fixture& f;
        const JsonValue& keys;
        std::set<char> held;

        static unsigned bit(char m) {
            switch (m) {
                case 'S':
                    return weft::key_modifier::shift;
                case 'C':
                    return weft::key_modifier::control;
                case 'A':
                    return weft::key_modifier::alt;
                case 'M':
                    return weft::key_modifier::meta;
            }
            throw std::invalid_argument("modifier");
        }

        void key(bool down, const std::string& name, const JsonValue& step) {
            const auto& k = keys[name];
            const auto& key = k["key"].string;
            static const std::map<std::string, char> modifier_keys{
                {"Shift", 'S'}, {"Control", 'C'}, {"Alt", 'A'}, {"Meta", 'M'}};
            if (const auto it = modifier_keys.find(key); it != modifier_keys.end()) {
                if (down) {
                    held.insert(it->second);
                } else {
                    held.erase(it->second);
                }
            }
            unsigned modifiers{};
            for (const char m : held) {
                modifiers |= bit(m);
            }
            if (const auto* mods = step.find("mods")) {
                for (const char m : mods->string) {
                    modifiers |= bit(m);
                }
            }
            const auto* repeat = step.find("repeat");
            f.view.key(
                {down, key, k["code"].string, modifiers, down && repeat && repeat->boolean, false, unsigned(k["vk"].number)});
            // A host gives every character, Enter's '\r' too; weft takes Enter's keypress from its keydown.
            if (const auto* text = k.find("text"); down && text) {
                f.view.text(text->string);
            }
        }

        void play(const JsonValue& step) {
            if (const auto* name = step.find("press")) {
                key(true, name->string, step);
                key(false, name->string, step);
            } else if (const auto* down = step.find("down")) {
                key(true, down->string, step);
            } else if (const auto* up = step.find("up")) {
                key(false, up->string, step);
            } else if (const auto* id = step.find("click")) {
                const auto centre = parse_json(f.eval("JSON.stringify(__keyboard.centre('" + id->string + "'))"));
                f.click(float(centre[0].number), float(centre[1].number));
            } else if (const auto* focus = step.find("focus")) {
                f.eval("document.getElementById('" + focus->string + "').focus()");
            } else if (step.find("blur")) {
                f.eval("document.activeElement && document.activeElement.blur()");
            } else if (const auto* script = step.find("script")) {
                f.eval(script->string);
            } else {
                throw std::invalid_argument("unknown step " + dump(step));
            }
        }
    };

    // Each scenario in a new View, each step's events and state against Chrome's. From a step listed here on, weft
    // differs by design and gives the focus to the element named.
    void chrome_scenarios() {
        using weft::testing::read_text_file;
        using weft::testing::source_path;
        const auto spec = parse_json(read_text_file(source_path("tests/fixtures/keyboard/scenarios.json")));
        const auto chrome = parse_json(read_text_file(source_path("tests/fixtures/keyboard/chrome-results.json")));
        const std::map<std::string, std::pair<std::size_t, std::string>> differs{
            // Shift+Tab on the first element comes round to the last; Chrome's focus leaves the page for its UI.
            {"tab-order", {15, "tail"}},
            // Shift+Tab after a click on text starts at the element clicked (a0, a1 before it); Chrome starts after
            // the text (t2 after it, then t1 before that). weft's hit tests find elements, not text.
            {"start-point", {21, "a1"}},
        };
        std::size_t steps{};
        for (const auto& scenario : spec["scenarios"].array) {
            const auto& name = scenario["name"].string;
            const auto& results = chrome["scenarios"][name];
            Fixture f;
            f.fixture();
            f.advance();
            f.eval("__keyboard.take()");
            Player player{f, spec["keys"]};
            const auto differ = differs.find(name);
            for (std::size_t i = 0; i < scenario["steps"].size(); ++i) {
                const auto& step = scenario["steps"][i];
                player.play(step);
                f.advance();
                const auto events = parse_json(f.eval("JSON.stringify(__keyboard.take())"));
                const auto state = parse_json(f.eval("JSON.stringify(__keyboard.state())"));
                const auto where = name + " step " + std::to_string(i) + " " + dump(step);
                if (differ != differs.end() && i >= differ->second.first) {
                    if (i == differ->second.first) {
                        require(state["active"].string == differ->second.second,
                                where + ": weft gives the focus to " + differ->second.second + ", not " +
                                    dump(state["active"]));
                    }
                    continue;
                }
                compare_events(results[i]["events"], events, where);
                compare_state(results[i]["state"], state, where);
                ++steps;
            }
        }
        require(steps > 100, "the scenarios ran");
    }

    // The log of a page made by log_script: the events window sees in its capture phase, as type@target(related).
    const char* log_script = R"JS(
        globalThis.log = [];
        const id = n => !n ? 'null' : n === window ? 'window' : n === document.body ? 'body' : n.id || n.localName;
        for (const type of ['keydown', 'keypress', 'keyup', 'click', 'focus', 'blur', 'focusin', 'focusout'])
          window.addEventListener(type, e => log.push(type + '@' + id(e.target) +
            (e.relatedTarget !== undefined && /focus|blur/.test(type) ? '(' + id(e.relatedTarget) + ')' : '')), true);
        globalThis.take = () => { const out = log.join(' '); log.length = 0; return out; };
      )JS";

    // A page of buttons a, b, c in #root, logged.
    void buttons(Fixture& f, const std::string& more = {}, const std::string& css = {}) {
        f.load(std::string(log_script) + R"JS(
            const root = document.getElementById('root');
            for (const name of ['a', 'b', 'c']) {
              const b = document.createElement('button');
              b.setAttribute('id', name);
              b.textContent = name;
              root.appendChild(b);
            }
          )JS" + more,
               "#root{position:relative;width:100%;height:100%}button{width:40px;height:20px}" + css);
        f.advance();
    }

    // Tab comes round from the last element to the first and back; a page with nothing to focus keeps the key.
    void tab_comes_round() {
        Fixture f;
        buttons(f);
        f.eval("document.getElementById('c').focus(); take()");
        require(f.press("Tab", "Tab").first == weft::input_default, "Tab moves the focus");
        require(f.eval("document.activeElement.id") == "a", "from the last to the first");
        f.key(true, "Shift", "ShiftLeft", weft::key_modifier::shift);
        f.press("Tab", "Tab", weft::key_modifier::shift);
        f.key(false, "Shift", "ShiftLeft");
        require(f.eval("document.activeElement.id") == "c", "from the first back to the last");

        Fixture empty;
        empty.load("document.getElementById('root').textContent = 'nothing to focus'", "#root{width:100px}");
        empty.advance();
        require(empty.press("Tab", "Tab").first == 0, "nothing takes the focus: the host keeps Tab");
    }

    // The host's focus leaving the View blurs the focused element without moving the focus; coming back focuses it
    // again. :focus and document.hasFocus() follow; focus() meanwhile moves the focus silently.
    void host_focus() {
        Fixture f;
        buttons(f, {}, "#a:focus{width:60px}#b:focus-visible{width:70px}");
        f.eval("document.getElementById('a').focus(); take()");
        f.advance();
        const auto width = [&](const char* id) {
            return f.eval("document.getElementById('" + std::string(id) + "').getBoundingClientRect().width");
        };
        require(width("a") == "60", ":focus while the View has the host's focus");
        f.view.set_focus(false);
        require(!f.view.focused(), "the View lost the host's focus");
        require(f.eval("take()") == "blur@a(null) focusout@a(null) blur@window(null)", "blur, focusout, window blur: " +
                                                                                           f.eval("String(log)"));
        require(f.eval("document.activeElement.id + document.hasFocus()") == "afalse", "the focus stays put");
        f.advance();
        require(width("a") == "40", "no :focus without the host's focus");
        f.eval("document.getElementById('b').focus()");
        require(f.eval("take()").empty(), "no events while the page has no focus");
        f.view.set_focus(true);
        require(f.eval("take()") == "focus@window(null) focus@b(null) focusin@b(null)", "window focus, focus, focusin");
        require(f.eval("document.hasFocus()") == "true", "hasFocus");
        f.advance();
        require(width("b") == "70", ":focus-visible of an element script focused with no mouse focus before");

        // The View keeps the host's focus state across loads.
        f.view.set_focus(false);
        buttons(f);
        require(f.eval("document.hasFocus()") == "false", "a new page in a View without the host's focus");
        f.view.set_focus(true);
    }

    // What hosts read back: a key the page prevented, a default action, keys that do nothing; the text after a used
    // keydown; Enter's own keypress; keys an input method took; keyCodes and locations from codes.
    void handled_bits() {
        Fixture f;
        buttons(f, R"JS(
            document.getElementById('b').addEventListener('keydown', e => { if (e.key === 'x') e.preventDefault(); });
            globalThis.last = null;
            window.addEventListener('keydown', e => { last = e; }, true);
            window.addEventListener('keypress', e => { last = e; }, true);
          )JS");
        require(f.press("ArrowDown", "ArrowDown").first == 0, "nothing to scroll: the host keeps the key");
        require(f.press("a", "KeyA", 0, "a") == std::pair<unsigned, unsigned>{0, 0}, "a key nothing uses");
        f.eval("document.getElementById('b').focus(); take()");
        // A key the page prevents drops its text.
        require(f.key(true, "x", "KeyX") == weft::input_prevented, "prevented");
        require(f.view.text("x") == weft::input_default, "its text is dropped");
        f.key(false, "x", "KeyX");
        require(f.eval("take()") == "keydown@b keyup@b", "no keypress after a prevented keydown: " + f.eval("String(log)"));
        // Space on a button: its keydown begins the activation, its text still reaches the page, its keyup clicks.
        require(f.key(true, " ", "Space") == weft::input_default, "Space makes the button :active");
        require(f.eval("document.getElementById('b').matches(':active')") == "true", ":active");
        require(f.view.text(" ") == weft::input_default, "the keypress, which scrolls nothing");
        require(f.key(false, " ", "Space") == weft::input_default, "the keyup clicks");
        require(f.eval("take()") == "keydown@b keypress@b keyup@b click@b", "Space's events: " + f.eval("String(log)"));
        require(f.eval("document.getElementById('b').matches(':active')") == "false", "no longer :active");
        // Enter's keypress comes from its keydown; with Control none does (Windows types no carriage return).
        f.press("Enter", "Enter", 0, "\r");
        require(f.eval("take()") == "keydown@b keypress@b click@b keyup@b", "Enter: " + f.eval("String(log)"));
        f.press("Enter", "Enter", weft::key_modifier::control, "\n");
        require(f.eval("take()") == "keydown@b keyup@b", "Ctrl+Enter: " + f.eval("String(log)"));
        // A key an input method took: "Process", keyCode 229, no default action.
        require(f.view.key({true, "Tab", "Tab", 0, false, true, 9}) == 0, "an input method's Tab does nothing");
        require(f.eval("[last.key, last.keyCode, last.code, document.activeElement.id].join()") == "Process,229,Tab,b",
                "Process: " + f.eval("[last.key, last.keyCode].join()"));
        f.view.key({false, "Tab", "Tab", 0, false, true, 9});
        // keyCode from the code when the host gives none; location from the code; the modifiers' state.
        f.key(true, ";", "Semicolon");
        require(f.eval("[last.keyCode, last.which, last.location].join()") == "186,186,0", "keyCode of Semicolon");
        f.view.text(";");
        require(f.eval("[last.type, last.keyCode, last.charCode, last.key, last.code].join()") == "keypress,59,59,;,Semicolon",
                "keypress of ;");
        f.key(false, ";", "Semicolon");
        f.key(true, "Enter", "NumpadEnter");
        require(f.eval("[last.type, last.keyCode, last.location].join()") == "keypress,13,3", "NumpadEnter's keypress");
        f.key(false, "Enter", "NumpadEnter");
        f.key(true, "A", "KeyA", weft::key_modifier::shift | weft::key_modifier::caps_lock | weft::key_modifier::alt_graph);
        require(f.eval("['Shift', 'CapsLock', 'AltGraph', 'Control', 'NumLock'].map(k => last.getModifierState(k)).join()") ==
                    "true,true,true,false,false",
                "getModifierState");
        require(f.eval("last.shiftKey && !last.ctrlKey && last.isTrusted && last instanceof KeyboardEvent") == "true",
                "a trusted KeyboardEvent");
        f.key(false, "A", "KeyA");
        // An auto-repeat repeats keydown and keypress only.
        f.view.key({true, "a", "KeyA", 0, true, false, 0});
        require(f.eval("last.repeat") == "true", "a repeating keydown");
        f.view.text("a");
        require(f.eval("last.type + last.repeat") == "keypresstrue", "its keypress repeats");
        f.view.key({false, "a", "KeyA", 0, true, false, 0});
        require(f.eval("last.type") == "keypress", "keyup is not logged here");
    }

    // Listeners that move the focus while it moves, or remove what has it.
    void reentrant_listeners() {
        Fixture f;
        buttons(f, R"JS(
            const [a, b, c] = ['a', 'b', 'c'].map(id => document.getElementById(id));
            globalThis.steal = false;
            a.addEventListener('blur', () => { if (steal) c.focus(); });
          )JS");
        // a's blur listener focuses c: b never takes the focus, a's focusout names no new element.
        f.eval("document.getElementById('a').focus(); steal = true; take()");
        f.eval("document.getElementById('b').focus()");
        require(f.eval("take()") == "blur@a(b) focus@c(null) focusin@c(null) focusout@a(null)",
                "the blur listener wins: " + f.eval("String(log)"));
        require(f.eval("document.activeElement.id") == "c", "c has the focus");
        // A keydown listener removes the focused element: Tab goes on from where it was.
        f.eval("steal = false; document.getElementById('b').focus(); take();"
               "document.getElementById('b').addEventListener('keydown', e => e.target.remove())");
        f.press("Tab", "Tab");
        require(f.eval("document.activeElement.id") == "c", "Tab after the removed element: " +
                                                             f.eval("document.activeElement.id"));
        require(f.eval("take()") == "keydown@b blur@b(null) focusout@b(null) focus@c(null) focusin@c(null) keyup@c",
                "removal blurs: " + f.eval("String(log)"));
        // Removing an ancestor of the focused element, or emptying it with textContent, blurs it first.
        f.eval("const box = document.createElement('div'); box.setAttribute('id', 'box');"
               "const inner = document.createElement('button'); inner.setAttribute('id', 'inner'); box.appendChild(inner);"
               "document.getElementById('root').appendChild(box); inner.focus(); take(); box.remove()");
        require(f.eval("take() + '|' + document.activeElement.id") == "blur@inner(null) focusout@inner(null)|",
                "an ancestor's removal");
        f.eval("document.getElementById('root').appendChild(box); inner.focus(); take(); box.textContent = ''");
        require(f.eval("take()") == "blur@inner(null) focusout@inner(null)", "textContent");
        // A focused element moved elsewhere loses the focus too.
        f.eval("box.appendChild(inner); inner.focus(); take(); document.getElementById('root').appendChild(inner)");
        require(f.eval("take() + '|' + document.activeElement.id") == "blur@inner(null) focusout@inner(null)|",
                "a moved element");
    }

    // Which elements focus() and the mouse focus: not hidden, not disabled; tabIndex.
    void focus_rules() {
        Fixture f;
        f.load(R"JS(
            const root = document.getElementById('root');
            const add = (tag, id, attributes = {}) => {
              const n = document.createElement(tag);
              n.setAttribute('id', id);
              for (const [k, v] of Object.entries(attributes)) n.setAttribute(k, v);
              n.textContent = id;
              root.appendChild(n);
              return n;
            };
            add('div', 'none', {tabindex: '0', style: 'display: none'});
            const outer = add('div', 'outer', {style: 'display: none'});
            const inner = document.createElement('button');
            inner.setAttribute('id', 'inner');
            outer.appendChild(inner);
            add('button', 'hidden', {style: 'visibility: hidden'});
            add('button', 'off', {disabled: ''});
            add('div', 'plain');
            add('a', 'bare');
            add('a', 'link', {href: '#'});
            add('div', 'spaces', {tabindex: '  3x'});
            add('div', 'word', {tabindex: 'abc'});
            add('input', 'field');
            add('input', 'secret', {type: 'hidden'});
            add('div', 'editor', {contenteditable: ''});
          )JS",
               "#root{position:relative;width:100%;height:100%}div,button,a,input{display:block;height:20px}");
        f.advance();
        for (const char* id : {"none", "inner", "hidden", "off", "plain", "bare", "word", "secret"}) {
            f.eval("document.getElementById('" + std::string(id) + "').focus()");
            require(f.eval("document.activeElement === document.body") == "true",
                    std::string(id) + " takes no focus: " + f.eval("document.activeElement.id"));
        }
        for (const char* id : {"link", "spaces", "field", "editor"}) {
            f.eval("document.getElementById('" + std::string(id) + "').focus()");
            require(f.eval("document.activeElement.id") == id, std::string(id) + " takes the focus");
        }
        require(f.eval("['link', 'spaces', 'word', 'plain', 'field', 'secret', 'off'].map(id => "
                       "document.getElementById(id).tabIndex).join()") == "0,3,-1,-1,0,-1,0",
                "tabIndex: " + f.eval("['link', 'spaces', 'word', 'plain', 'field', 'secret', 'off'].map(id => "
                                      "document.getElementById(id).tabIndex).join()"));
        f.eval("document.getElementById('plain').tabIndex = 5");
        require(f.eval("document.getElementById('plain').getAttribute('tabindex')") == "5", "tabIndex reflects");
        // An editable element shows its focus even when the mouse gave it.
        f.eval("document.getElementById('editor').blur()");
        f.advance();
        const auto centre = parse_json(f.eval("(() => { const r = document.getElementById('editor')"
                                              ".getBoundingClientRect(); return JSON.stringify([r.left + 5, r.top + 5]); })()"));
        f.click(float(centre[0].number), float(centre[1].number));
        require(f.eval("document.activeElement.id + document.activeElement.matches(':focus-visible')") == "editortrue",
                "a mouse-focused editor shows its focus");
    }

    // Pages with neither the web platform nor forgiving CSS: KeyboardEvent, FocusEvent, focus(), activeElement and
    // the keys are every page's; their DOM queries keep the strict selectors.
    void pages_without_web_platform() {
        Fixture f;
        f.load(R"JS(
            const b = document.createElement('button');
            b.setAttribute('id', 'b');
            document.getElementById('root').appendChild(b);
            globalThis.keys = [];
            document.addEventListener('keydown', e => keys.push(e.key));
          )JS",
               "#root{position:relative;width:100px;height:100px}#b{width:20px;height:20px}", false, false);
        f.advance();
        require(f.eval("typeof KeyboardEvent + typeof FocusEvent + (document.activeElement === document.body)") ==
                    "functionfunctiontrue",
                "the events and the focus without the web platform");
        f.eval("document.getElementById('b').focus()");
        f.press("q", "KeyQ", 0, "q");
        require(f.eval("keys.join() + document.activeElement.id") == "qb", "keys reach a page without the web platform");
        require(f.eval("try { document.querySelector(':focus-visible'); 'ok' } catch (e) { e.message.split(':')[0] }") ==
                    "WEFT_UNSUPPORTED_SELECTOR",
                "strict pages keep their strict DOM queries");
        require(f.eval("new KeyboardEvent('keydown', {key: 'a', keyCode: 65}).which") == "65", "a page's own event");
    }

    // Chrome's focus ring where an element shows its focus (the UA's :focus-visible { outline: auto 1px }), drawn by the
    // reference raster where Chrome draws it (measured in Chrome 155 on the same boxes): 2 px of rgb(16, 16, 16)
    // straddling the border box's edge, moved in by 1 px when every border is at least 1 px wide and out by
    // outline-offset; none where the mouse gave the focus.
    void focus_ring() {
        Fixture f;
        f.load(R"JS(
            const root = document.getElementById('root');
            const add = (tag, id, style) => {
              const n = document.createElement(tag);
              n.setAttribute('id', id);
              n.setAttribute('style', 'position: absolute; width: 60px; height: 24px; ' + style);
              if (tag === 'div') n.setAttribute('tabindex', '0');
              root.appendChild(n);
            };
            add('button', 'b', 'left: 20px; top: 20px; border: 2px solid #777777; background-color: #eeeeee');
            add('div', 'd', 'left: 120px; top: 20px; background-color: #ddddff');
            add('div', 't', 'left: 120px; top: 60px; background-color: #ddddff; border: 4px solid #777777; outline-offset: 3px');
          )JS",
               "#root{position:relative;width:100%;height:100%;background-color:#ffffff}");
        f.advance();
        weft::SceneRaster raster(f.text);
        // The red channel along the row through the box's middle, x relative to its left edge.
        const auto row = [&](float scale, float left, float top) {
            f.advance();
            const auto bitmap = raster.raster(f.view.scene(), scale);
            std::map<int, int> out;
            const auto y = unsigned((top + 12) * scale);
            for (int dx = -4; dx < 3; ++dx) {
                const auto x = unsigned(std::lround((left + dx) * scale));
                out[dx] = bitmap.rgba[(std::size_t(y) * bitmap.width + x) * 4];
            }
            return out;
        };
        const auto ring = [](const std::map<int, int>& r, int from, std::string_view what) {
            for (const auto& [dx, red] : r) {
                const bool dark = dx >= from && dx < from + 2;
                require(dark ? red <= 24 : red > 100, std::string(what) + ": " + std::to_string(red) + " at " +
                                                          std::to_string(dx) + " px from the edge");
            }
        };
        f.press("Tab", "Tab");
        ring(row(1, 20, 20), 0, "a 2 px border: the ring over it");
        ring(row(2, 20, 20), 0, "the same at scale 2");
        f.press("Tab", "Tab");
        ring(row(1, 120, 20), -1, "no border: the ring across the edge");
        f.press("Tab", "Tab");
        ring(row(1, 120, 60), -3, "a 4 px border and outline-offset 3px");
        f.click(30, 30);
        const auto clicked = row(1, 20, 20);
        require(clicked.at(0) == 0x77 && clicked.at(1) == 0x77, "no ring where the mouse gave the focus");
    }

    // The key values hosts ask weft_keyboard_key for, and keyCode and location from codes.
    void key_values() {
        require(weft::layout_key("Enter", 0) == "Enter" && weft::layout_key("ArrowLeft", 0) == "ArrowLeft" &&
                    weft::layout_key("ShiftRight", weft::key_modifier::shift) == "Shift",
                "the names of keys that type nothing");
        require(weft::layout_key("Numpad1", 0) == "End" && weft::layout_key("Numpad1", weft::key_modifier::num_lock) == "1",
                "the numeric keypad with Num Lock off and on");
        const auto a = weft::layout_key("KeyA", 0);
        require(a && !a->empty() && *a != "Dead" && *a != "Unidentified", "the layout types something with KeyA");
        require(!weft::layout_key("NoSuchKey", 0), "an unknown code");
        require(weft::windows_key_code("KeyA") == 65 && weft::windows_key_code("Semicolon") == 186 &&
                    weft::windows_key_code("NumpadEnter") == 13 && weft::windows_key_code("NoSuchKey") == 0,
                "keyCodes");
        constexpr auto num_lock = weft::key_modifier::num_lock;
        const auto a_code = weft::layout_key_code("KeyA", num_lock);
        require(weft::layout_key_code("Enter", 0) == 13 && weft::layout_key_code("ArrowLeft", 0) == 37 &&
                    weft::layout_key_code("Numpad1", num_lock) == 97 && weft::layout_key_code("Numpad1", 0) == 35 &&
                    weft::layout_key_code("NumpadAdd", 0) == 107 && weft::layout_key_code("NoSuchKey", 0) == 0 &&
                    a_code >= 'A' && a_code <= 'Z',
                "keyCodes as the system gives them");
        const auto semicolon = weft::physical_code(0xBA);
        const auto letter = weft::physical_code('A');
        require(weft::physical_code(0xA0) == "ShiftLeft" && weft::physical_code(0xA5) == "AltRight" &&
                    weft::physical_code(13) == "Enter" && weft::physical_code(97) == "Numpad1" &&
                    weft::physical_code(35) == "End" && weft::physical_code(91) == "MetaLeft" &&
                    weft::physical_code(32) == "Space" && letter && letter->starts_with("Key") && semicolon &&
                    !weft::physical_code(0xFF),
                "the physical keys of virtual keys");
        require(weft::key_location("ShiftLeft") == 1 && weft::key_location("MetaRight") == 2 &&
                    weft::key_location("Numpad0") == 3 && weft::key_location("ArrowLeft") == 0,
                "locations");
    }
} // namespace

int main() {
    try {
        // The reference raster of the focus ring needs COM.
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM init");
        key_values();
        tab_comes_round();
        host_focus();
        handled_bits();
        reentrant_listeners();
        focus_rules();
        pages_without_web_platform();
        focus_ring();
        chrome_scenarios();
        std::cout << "keyboard_contract passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
