#include "script/script_dom.hpp"
#include "script/script_platform.hpp"
#include "support/check.hpp"
#include "test_constants/script.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

// An element's inline style declaration (src/script/js/web_host_bootstrap.js): what element.style's property writes
// and reads, setProperty, getPropertyValue, removeProperty and cssText do to the style attribute, against the
// reference the declaration was first written as in JS (kept here verbatim): every write re-parses the attribute into
// declarations split at ';' (each name and value trimmed as String.prototype.trim does, a later name updating the
// earlier in place), sets or drops the one written (its value trimmed, ignored when its parentheses or quotes do not
// balance) and writes the attribute back as "name: value;" joined by spaces. Random operations on several elements
// over values with JS whitespace, quotes, parentheses, semicolons and non-ASCII text, the attribute and every read
// compared after each.
namespace {
    using weft::testing::require;

    constexpr const char* reference = R"JS(
globalThis.__reference = (() => {
  const cssName = name => name.startsWith('--') ? name : name.replace(/[A-Z]/g, c => '-' + c.toLowerCase());
  const declarations = text => {
    const result = new Map();
    for (const part of (text || '').split(';')) {
      const colon = part.indexOf(':');
      if (colon > 0) result.set(part.slice(0, colon).trim(), part.slice(colon + 1).trim());
    }
    return result;
  };
  // The attribute after writing name (already a CSS name) with value; null: the write changes nothing.
  const write = (text, name, value, priority = '') => {
    if (priority) throw new Error('WEFT_CSSOM_PROFILE: important unsupported in inline probe');
    value=String(value).trim();let depth=0,quote='';for(const c of value){if(quote){if(c===quote)quote='';}else if(c==='"'||c==="'")quote=c;else if(c==='(')++depth;else if(c===')'&&--depth<0)return text;}if(depth||quote)return text;
    const items = declarations(text);
    if (String(value).trim()) items.set(name, String(value).trim()); else items.delete(name);
    return Array.from(items, ([k, v]) => k + ': ' + v + ';').join(' ');
  };
  const read = (text, name) => declarations(text).get(name) || '';
  return { cssName, write, read };
})();
)JS";

    // The random run: per step one operation on one of the elements, then the attribute and reads of every name
    // compared with the reference's. Returns "ok", or the first difference.
    constexpr const char* run = R"JS(
(() => {
  const ref = globalThis.__reference;
  let seed = SEED >>> 0;
  const next = n => { seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0; return seed % n; };
  const pick = list => list[next(list.length)];
  const names = ['width', 'height', 'color', 'backgroundColor', 'marginLeft', '--gap', '--Accent-Color', 'transform',
                 'opacity', 'zIndex', 'fontFamily'];
  const rawNames = ['width', ' width ', 'background-color', '--gap', 'COLOR', 'x:y', '', ' ', 'a b'];
  const values = ['', ' ', '10px', ' 10px ', ' 10px　', '﻿5 ', '\t1\n', 'red', 'rgba(1, 2, 3, 0.5)',
                  'calc(1px + 2px)', 'calc(1px', 'a)', '((x))', ')(', '"x;y"', "'a(b'", "url('a(b')", '"', "'",
                  'é', '中文 字体', '\u{1F600}', 'a;b', 'k: v', '    ', 12, 0, -1.5, null, undefined, true,
                  ' x ', '1px !important'];
  const texts = ['', 'a:1;a:2', ' : x', 'b', ';;', 'c: ', 'd :  e ; f:g:h', 'width: 1px; height: 2px;',
                 'color:red;COLOR:blue', ' width : 3px', '--x: (;', 'a: "b;c"; d: e'];
  const elements = [];
  for (let i = 0; i < 4; i++) {
    const e = document.createElement('div');
    document.body.appendChild(e);
    elements.push({ e, text: null });
  }
  const attribute = s => s.e.getAttribute('style');
  const check = (s, step, what) => {
    const got = attribute(s);
    if (got !== s.text) return step + ' ' + what + ': attribute ' + JSON.stringify(got) + ', reference ' + JSON.stringify(s.text);
    for (const name of names) {
      const css = ref.cssName(name);
      const want = ref.read(s.text, css);
      if (s.e.style[name] !== want) return step + ' ' + what + ': style.' + name + ' ' + JSON.stringify(s.e.style[name]) + ', reference ' + JSON.stringify(want);
      if (s.e.style.getPropertyValue(css) !== want) return step + ' ' + what + ': getPropertyValue(' + css + ')';
    }
    for (const raw of rawNames) {
      if (s.e.style.getPropertyValue(raw) !== ref.read(s.text, raw)) return step + ' ' + what + ': getPropertyValue(' + JSON.stringify(raw) + ')';
    }
    if (s.e.style.cssText !== (s.text || '')) return step + ' ' + what + ': cssText';
    return '';
  };
  for (let step = 0; step < STEPS; step++) {
    const s = pick(elements);
    const op = next(7);
    let what;
    if (op === 0 || op === 1) {
      const name = pick(names), value = pick(values);
      what = 'style.' + name + ' = ' + JSON.stringify(value);
      s.e.style[name] = value;
      s.text = ref.write(s.text, ref.cssName(name), value);
    } else if (op === 2) {
      const name = pick(rawNames), value = pick(values);
      what = 'setProperty(' + JSON.stringify(name) + ', ' + JSON.stringify(value) + ')';
      s.e.style.setProperty(name, value);
      s.text = ref.write(s.text, name, value);
    } else if (op === 3) {
      const name = next(2) ? pick(rawNames) : ref.cssName(pick(names));
      what = 'removeProperty(' + JSON.stringify(name) + ')';
      const old = s.e.style.removeProperty(name);
      const want = ref.read(s.text, name);
      if (old !== want) return step + ' ' + what + ': returned ' + JSON.stringify(old) + ', reference ' + JSON.stringify(want);
      s.text = ref.write(s.text, name, '');
    } else if (op === 4) {
      const text = pick(texts);
      what = 'cssText = ' + JSON.stringify(text);
      s.e.style.cssText = text;
      s.text = text;
    } else if (op === 5) {
      const text = pick(texts);
      what = 'setAttribute(style, ' + JSON.stringify(text) + ')';
      s.e.setAttribute('style', text);
      s.text = text;
    } else {
      what = 'setProperty with priority';
      let threw = false;
      try { s.e.style.setProperty('width', '1px', 'important'); } catch (e) { threw = String(e.message).includes('WEFT_CSSOM_PROFILE'); }
      if (!threw) return step + ' ' + what + ': no profile error';
    }
    const difference = check(s, step, what);
    if (difference) return difference;
  }
  return 'ok';
})()
)JS";

    std::string replace(std::string text, const std::string& from, const std::string& to) {
        text.replace(text.find(from), from.size(), to);
        return text;
    }

    void MatchesTheReference() {
        weft::ScriptDom page;
        page.eval(weft::script_platform_source(weft::ScriptPlatformModule::WebHost));
        page.eval(reference);
        for (unsigned seed = 1; seed <= weft::test_constants::script::inline_style_seeds; ++seed) {
            auto source = replace(run, "SEED", std::to_string(seed * 2654435761u));
            source = replace(source, "STEPS", std::to_string(weft::test_constants::script::inline_style_steps));
            const auto result = page.eval(source);
            require(result == "ok", "seed " + std::to_string(seed) + ": " + result);
        }
        // An element's declaration lives as long as the element; a text node has none.
        require(page.eval("document.createTextNode('x').style === undefined") == "true", "a text node's style");
    }

    // Many writes of a few properties on many elements, as a page's per-frame update does: the time per write (for the
    // log; the release build's figure is in docs/status.md).
    void WriteCost() {
        weft::ScriptDom page;
        page.eval(weft::script_platform_source(weft::ScriptPlatformModule::WebHost));
        const auto start = std::chrono::steady_clock::now();
        const auto writes = page.eval(R"JS(
(() => {
  const cols = [];
  for (let i = 0; i < 16; i++) { const e = document.createElement('div'); e.style.width = '10px'; e.style.borderRadius = '3px'; document.body.appendChild(e); cols.push(e); }
  let n = 0;
  for (let frame = 0; frame < 2000; frame++) for (let i = 0; i < cols.length; i++) { cols[i].style.height = ((frame + i) % 100) + '%'; ++n; }
  return n;
})()
)JS");
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::cout << "inline style: " << writes << " property writes in " << ms << " ms ("
                  << ms * 1000 / std::stod(writes) << " us each)\n";
    }
} // namespace

int main() {
    try {
        MatchesTheReference();
        WriteCost();
        std::cout << "PASS inline style: element.style writes and reads match the reference declaration\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
