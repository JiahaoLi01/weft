// Chrome oracle for absolutely positioned boxes of auto width in scenes: one page in a 600 x 400 viewport whose boxes
// shrink to fit (CSS 2 10.3.7) the width their containing block leaves beside their insets, or their content's
// min-content width when that is wider: in a containing block 1px wide (a strip that moves along a timeline) with nowrap
// CJK text, inline-blocks and wrapping text at a left or a right inset; in wide containing blocks at insets that leave
// less than the text's max-content width, in a block and in a row flex container, with margins, padding and borders,
// at a percentage inset, around a block child, under max-width and min-width; and, unchanged, a box at its static
// position and one between two insets. Records every element's bounding client rect in tree order. Offline: the page is
// a data URL; the tree is written with the rects, so the test builds the same page.
//
//   node tools/browser_scene_absolute_reference.mjs [output.json] [chrome.exe]
import {spawn} from 'node:child_process';
import {writeFileSync, mkdtempSync, existsSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {resolve, join} from 'node:path';
import {repo} from './gallery_runtime.mjs';

const output = resolve(process.argv[2] ?? resolve(repo, 'tests/fixtures/scene_absolute_reference.json'));
const executable = process.argv[3] ?? 'C:/Program Files/Google/Chrome/Application/chrome.exe';
if (!existsSync(executable)) throw new Error('Provide a local Chromium executable as the second argument');

const width = 600;
const height = 400;
const node = (tag) => (style, ...children) => ({tag, style, children});
const el = node('div');
const span = node('span');
const font = 'font-family:system-ui;font-size:14px;line-height:20px;';
const text = 'Window timeline track';
const chip = () => span('display:inline-block;width:30px;height:16px;margin-left:4px;background-color:#3366cc');
// A containing block: absolutely positioned at left, top, of a width and height.
const block = (left, top, w, h, more, ...children) =>
  el(`position:absolute;left:${left}px;top:${top}px;width:${w}px;height:${h}px;${more}`, ...children);
const tree = el('position:relative;width:600px;height:400px;background-color:#101010;color:#e0e0e0;' + font,
  // A strip 1px wide: nowrap CJK text at a left inset, nowrap inline-blocks at a right inset.
  block(300, 10, 1, 40, 'background-color:#ff0000',
    el('position:absolute;left:4px;top:0;white-space:nowrap;background-color:#4a3020', '窗口时间轴 滑铲'),
    el('position:absolute;right:4px;top:20px;white-space:nowrap;background-color:#203048', chip(), chip(), chip())),
  // Wrapping text in the strip: its min-content width, its lines laid out there; nowrap text at a right inset.
  block(300, 60, 1, 60, 'background-color:#ff0000',
    el('position:absolute;left:4px;top:0;background-color:#204020', '窗口 slide track'),
    el('position:absolute;right:4px;top:40px;white-space:nowrap;background-color:#402040', '校正 3 cm')),
  // Wide containing blocks whose insets leave less than the text's max-content width: a left inset, a right inset.
  block(0, 130, 300, 60, 'background-color:#202020',
    el('position:absolute;left:230px;top:0;background-color:#4a3020', text)),
  block(0, 200, 300, 60, 'background-color:#202020',
    el('position:absolute;right:230px;top:0;background-color:#203048', text)),
  // A row flex container's child, and one with margins, padding and borders.
  block(0, 270, 300, 60, 'display:flex;background-color:#202020',
    el('position:absolute;left:230px;top:0;background-color:#204020', text)),
  block(0, 340, 300, 60, 'background-color:#202020',
    el('position:absolute;left:230px;top:0;margin-left:10px;padding:0 5px;border:2px solid #888888;' +
      'background-color:#402040', text)),
  // A percentage inset; a block child inside the box; max-width and min-width.
  block(320, 130, 200, 60, 'background-color:#202020',
    el('position:absolute;left:50%;top:0;background-color:#4a3020', text + ' and more')),
  block(320, 200, 200, 40, 'background-color:#202020',
    el('position:absolute;left:150px;top:0;background-color:#203048',
      el('white-space:nowrap;background-color:#305070', 'Nested block'))),
  block(320, 250, 1, 40, 'background-color:#ff0000',
    el('position:absolute;left:4px;top:0;max-width:40px;white-space:nowrap;background-color:#204020', '窗口时间轴'),
    el('position:absolute;left:4px;top:20px;min-width:120px;background-color:#402040', 'short')),
  // Unchanged: a box at its static position, and one between two insets.
  block(320, 300, 200, 90, 'background-color:#202020',
    el('position:absolute;background-color:#4a3020', text + ' at its static place'),
    el('position:absolute;left:10px;right:10px;top:50px;background-color:#203048', text)));

const escape = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/"/g, '&quot;');
const html = (n) => typeof n === 'string' ? escape(n) :
  `<${n.tag} style="${escape(n.style)}">${n.children.map(html).join('')}</${n.tag}>`;
const page = '<!doctype html><html><head><meta charset=utf-8><style>*{box-sizing:border-box;border:0 solid;margin:0;' +
  `padding:0}body{margin:0;width:${width}px;height:${height}px;overflow:hidden}</style></head><body>` + html(tree) +
  '</body></html>';
const measure = `JSON.stringify([...document.body.querySelectorAll('*')].map((e) => {
  const r = e.getBoundingClientRect();
  return [r.left, r.top, r.width, r.height];
}))`;

const profile = mkdtempSync(join(tmpdir(), 'weft-absolute-'));
const child = spawn(executable, ['--headless=new', '--no-first-run', '--disable-background-networking',
  '--remote-debugging-port=0', '--force-device-scale-factor=1', `--window-size=${width},${height}`,
  '--user-data-dir=' + profile, 'about:blank'], {windowsHide: true, stdio: ['ignore', 'ignore', 'pipe']});
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
  await call('Emulation.setDeviceMetricsOverride', {width, height, deviceScaleFactor: 0, mobile: false});
  const loaded = new Promise((r) => socket.addEventListener('message', (e) => {
    if (JSON.parse(e.data).method === 'Page.loadEventFired') r();
  }));
  await call('Page.navigate', {url: 'data:text/html;charset=utf-8,' + encodeURIComponent(page)});
  await loaded;
  await call('Runtime.evaluate', {expression: 'document.fonts.ready', awaitPromise: true});
  const rects = JSON.parse((await call('Runtime.evaluate', {expression: measure, returnByValue: true})).result.value);
  writeFileSync(output, JSON.stringify({browser: version.Browser, width, height, tree, rects}, null, 1) + '\n');
  console.log(`Recorded ${rects.length} element rects from ${version.Browser} to ${output}`);
} finally {
  socket?.close();
  child.kill();
}
