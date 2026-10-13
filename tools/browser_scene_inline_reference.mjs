// Chrome oracle for scene inline formatting contexts (src/scene/scene_inline.cpp): lays out small trees of text,
// inline elements, <br> and atomic inlines in a block of a given width, then records the block's height, every
// element's client rects (an inline element's fragments) and, for every text node, the text on each line with its
// horizontal extent and the top of its glyphs' content area. Offline: the page is a data URL; fonts are this
// machine's installed fonts, as for the scene text references.
//
//   node tools/browser_scene_inline_reference.mjs [output.json] [chrome.exe]
import {spawn} from 'node:child_process';
import {writeFileSync, mkdtempSync, existsSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {resolve, join} from 'node:path';
import {repo} from './gallery_runtime.mjs';

const output = resolve(process.argv[2] ?? resolve(repo, 'tests/fixtures/scene_inline_reference.json'));
const executable = process.argv[3] ?? 'C:/Program Files/Google/Chrome/Application/chrome.exe';
if (!existsSync(executable)) throw new Error('Provide a local Chromium executable as the second argument');

// A case: the block's width and style, and its children: a string is a text node, an object an element.
const el = (tag, style, ...children) => ({tag, style, children});
const span = (style, ...children) => el('span', style, ...children);
const keyword = (text) => span('font-weight:600;border-bottom-width:1px;border-bottom-style:solid;border-bottom-color:currentColor', text);
const sans = 'font-family:system-ui;';
const tailwind = 'font-family:ui-sans-serif, system-ui, sans-serif, "Apple Color Emoji", "Segoe UI Emoji", ' +
  '"Segoe UI Symbol", "Noto Color Emoji";';
const cases = [
  {name: 'paragraph', width: 200, style: sans + 'font-size:14px;line-height:normal',
    children: ['Hover keywords for tooltips - hold 3s to pin as a floating window, then drag it anywhere you like.']},
  {name: 'paragraph narrow', width: 97, style: sans + 'font-size:16px;line-height:1.5',
    children: ['Elemental damage that ignites targets and can trigger burning.']},
  {name: 'rich text', width: 286, style: tailwind + 'font-size:14px;line-height:1.625',
    children: [span('', 'Deals '), keyword('Fire Damage'), span('', ' on hit. Critical strikes apply '), keyword('Burn'),
      span('', ' for 3 seconds. Pairs well with '), keyword('Ember Amulet'), span('', '.')]},
  {name: 'rich text small', width: 230, style: tailwind + 'font-size:12px;line-height:1.625',
    children: [span('', 'Consumes 45 '), keyword('Mana'), span('', ' to deal '), keyword('Arcane Damage'),
      span('', '. Can be combo-cast after '), keyword('Fireball'), span('', ' for '), keyword('Elemental Reaction'),
      span('', '.')]},
  {name: 'status header', width: 230, style: tailwind + 'font-size:14px;line-height:20px;font-weight:700',
    children: [span('margin-right:4px', '\u{1F525}'), 'Burn']},
  {name: 'nested boxes', width: 120, style: sans + 'font-size:15px;line-height:1.4',
    children: ['a ', span('padding:0 6px;border:1px solid #888;margin:0 3px', 'b ', el('b', '', 'bold text that wraps across lines'),
      ' c'), ' d']},
  {name: 'spaces', width: 300, style: sans + 'font-size:14px',
    children: ['  leading   and ', span('', 'a'), ' ', span('', 'b'), '   ', span('', ' c '), '  trailing  ']},
  {name: 'breaks', width: 300, style: sans + 'font-size:14px;line-height:20px',
    children: ['first line ', el('br', ''), '  second', el('br', ''), el('br', ''), 'fourth']},
  {name: 'long word', width: 80, style: sans + 'font-size:14px',
    children: ['Supercalifragilisticexpialidocious and more']},
  {name: 'hyphens', width: 60, style: sans + 'font-size:14px',
    children: ['self-contained well-known state-of-the-art']},
  {name: 'centered', width: 150, style: sans + 'font-size:13px;line-height:1.5;text-align:center',
    children: ['Blocks the next attack and reflects stuns back to the attacker.']},
  {name: 'inherited align', width: 220, style: sans + 'font-size:13px;text-align:center',
    children: [el('div', 'padding:0 10px', 'A block inside inherits the centering of its parent')]},
  {name: 'anonymous flex item', width: 200,
    style: sans + 'display:flex;flex-direction:column;text-align:center;font-size:13px',
    children: ['Text beside an element in a column flex container wraps here', span('', 'span')]},
  {name: 'right', width: 150, style: sans + 'font-size:13px;line-height:1.5;text-align:right',
    children: ['Blocks the next attack and reflects stuns back to the attacker.']},
  {name: 'letter spacing', width: 100, style: tailwind + 'font-size:10px;letter-spacing:0.05em;line-height:1.5',
    children: ['RARE EPIC LEGENDARY COMMON SPECIAL']},
  {name: 'inline blocks', width: 300, style: sans + 'font-size:24px;line-height:32px;font-weight:700',
    children: [...'Purple Scheme'].map((c) => span('display:inline-block', c === ' ' ? '\u00a0' : c))},
  {name: 'inline svg', width: 200, style: sans + 'font-size:16px;line-height:1.5',
    children: [el('svg', '', ), ' after the ring']},
  {name: 'cjk', width: 120, style: 'font-family:sans-serif;font-size:14px;line-height:1.5',
    children: ['进度 42% 战斗开始了，这是一段很长的中文文本，用来测试换行。']},
  {name: 'mixed sizes', width: 300, style: sans + 'font-size:12px;line-height:normal',
    children: ['small ', span('font-size:24px', 'BIG'), ' small ', span('font-size:8px;line-height:30px', 'tall'), ' end']},
  {name: 'box strut', width: 300, style: sans + 'font-size:12px;line-height:14px',
    children: ['plain ', span('font-size:20px;line-height:40px', span('font-size:12px;line-height:12px', 'inner')),
      ' plain']},
  {name: 'nowrap', width: 90, style: sans + 'font-size:14px',
    children: ['aaa bbb ', span('white-space:nowrap', 'ccc ddd eee'), ' fff ggg']},
  {name: 'fallback faces', width: 300, style: sans + 'font-size:14px;line-height:normal',
    children: ['Fire \u{1F525} and 进度 42% with fallback faces in a normal line height']},
  {name: 'emoji', width: 120, style: tailwind + 'font-size:14px;line-height:1.625',
    children: ['Fire \u{1F525} and ice \u2744\uFE0F and shield \u{1F6E1} and dagger \u{1F5E1} here']},
  // text-transform: full case mappings (\u00DF, the \uFB01 ligature and \u0130 become two characters).
  {name: 'uppercase', width: 260, style: sans + 'font-size:14px;line-height:1.5',
    children: ['Rarity: ', span('text-transform:uppercase;letter-spacing:0.05em', 'legendary stra\u00DFe \uFB01nale \u00E9clair'),
      ' then ', span('text-transform:lowercase', 'LOUD \u00C9CLAT \u0130STANBUL'), ' end.']},
  {name: 'uppercase block', width: 120, style: tailwind + 'font-size:9px;text-transform:uppercase;letter-spacing:0.05em',
    children: ['Rare item with a long name that wraps', span('text-transform:none', ' kept')]},
  // font-style: the face's italics, or a slanted upright face where it has none (Impact).
  {name: 'italic', width: 180, style: sans + 'font-size:14px',
    children: ['Upright then ', span('font-style:italic', 'italic words that wrap onto the next line'), ' and upright.']},
  {name: 'synthetic oblique', width: 300, style: 'font-family:Impact;font-size:16px',
    children: ['Upright ', span('font-style:italic', 'Slanted text'), ' end']},
  // font-variant-numeric: Constantia's figures are oldstyle and proportional by default.
  {name: 'numeric variants', width: 300, style: 'font-family:Constantia;font-size:16px;line-height:24px',
    children: ['1111 7777 0000', el('br', ''), span('font-variant-numeric:tabular-nums', '1111 7777 0000'), el('br', ''),
      span('font-variant-numeric:lining-nums tabular-nums', '1111 7777 0000'), el('br', ''),
      span('font-variant-numeric:oldstyle-nums proportional-nums slashed-zero', '1111 7777 0000')]},
  {name: 'tabular nums', width: 300, style: tailwind + 'font-size:12px',
    children: ['Damage 1,234 / ', span('font-variant-numeric:tabular-nums', '1,111 / 9,999 / 1.0s')]},
];
// The svg case: a 20 x 20 replaced inline element on the baseline.
cases.find((c) => c.name === 'inline svg').children[0] = {tag: 'svg', style: '', attributes: {width: '20', height: '20'},
  children: []};

const escape = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/"/g, '&quot;');
const html = (n) => {
  if (typeof n === 'string') return escape(n);
  const attributes = Object.entries(n.attributes ?? {}).map(([k, v]) => ` ${k}="${escape(v)}"`).join('');
  if (n.tag === 'br') return '<br>';
  return `<${n.tag} style="${escape(n.style)}"${attributes}>${n.children.map(html).join('')}</${n.tag}>`;
};
// Each case's block at the top left of its own positioned row; the browser's own UA sheet with Tailwind's preflight
// box model, as the scene's UA sheet has it.
const page = '<!doctype html><html><head><meta charset=utf-8><style>*{box-sizing:border-box;border:0 solid;margin:0;' +
  'padding:0}body{margin:0}.row{position:relative;height:400px}svg{display:inline}</style></head><body>' +
  cases.map((c, i) => `<div class=row><div id=case${i} style="width:${c.width}px;${escape(c.style)}">` +
    c.children.map(html).join('') + '</div></div>').join('') + '</body></html>';

const measure = `(() => {
  const out = [];
  for (let i = 0; ; ++i) {
    const root = document.getElementById('case' + i);
    if (!root) break;
    const origin = root.getBoundingClientRect();
    const rel = (r) => [r.left - origin.left, r.top - origin.top, r.width, r.height];
    const elements = [];
    const texts = [];
    const lines = (node) => {
      const range = document.createRange();
      const chars = [];
      const data = node.data;
      for (let k = 0; k < data.length;) {
        const size = data.codePointAt(k) > 0xffff ? 2 : 1;
        range.setStart(node, k);
        range.setEnd(node, k + size);
        const rects = [...range.getClientRects()].filter((r) => r.width > 0);
        if (rects.length) chars.push({c: data.slice(k, k + size), r: rects[0]});
        k += size;
      }
      const result = [];
      for (const ch of chars) {
        const last = result[result.length - 1];
        if (last && Math.abs(last.top - ch.r.top) < 0.5) {
          last.text += ch.c;
          if (ch.c.trim()) last.right = ch.r.right;
        } else {
          result.push({text: ch.c, left: ch.r.left, right: ch.c.trim() ? ch.r.right : ch.r.left, top: ch.r.top,
            height: ch.r.height});
        }
      }
      return result.map((l) => ({text: l.text.trim(), left: l.left - origin.left, right: l.right - origin.left,
        top: l.top - origin.top, height: l.height})).filter((l) => l.text);
    };
    const walk = (n) => {
      for (const c of n.childNodes) {
        if (c.nodeType === 1) {
          elements.push({tag: c.localName, rects: [...c.getClientRects()].map(rel)});
          walk(c);
        } else if (c.nodeType === 3) {
          texts.push(lines(c));
        }
      }
    };
    walk(root);
    out.push({width: origin.width, height: origin.height, elements, texts});
  }
  return JSON.stringify(out);
})()`;

const profile = mkdtempSync(join(tmpdir(), 'weft-inline-'));
const child = spawn(executable, ['--headless=new', '--no-first-run', '--disable-background-networking', '--remote-debugging-port=0',
  '--force-device-scale-factor=1', '--window-size=1000,800', '--user-data-dir=' + profile, 'about:blank'],
  {windowsHide: true, stdio: ['ignore', 'ignore', 'pipe']});
let socket;
try {
  const endpoint = await new Promise((resolveEndpoint, reject) => {
    let text = '';
    const limit = setTimeout(() => reject(new Error('Chromium startup timeout')), 20000);
    child.on('error', reject);
    child.stderr.on('data', (chunk) => {
      text += chunk;
      const m = text.match(/DevTools listening on ws:\/\/127\.0\.0\.1:(\d+)\//);
      if (m) {
        clearTimeout(limit);
        resolveEndpoint('http://127.0.0.1:' + m[1]);
      }
    });
  });
  const version = await (await fetch(endpoint + '/json/version')).json();
  const tab = (await (await fetch(endpoint + '/json/list')).json()).find((t) => t.type === 'page');
  socket = new WebSocket(tab.webSocketDebuggerUrl);
  await new Promise((r, j) => {
    socket.addEventListener('open', r, {once: true});
    socket.addEventListener('error', j, {once: true});
  });
  let next = 0;
  const pending = new Map();
  socket.addEventListener('message', (event) => {
    const message = JSON.parse(event.data);
    if (message.id && pending.has(message.id)) {
      const {resolve: done, reject} = pending.get(message.id);
      pending.delete(message.id);
      message.error ? reject(new Error(message.error.message)) : done(message.result);
    }
  });
  const call = (method, params = {}) => new Promise((done, reject) => {
    const id = ++next;
    pending.set(id, {resolve: done, reject});
    socket.send(JSON.stringify({id, method, params}));
  });
  await call('Page.enable');
  const loaded = new Promise((r) => socket.addEventListener('message', (e) => {
    if (JSON.parse(e.data).method === 'Page.loadEventFired') r();
  }));
  await call('Page.navigate', {url: 'data:text/html;charset=utf-8,' + encodeURIComponent(page)});
  await loaded;
  await call('Runtime.evaluate', {expression: 'document.fonts.ready', awaitPromise: true});
  const measured = JSON.parse((await call('Runtime.evaluate', {expression: measure, returnByValue: true})).result.value);
  const results = cases.map((c, i) => ({...c, chrome: measured[i]}));
  writeFileSync(output, JSON.stringify({browser: version.Browser, dpr: 1,
    source: 'headless Chrome, installed Windows fonts', cases: results}, null, 1) + '\n');
  console.log(`Recorded ${results.length} inline cases from ${version.Browser} to ${output}`);
} finally {
  socket?.close();
  child.kill();
}
