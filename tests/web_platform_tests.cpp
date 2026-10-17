#define NOMINMAX
#include "view/view.hpp"
#include "support/check.hpp"
#include "support/scene_fonts.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// The web platform module (WEFT_PAGE_WEB_PLATFORM, src/script/js/web_platform_bootstrap.js): the console as
// diagnostics, localStorage and sessionStorage over the View's areas across loads, dataset, document.head,
// KeyboardEvent, focus with :focus and :focus-within, matchMedia as @media, IntersectionObserver and toLocaleString;
// and pages without it as before.
namespace {
    using weft::ViewDiagnostic;
    using weft::testing::require;

    struct Fixture {
        std::shared_ptr<const weft::FontCatalog> fonts =
            std::make_shared<weft::FontCatalog>(weft::testing::chrome_windows_font_catalog());
        weft::SceneText text{*fonts};
        weft::View view{fonts, text, 1280, 720, 1};

        void load(std::string script, std::string stylesheet = {}, bool web = true) {
            weft::ViewPage page;
            page.scripts.push_back({"page.js", std::move(script)});
            page.stylesheet = "#root{position:relative;width:100%;height:100%}" + stylesheet;
            page.web_platform = web;
            page.forgiving_css = web;
            view.load(page);
        }

        // String() of an expression in the page; throws when it threw.
        std::string eval(const std::string& source) {
            const auto [ok, result] = view.evaluate(source);
            require(ok, "evaluates: " + source + " (" + result + ")");
            return result;
        }

        std::vector<ViewDiagnostic> diagnostics() {
            return view.take_diagnostics();
        }
    };

    const weft::SceneNode* find(const weft::SceneNode& n, std::string_view id) {
        if (const auto it = n.attributes.find("id"); it != n.attributes.end() && it->second == id) {
            return &n;
        }
        for (const auto& c : n.children) {
            if (const auto* found = find(c, id)) {
                return found;
            }
        }
        return nullptr;
    }

    void console() {
        Fixture f;
        f.load(R"JS(
            console.log('a', 1, {b: 2, c: [1, 'x']});
            console.info('%s has %d items%c', 'list', 3.7, 'color: red');
            console.warn('careful');
            console.error(new Error('boom'));
            console.assert(1 === 2, 'math');
            console.assert(true, 'never');
            const cycle = {}; cycle.self = cycle;
            console.debug(cycle);
          )JS");
        const auto d = f.diagnostics();
        require(d.size() == 6, "six console messages, nothing thrown (got " + std::to_string(d.size()) + ")");
        require(d[0].kind == ViewDiagnostic::Kind::Console && d[0].message == "a 1 {b: 2, c: [1, \"x\"]}",
                "log: " + d[0].message);
        require(d[1].kind == ViewDiagnostic::Kind::Console && d[1].message == "list has 3 items", "substitutions");
        require(d[2].kind == ViewDiagnostic::Kind::ConsoleWarning && d[2].message == "careful", "warn");
        require(d[3].kind == ViewDiagnostic::Kind::ConsoleError && d[3].message.starts_with("Error: boom"), "error");
        require(d[4].kind == ViewDiagnostic::Kind::ConsoleError && d[4].message == "Assertion failed: math", "assert");
        require(d[5].kind == ViewDiagnostic::Kind::Console && d[5].message == "{self: [\xE2\x80\xA6]}", "a cycle");

        // Without the web platform console.error stays an error of the page.
        f.load("console.error('strict'); console.log('quiet');", {}, false);
        f.view.advance(16);
        const auto strict = f.diagnostics();
        require(strict.size() == 1 && strict[0].kind == ViewDiagnostic::Kind::Script &&
                    strict[0].message.find("WEFT_CONSOLE_ERROR: strict") != std::string::npos,
                "strict console.error");
        require(f.eval("typeof localStorage") == "undefined", "no storage without the web platform");
    }

    void storage() {
        Fixture f;
        auto local = std::make_shared<weft::WebStorage>();
        local->set("restored", "yes");
        f.view.set_local_storage(local);
        const auto before = local->revision();
        f.load(R"JS(
            localStorage.setItem('k', 'v');
            localStorage.n = 5;
            sessionStorage.setItem('s', '1');
          )JS");
        require(local->get("k") == "v" && local->get("n") == "5" && local->revision() > before,
                "writes reach the area");
        require(f.eval("localStorage.getItem('restored')") == "yes", "the host's items");
        require(f.eval("[localStorage.length, localStorage.key(0), localStorage.key(9), Object.keys(localStorage)]"
                       ".join('|')") == "3|k||k,n,restored",
                "length, key and enumeration");
        // join() writes undefined and null as empty strings.
        require(f.eval("[localStorage.missing, localStorage.getItem('missing'), 'n' in localStorage, "
                       "'getItem' in localStorage].join('|')") == "||true|true",
                "absent items");
        require(f.eval("String(localStorage.getItem('missing'))") == "null", "getItem of an absent key is null");

        // The areas outlive the page: the next load reads both.
        f.load("weft.engine.send('read', localStorage.getItem('k') + sessionStorage.getItem('s'));");
        const auto messages = f.view.take_messages();
        require(messages.size() == 1 && messages[0].name == "read", "the next page reads");
        require(f.eval("localStorage.k + sessionStorage.s") == "v1", "kept across loads");

        require(f.eval(R"JS(
            try { localStorage.setItem('big', 'x'.repeat(6 * 1024 * 1024)); 'stored' }
            catch (e) { e.name + ':' + (e instanceof DOMException) }
          )JS") == "QuotaExceededError:true",
                "past the quota");
        require(!local->get("big"), "an item past the quota is not kept");
        f.eval("localStorage.removeItem('k'); delete localStorage.n;");
        require(!local->get("k") && !local->get("n") && local->get("restored"), "removed");
        f.eval("localStorage.clear()");
        require(local->size() == 0, "cleared");
    }

    void documents() {
        Fixture f;
        f.load(R"JS(
            const el = document.createElement('div');
            el.setAttribute('id', 'el');
            el.dataset.fooBar = 'x';
            document.getElementById('root').appendChild(el);
            const style = document.createElement('style');
            style.textContent = '#el { color: red }';
            document.head.appendChild(style);
            document.title = 'Title';
          )JS");
        require(f.eval("document.getElementById('el').getAttribute('data-foo-bar')") == "x", "dataset writes data-");
        require(f.eval("[el.dataset.fooBar, 'fooBar' in el.dataset, el.dataset.other].join('|')") == "x|true|",
                "dataset reads");
        f.eval("delete el.dataset.fooBar");
        require(f.eval("el.hasAttribute('data-foo-bar')") == "false", "dataset deletes");
        require(f.eval("document.head === document.head && document.head.parentNode === document.documentElement && "
                       "document.head.nextSibling === document.body") == "true",
                "one head before the body");
        require(f.eval("document.title") == "Title", "title");
        f.view.advance(16);
        require(find(f.view.scene().root, "el") && !find(f.view.scene().root, "nothing"), "the scene has the element");
        f.eval("el.append('a', document.createElement('b')); el.prepend('z');");
        require(f.eval("el.childNodes.length + ':' + el.firstChild.textContent") == "3:z", "append and prepend");
        f.eval("el.replaceChildren('only')");
        require(f.eval("el.childNodes.length + ':' + el.textContent") == "1:only", "replaceChildren");
        require(f.eval("el.toggleAttribute('hidden') + ':' + el.hasAttribute('hidden')") == "true:true", "toggle");
    }

    void keys_and_focus() {
        Fixture f;
        f.load(R"JS(
            const root = document.getElementById('root');
            root.innerHTML = '<div id="wrap"><div id="b" class="b"></div><div id="t" class="t"></div></div><div id="d"></div>';
            document.getElementById('b').setAttribute('tabindex', '0');
            document.getElementById('t').setAttribute('tabindex', '-1');
            globalThis.log = [];
            for (const type of ['focus', 'blur', 'focusin', 'focusout'])
              document.addEventListener(type, e => log.push(type + ':' + e.target.id + ':' + (e.relatedTarget?.id ?? '')), true);
            window.addEventListener('keydown', e => log.push('key:' + e.key + ':' + e.target.id + ':' + e.keyCode));
          )JS",
               "#wrap{width:200px;height:20px}#wrap:focus-within{height:40px}#b,#t,#d{width:10px;height:10px}"
               "#t:focus{width:50px}");
        require(f.eval("document.activeElement === document.body") == "true", "the body before any focus");
        f.eval("document.getElementById('d').focus()");
        require(f.eval("document.activeElement === document.body && log.length === 0") == "true",
                "an element that takes no focus");
        f.eval("document.getElementById('b').focus()");
        require(f.eval("document.activeElement.id + '|' + log.join(',')") == "b|focus:b:,focusin:b:",
                "focus and focusin");
        f.eval("log.length = 0; document.getElementById('t').focus()");
        require(f.eval("log.join(',')") == "blur:b:t,focusout:b:t,focus:t:b,focusin:t:b", "the focus moves");
        f.view.advance(16);
        const auto* wrap = find(f.view.scene().root, "wrap");
        const auto* t = find(f.view.scene().root, "t");
        require(wrap && wrap->box[3] == 40 && t && t->box[2] == 50, ":focus and :focus-within");
        require(f.view.script().focused().has_value(), "the VM knows the focus");

        f.eval("log.length = 0; document.activeElement.dispatchEvent(new KeyboardEvent('keydown', {key: 'ArrowLeft', "
               "bubbles: true}))");
        require(f.eval("log.join(',')") == "key:ArrowLeft:t:0", "a key event bubbles to the window");
        f.eval("document.getElementById('t').blur()");
        f.view.advance(32);
        wrap = find(f.view.scene().root, "wrap");
        require(f.eval("document.activeElement === document.body") == "true" && wrap && wrap->box[3] == 20, "blur");
        f.eval("document.getElementById('b').focus(); document.getElementById('b').remove()");
        require(f.eval("document.activeElement === document.body") == "true", "a removed element loses the focus");
    }

    void media_and_observers() {
        Fixture f;
        f.load(R"JS(
            const root = document.getElementById('root');
            root.innerHTML = '<div id="in" style="position:absolute;left:10px;top:10px;width:20px;height:20px"></div>' +
                             '<div id="out" style="position:absolute;left:-500px;top:10px;width:20px;height:20px"></div>';
            globalThis.seen = [];
            const o = new IntersectionObserver(entries => {
              for (const e of entries) seen.push(e.target.id + ':' + e.isIntersecting + ':' + e.intersectionRatio);
            });
            o.observe(document.getElementById('in'));
            o.observe(document.getElementById('out'));
            globalThis.wide = matchMedia('(min-width: 1000px)');
          )JS");
        require(f.eval("[wide.matches, matchMedia('(max-width: 600px)').matches, "
                       "matchMedia('(prefers-reduced-motion: reduce)').matches, matchMedia('screen and (hover: "
                       "hover)').matches, matchMedia('nonsense ((').matches, matchMedia('').matches].join('|')") ==
                    "true|false|false|true|false|true",
                "media queries as @media");
        f.view.resize(500, 400, 1);
        require(f.eval("wide.matches + '|' + matchMedia('(max-width: 600px)').matches") == "false|true",
                "after a resize");
        f.view.advance(16);
        f.view.advance(32);
        require(f.eval("seen.sort().join(',')") == "in:true:1,out:false:0", "intersections");
        f.eval("seen.length = 0; document.getElementById('out').style.left = '100px'");
        f.view.advance(48);
        f.view.advance(64);
        require(f.eval("seen.join(',')") == "out:true:1", "an element that came into view");
    }

    // A forgiving page's filter the scene cannot draw is dropped with a note; the frame still comes.
    void filters() {
        Fixture f;
        f.load(R"JS(
            const glow = document.createElement('div');
            glow.id = 'glow';
            glow.setAttribute('style', 'width:40px;height:20px;background:#335577;filter:blur(10px)');
            document.getElementById('root').appendChild(glow);
          )JS");
        f.view.advance(16);
        const auto d = f.diagnostics();
        require(std::any_of(d.begin(), d.end(),
                            [](const ViewDiagnostic& x) {
                                return x.kind == ViewDiagnostic::Kind::Scene &&
                                       x.message == "WEFT_STYLE_DROPPED: filter blur(10px) on <div> (the scene draws "
                                                    "one drop-shadow() of an svg element)";
                            }),
                "the filter dropped with a note");
        const auto* glow = find(f.view.scene().root, "glow");
        require(glow && !glow->style.contains(weft::PropertyId::Filter) && f.view.shared()->frame(),
                "the element without it, in a frame");
    }

    // translate, rotate and scale apply before transform, in that order (CSS Transforms 2).
    void transforms() {
        Fixture f;
        f.load(R"JS(
            const box = document.createElement('div');
            box.id = 'box';
            box.setAttribute('style', 'position:absolute;left:100px;top:100px;width:40px;height:20px;' +
                                      'translate:-50% 10px;scale:200% 1;transform-origin:0 0');
            document.getElementById('root').appendChild(box);
          )JS");
        f.view.advance(16);
        const auto* box = find(f.view.scene().root, "box");
        require(box && box->style.text(weft::PropertyId::Transform, "none") == "translate(-50%, 10px) scale(2, 1)",
                "folded into transform: " + std::string(box ? box->style.text(weft::PropertyId::Transform, "") : ""));
        require(f.eval("JSON.stringify(Object.values(box.getBoundingClientRect().toJSON ? box.getBoundingClientRect()"
                       ".toJSON() : box.getBoundingClientRect()).slice(0, 4))") == "[80,110,80,20]",
                "the box where translate then scale put it");
    }

    void numbers() {
        Fixture f;
        f.load("");
        require(f.eval("(1234567.891).toLocaleString()") == "1,234,567.891", "grouping");
        require(f.eval("(12.5).toLocaleString('en-US', {minimumFractionDigits: 2})") == "12.50", "minimum digits");
        require(f.eval("(2/3).toLocaleString(undefined, {maximumFractionDigits: 1})") == "0.7", "maximum digits");
        require(f.eval("(0.256).toLocaleString(undefined, {style: 'percent'})") == "26%", "percent");
        require(f.eval("(-1500).toLocaleString()") == "-1,500", "negative");
        require(f.eval("(1500).toLocaleString('en-US', {useGrouping: false})") == "1500", "without grouping");
    }
} // namespace

int main() {
    try {
        console();
        storage();
        documents();
        keys_and_focus();
        media_and_observers();
        filters();
        transforms();
        numbers();
        std::cout << "web platform tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "web platform tests failed: " << e.what() << '\n';
        return 1;
    }
}
