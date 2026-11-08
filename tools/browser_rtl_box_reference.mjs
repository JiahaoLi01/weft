// Chrome oracle for the direction of boxes (src/layout/flex.cpp, src/scene/scene.cpp): blocks and their margins,
// flex rows and columns, absolutely positioned boxes, grids, inline blocks and scroll containers under direction rtl,
// nested in either direction. Records every element's border box relative to the case's block and, for each scroll
// container, its scrollLeft, scrollWidth and clientWidth (after the scrollLeft a case sets). Text uses the
// repository's fonts (embedded as data URLs) at a fixed line height, so that heights do not depend on font metrics.
// The scene test is tests/scene_rtl_box_tests.cpp.
//
//   node tools/browser_rtl_box_reference.mjs [output.json] [chrome executable]
import {spawn} from 'node:child_process';
import {readFileSync, writeFileSync, mkdtempSync, existsSync, rmSync} from 'node:fs';
import {createHash} from 'node:crypto';
import {tmpdir} from 'node:os';
import {resolve, join} from 'node:path';
import {fileURLToPath} from 'node:url';

const repo = resolve(fileURLToPath(new URL('..', import.meta.url)));
const output = resolve(process.argv[2] ?? resolve(repo, 'tests/fixtures/scene_rtl_box_reference.json'));
const defaults = ['C:/Program Files/Google/Chrome/Application/chrome.exe',
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'];
const executable = process.argv[3] ?? defaults.find(existsSync);
if (!executable || !existsSync(executable)) throw new Error('Provide a local Chromium executable as the second argument');

const fonts = [
  {family: 'Roboto', path: 'third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf'},
  {family: 'TestArabic', path: 'third_party/harfbuzz/source/perf/fonts/Amiri-Regular.ttf'},
  {family: 'TestHebrew', path: 'third_party/harfbuzz/source/test/subset/data/fonts/NotoIKEAHebrewLatin-Regular.ttf'},
];
const files = fonts.map((f) => ({...f, bytes: readFileSync(resolve(repo, f.path))}));

// An element: tag, style, attributes and children (a string is a text node).
const el = (tag, style, children = [], attributes = {}) => ({tag, style, attributes, children});
const div = (style, ...children) => el('div', style, children);
const box = (w, h, extra = '') => div(`width:${w}px;height:${h}px;background:#468;${extra}`);
const font = 'font-family:Roboto, TestArabic, TestHebrew;font-size:16px;line-height:20px';
const rtl = {dir: 'rtl'};
const cases = [
  {name: 'block children', width: 300, attributes: rtl, children: [
    box(100, 10), box(100, 10, 'margin-left:20px'), box(100, 10, 'margin-right:20px'),
    box(100, 10, 'margin:0 auto'), box(100, 10, 'margin-left:auto'), box(100, 10, 'margin-right:auto'),
    div('height:10px;padding:0 10px 0 30px;background:#555', box(50, 10)),
    box(400, 10)]},
  {name: 'block percentages', width: 300, attributes: rtl, children: [
    box(100, 10, 'margin-right:10%'), div('height:10px;padding-left:5%;padding-right:10%', box(40, 10))]},
  {name: 'nested directions', width: 300, attributes: rtl, children: [
    div('padding:5px', div('direction:ltr;padding:5px', box(50, 10), div('direction:rtl', box(50, 10))))]},
  {name: 'flex row', width: 300, attributes: rtl, children: [
    div('display:flex;gap:10px', box(50, 10), box(60, 10), box(70, 10)),
    div('display:flex;justify-content:flex-end', box(50, 10), box(60, 10)),
    div('display:flex;justify-content:center', box(50, 10), box(60, 10)),
    div('display:flex;justify-content:space-between', box(50, 10), box(60, 10), box(10, 10)),
    div('display:flex;flex-direction:row-reverse', box(50, 10), box(60, 10)),
    div('display:flex;justify-content:start', box(50, 10), box(60, 10)),
    div('display:flex;justify-content:end', box(50, 10), box(60, 10))]},
  {name: 'flex wrap and grow', width: 200, attributes: rtl, children: [
    div('display:flex;flex-wrap:wrap;gap:5px', box(80, 10), box(80, 10), box(80, 10), box(30, 10)),
    div('display:flex', box(50, 10, 'flex-grow:1'), box(50, 10), box(50, 10, 'margin-left:15px')),
    div('display:flex', box(50, 10, 'order:2'), box(50, 10, 'order:1'))]},
  {name: 'flex column', width: 300, attributes: rtl, children: [
    div('display:flex;flex-direction:column', box(50, 10), box(60, 10, 'align-self:flex-end'),
      box(70, 10, 'align-self:center'), div('height:10px;background:#555')),
    div('display:flex;flex-direction:column;align-items:flex-start', box(50, 10)),
    div('display:flex;flex-direction:column;align-items:end', box(50, 10)),
    div('display:flex;flex-direction:column;align-items:start', box(50, 10))]},
  {name: 'flex in ltr island', width: 300, attributes: rtl, children: [
    div('display:flex;direction:ltr;gap:4px', box(50, 10), box(60, 10)),
    div('display:flex;gap:4px', div('direction:ltr;display:flex', box(20, 10), box(30, 10)), box(60, 10))]},
  {name: 'absolute', width: 300, attributes: rtl, style: 'position:relative;height:120px', children: [
    box(40, 10, 'position:absolute;left:10px;top:0'),
    box(40, 10, 'position:absolute;right:10px;top:15px'),
    box(40, 10, 'position:absolute;left:10px;right:20px;top:30px'),
    div('position:absolute;left:10px;right:20px;top:45px;height:10px;background:#555'),
    box(40, 10, 'position:absolute;top:60px'),
    box(40, 10, 'position:absolute;top:75px;margin-left:7px;margin-right:9px'),
    div('position:absolute;top:90px;height:10px;background:#555', box(30, 10))]},
  // First in its parent: the scene's static position does not follow in-flow siblings down yet (docs/status.md).
  {name: 'absolute static position', width: 300, attributes: rtl, style: 'position:relative', children: [
    box(40, 10, 'position:absolute'), box(100, 10), box(80, 10),
    div('display:flex;justify-content:center;position:relative;height:30px', box(40, 10, 'position:absolute'))]},
  {name: 'grid', width: 300, attributes: rtl, children: [
    div('display:grid;grid-template-columns:100px 1fr 50px;column-gap:10px;row-gap:5px',
      box(20, 10), div('height:10px;background:#555'), box(20, 10), div('height:10px;grid-column:span 2'),
      box(20, 10, 'justify-self:end'), box(20, 10, 'justify-self:center'), box(20, 10, 'justify-self:start')),
    div('display:grid;grid-template-columns:repeat(3, 40px);justify-content:center;column-gap:5px',
      box(10, 10), box(10, 10), box(10, 10)),
    div('display:grid;grid-template-columns:repeat(2, 40px);justify-content:end',
      box(10, 10), div('height:10px;margin-left:5px;margin-right:12px;background:#555')),
    div('display:grid;grid-template-columns:50px 50px 50px', box(10, 10, 'grid-column:3'), box(10, 10,
      'grid-column:1 / 3'))]},
  {name: 'grid absolute', width: 300, attributes: rtl, children: [
    div('display:grid;grid-template-columns:60px 60px 60px;height:60px;position:relative;padding:0 7px 0 13px',
      box(10, 10), div('position:absolute;grid-column:2 / 4;top:0;height:10px;background:#555'),
      div('position:absolute;grid-column:2;top:20px;height:10px;background:#555'),
      div('position:absolute;grid-column:auto / 2;top:40px;height:10px;background:#555'))]},
  {name: 'inline blocks', width: 300, attributes: rtl, style: font, children: [
    div('', el('span', 'display:inline-block;width:40px;height:10px;background:#468'), ' ',
      el('span', 'display:inline-block;width:60px;height:10px;background:#468'), ' שלום'),
    div('text-align:left', el('span', 'display:inline-block;width:40px;height:10px;background:#468')),
    div('text-align:center', el('span', 'display:inline-block;width:40px;height:10px;background:#468'))]},
  {name: 'scroll container', width: 300, attributes: rtl, children: [
    div('width:200px;height:30px;overflow:auto', box(500, 10), box(50, 10)),
    div('width:200px;height:30px;overflow:hidden;padding:0 10px 0 20px', box(300, 10, 'margin-left:5px')),
    div('width:200px;height:30px;overflow:auto;display:flex;gap:10px', box(120, 10), box(120, 10)),
    div('width:200px;height:30px;overflow:auto', box(100, 10))]},
  {name: 'scrolled', width: 300, attributes: rtl, scroll: [{element: 0, left: -120}, {element: 3, left: -500},
    {element: 6, left: 30}],
  children: [
    div('width:200px;height:30px;overflow:auto', box(500, 10), box(50, 10)),
    div('width:200px;height:30px;overflow:hidden', box(400, 10)),
    div('width:200px;height:30px;overflow:auto', box(100, 10))]},
  {name: 'ltr scroll inside rtl', width: 300, attributes: rtl, scroll: [{element: 0, left: 50}], children: [
    div('width:200px;height:30px;overflow:auto;direction:ltr', box(500, 10))]},
  {name: 'text blocks', width: 300, attributes: rtl, style: font, children: [
    div('width:150px', 'שלום עולם'), div('width:150px;margin-left:auto', 'abc'),
    div('display:flex;gap:6px', div('', 'שלום'), div('', 'עולם'), div('', 'abc'))]},
];

// Random trees (a seeded generator, so that the fixture is reproducible): blocks, flex and grid containers of random
// direction, sizes, margins (auto too), padding, alignment and order, with absolutely positioned children, and boxes
// without text (no font metrics).
let seed = 20261108;
const random = (n) => {
  seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
  return (seed >>> 8) % n;
};
const pick = (list) => list[random(list.length)];
const direction = () => pick(['', '', '', 'direction:rtl;', 'direction:ltr;']);
// Horizontal edges only: a vertical margin would collapse, which the scene does not do yet. auto only on leaves (of a
// definite width): a block of auto width takes auto margins as zero in Chrome, not yet in the scene.
const edges = (property, automatic = true) => random(3) ? '' : (`${property}:${property === 'padding' ? random(3) * 4 :
  0}px ${pick(['0', '6px', 'auto', '11px'])} 0 ${pick(['0', '3px', 'auto', '8px'])};`)
  .replace(/auto/g, property === 'padding' || !automatic ? '2px' : 'auto');
const leaf = () => box(10 + random(60), 6 + random(10), direction() + edges('margin') +
  (random(5) ? '' : 'align-self:' + pick(['flex-start', 'flex-end', 'center', 'start', 'end']) + ';') +
  (random(6) ? '' : 'justify-self:' + pick(['start', 'end', 'center']) + ';') +
  (random(6) ? '' : 'order:' + random(3) + ';'));
const absolute = () => box(10 + random(30), 6, 'position:absolute;' + pick(['', 'left:5px;', 'right:7px;',
  'left:4px;right:9px;', 'left:10%;']) + pick(['top:0;', 'bottom:3px;', 'top:12px;']) + edges('margin'));
// gridItem: a grid's child, laid out in its own area, which contains its absolutely positioned descendants in the
// scene (a containing block beyond a grid item is not reached yet): one with such a child is positioned.
const container = (depth, gridItem = false) => {
  const kind = random(depth > 1 ? 2 : 4);
  const children = [];
  for (let k = 0, n = 1 + random(4); k < n; ++k) {
    children.push(depth < 2 && random(3) === 0 ? container(depth + 1, kind >= 2 || gridItem) : leaf());
  }
  const positioned = random(4) === 0;
  if (random(4) === 0) children.push(absolute());
  const common = direction() + edges('padding') + edges('margin', false) +
    (positioned || (gridItem && children.some((c) => c.style.includes('position:absolute'))) ? 'position:relative;' : '') +
    (random(4) ? '' : `width:${80 + random(150)}px;`);
  if (kind === 0) return div(common, ...children);
  if (kind === 1) {
    return div(common + 'display:flex;' + pick(['', 'flex-direction:row-reverse;', 'flex-direction:column;',
      'flex-wrap:wrap;']) + pick(['', 'justify-content:flex-end;', 'justify-content:center;',
      'justify-content:space-between;', 'justify-content:start;', 'justify-content:end;']) +
      pick(['', 'align-items:flex-start;', 'align-items:flex-end;', 'align-items:center;']) +
      (random(2) ? 'gap:' + random(8) + 'px;' : ''), ...children);
  }
  return div(common + 'display:grid;grid-template-columns:' + pick(['40px 1fr', 'repeat(3, 30px)', '20px 50px 1fr',
    '1fr 1fr']) + ';column-gap:' + random(8) + 'px;' + pick(['', 'justify-content:end;', 'justify-content:center;',
    'justify-items:end;', 'justify-items:center;']), ...children.filter((c) => !c.style.includes('position')));
};
// Each random tree after its left-to-right twin (no direction anywhere): the scene test compares a tree only where its
// twin matches, so that what differs is the direction, not layout the scene does otherwise (margin collapsing, auto
// margins of a block of auto width).
const ltr = (n) => typeof n === 'string' ? n : {...n, attributes: {},
  style: n.style.replace(/direction:(rtl|ltr);/g, ''), children: n.children.map(ltr)};
for (let i = 0; i < 150; ++i) {
  const children = [];
  for (let k = 0, n = 1 + random(3); k < n; ++k) children.push(container(0));
  const width = 150 + random(200);
  const attributes = random(4) ? rtl : {};
  cases.push({name: 'random ' + i + ' ltr', width, attributes: {}, style: 'position:relative;',
    children: children.map(ltr)});
  cases.push({name: 'random ' + i, width, attributes, style: 'position:relative;', children});
}

const escape = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/"/g, '&quot;');
const attributes = (n) => Object.entries(n.attributes ?? {}).map(([k, v]) => ` ${k}="${escape(v)}"`).join('');
const html = (n) => {
  if (typeof n === 'string') return escape(n);
  return `<${n.tag} style="${escape(n.style)}"${attributes(n)}>${n.children.map(html).join('')}</${n.tag}>`;
};
const faces = files.map((f) => `@font-face{font-family:${f.family};src:url(data:font/ttf;base64,` +
  f.bytes.toString('base64') + `)}`).join('');
const page = '<!doctype html><html><head><meta charset=utf-8><style>' + faces +
  '*{box-sizing:border-box;border:0 solid;margin:0;padding:0}body{margin:0;' + font + '}.row{position:relative;' +
  'height:400px}</style></head><body>' +
  cases.map((c, i) => `<div class=row><div id=case${i} style="width:${c.width}px;${escape(c.style ?? '')}"` +
    `${attributes(c)}>` + c.children.map(html).join('') + '</div></div>').join('') + '</body></html>';

const measure = `((scrolls) => {
  const out = [];
  for (let i = 0; ; ++i) {
    const root = document.getElementById('case' + i);
    if (!root) break;
    const all = [...root.querySelectorAll('*')];
    for (const s of scrolls[i] ?? []) all[s.element].scrollLeft = s.left;
    const origin = root.getBoundingClientRect();
    const elements = all.map((e) => {
      const r = e.getBoundingClientRect();
      const o = {tag: e.localName, rect: [r.left - origin.left, r.top - origin.top, r.width, r.height]};
      const overflow = getComputedStyle(e).overflow;
      if (overflow !== 'visible' && overflow !== 'clip') {
        o.scroll = [e.scrollLeft, e.scrollWidth, e.clientWidth];
      }
      return o;
    });
    out.push({width: origin.width, height: origin.height, elements});
  }
  return JSON.stringify(out);
})`;

const profile = mkdtempSync(join(tmpdir(), 'weft-rtl-box-'));
const child = spawn(executable, ['--headless=new', '--no-first-run', '--disable-background-networking',
  '--remote-debugging-port=0', '--force-device-scale-factor=1', '--window-size=1000,800', '--user-data-dir=' + profile,
  'about:blank'], {windowsHide: true, stdio: ['ignore', 'ignore', 'pipe']});
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
  const tree = await call('Page.getFrameTree');
  await call('Page.setDocumentContent', {frameId: tree.frameTree.frame.id, html: page});
  await call('Runtime.evaluate', {expression: 'document.fonts.ready.then(() => document.fonts.size)',
    awaitPromise: true});
  const scrolls = JSON.stringify(cases.map((c) => c.scroll ?? []));
  const measured = JSON.parse((await call('Runtime.evaluate', {expression: measure + '(' + scrolls + ')',
    returnByValue: true})).result.value);
  const results = cases.map((c, i) => ({...c, chrome: measured[i]}));
  writeFileSync(output, JSON.stringify({browser: version.Browser, dpr: 1,
    source: 'headless Chrome, repository fonts as data URLs',
    fonts: files.map((f) => ({family: f.family, path: f.path,
      sha256: createHash('sha256').update(f.bytes).digest('hex')})),
    cases: results}, null, 1) + '\n');
  console.log(`Recorded ${results.length} box direction cases from ${version.Browser} to ${output}`);
} finally {
  socket?.close();
  const exited = new Promise((r) => child.once('exit', r));
  child.kill();
  await exited;
  rmSync(profile, {recursive: true, force: true, maxRetries: 5, retryDelay: 200});
}
