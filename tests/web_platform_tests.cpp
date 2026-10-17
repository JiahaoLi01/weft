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

    // A forgiving page keeps the filters the scene draws (the CRPG subtitles' glow) and drops one it cannot with a
    // note; the frame still comes.
    void filters() {
        Fixture f;
        f.load(R"JS(
            const glow = document.createElement('div');
            glow.id = 'glow';
            glow.setAttribute('style', 'width:40px;height:20px;background:#335577;filter:blur(10px)');
            document.getElementById('root').appendChild(glow);
            const wide = document.createElement('div');
            wide.id = 'wide';
            wide.setAttribute('style', 'width:40px;height:20px;background:#335577;filter:blur(1em)');
            document.getElementById('root').appendChild(wide);
          )JS");
        f.view.advance(16);
        const auto d = f.diagnostics();
        require(std::any_of(d.begin(), d.end(),
                            [](const ViewDiagnostic& x) {
                                return x.kind == ViewDiagnostic::Kind::Scene &&
                                       x.message == "WEFT_STYLE_DROPPED: filter blur(1em) on <div> (blur() takes a "
                                                    "length of 0 to 256px)";
                            }),
                "the filter the scene cannot draw dropped with a note");
        const auto* glow = find(f.view.scene().root, "glow");
        const auto* wide = find(f.view.scene().root, "wide");
        require(glow && glow->style.text(weft::PropertyId::Filter, "none") == "blur(10px)" && wide &&
                    !wide->style.contains(weft::PropertyId::Filter) && f.view.shared()->frame(),
                "the glow keeps its blur, the other element loses its filter, in a frame");
    }

    // CSS Grid on a forgiving page: the CRPG Pick 17 draft's automatic repetition (inline styles, as React sets them),
    // whose cards stretch to their row, and the Market's auto and fr columns (Tailwind classes). A grid value the
    // layout does not take is dropped with a note and the frame still comes; a strict page keeps its diagnostic and its
    // last frame.
    void grids() {
        const std::string script = R"JS(
            const root = document.getElementById('root');
            const draft = document.createElement('div');
            draft.id = 'draft';
            Object.assign(draft.style, {display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(320px, 1fr))',
                                        gap: '8px', width: '1000px'});
            for (let i = 0; i < 4; ++i) {
              const card = document.createElement('div');
              card.id = 'card' + i;
              if (i == 1) card.style.height = '50px';
              draft.appendChild(card);
            }
            root.appendChild(draft);
            const market = document.createElement('div');
            market.id = 'market';
            market.className = 'market';
            for (const [id, width] of [['day', 30], ['side', 12], ['shares', 0], ['price', 56]]) {
              const cell = document.createElement('div');
              cell.id = id;
              if (width) cell.style.width = width + 'px';
              cell.style.height = '10px';
              market.appendChild(cell);
            }
            root.appendChild(market);
            const named = document.createElement('div');
            named.id = 'named';
            named.setAttribute('style', 'display:grid;grid-template-columns:100px 100px;width:200px');
            named.innerHTML = '<div id="first" style="height:10px"></div>' +
                              '<div id="header" style="grid-column:header;height:10px"></div>';
            root.appendChild(named);
            globalThis.box = id => {
              const r = document.getElementById(id).getBoundingClientRect();
              return JSON.stringify([r.x, r.y, r.width, r.height]);
            };
          )JS";
        const std::string sheet = ".market{display:grid;grid-template-columns:auto auto 1fr auto;column-gap:8px;"
                                  "width:400px}";
        Fixture f;
        f.load(script, sheet);
        f.view.advance(16);
        const auto d = f.diagnostics();
        const auto noted = [&](const std::string& message) {
            return std::any_of(d.begin(), d.end(), [&](const ViewDiagnostic& x) {
                return x.kind == ViewDiagnostic::Kind::Scene && x.message == message;
            });
        };
        require(noted("WEFT_STYLE_DROPPED: grid-column-start header on <div> (named lines and areas are outside the "
                      "profile)") &&
                    noted("WEFT_STYLE_DROPPED: grid-column-end header on <div> (named lines and areas are outside the "
                          "profile)"),
                "a named line dropped with a note");
        require(f.view.shared()->frame() != nullptr, "the frame comes");
        // Three 328px columns fit 1000px with 8px gaps; the cards of auto height stretch to the 50px row.
        const auto top = f.eval("document.getElementById('draft').getBoundingClientRect().y");
        require(f.eval("box('card0')") == "[0," + top + ",328,50]" &&
                    f.eval("box('card1')") == "[336," + top + ",328,50]" &&
                    f.eval("box('card2')") == "[672," + top + ",328,50]",
                "the draft's first row: " + f.eval("box('card0')") + f.eval("box('card1')") + f.eval("box('card2')"));
        require(f.eval("box('card3').split(',')[2]") == "328" &&
                    f.eval("document.getElementById('card3').getBoundingClientRect().y - " + top) == "58",
                "the fourth card on the second row");
        // The Market: two auto columns (30px, 12px), a fr column taking the rest, an auto column of 56px.
        require(f.eval("['day','side','shares','price'].map(id => document.getElementById(id).getBoundingClientRect())"
                       ".map(r => r.x + ':' + r.width).join(' ')") == "0:30 38:12 58:278 344:56",
                "the Market's columns");
        // The named line is gone: the item is placed automatically, after the first.
        require(f.eval("document.getElementById('header').getBoundingClientRect().x") == "100", "automatic placement");
        // A strict page keeps the diagnostic.
        Fixture strict;
        strict.load(R"JS(
            const named = document.createElement('div');
            named.setAttribute('style', 'display:grid;grid-template-columns:100px 100px;width:200px');
            const item = document.createElement('div');
            item.setAttribute('style', 'grid-column:header;height:10px');
            named.appendChild(item);
            document.getElementById('root').appendChild(named);
          )JS",
                    {}, false);
        strict.view.advance(16);
        bool failed{};
        for (const auto& x : strict.diagnostics()) {
            failed |= x.message.find("WEFT_UNSUPPORTED_FORMAT: grid-column-start header") != std::string::npos;
        }
        require(failed, "a strict page's grid value outside the profile is a diagnostic");
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
        grids();
        transforms();
        numbers();
        std::cout << "web platform tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "web platform tests failed: " << e.what() << '\n';
        return 1;
    }
}
