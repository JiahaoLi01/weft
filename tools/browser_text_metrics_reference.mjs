// Chrome text metrics oracle for the portable scene text (src/scene/scene_text.cpp).
// Renders single-line spans for a matrix of font-family lists, weights, sizes, letter-spacing and
// line-height values used by the fixed gallery, then records each span's width, height, baseline
// offset and the platform font Chrome selected (CSS.getPlatformFontsForNode). Offline: the page is
// a data URL; fonts are this machine's installed fonts, as for the scene gallery references.
//
//   node tools/browser_text_metrics_reference.mjs [output.json] [chrome.exe]
import {spawn} from 'node:child_process';
import {writeFileSync, mkdtempSync, existsSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {resolve, join} from 'node:path';
import {repo} from './gallery_runtime.mjs';

const output = resolve(process.argv[2] ?? resolve(repo, 'tests/fixtures/scene_text_reference.json'));
const executable = process.argv[3] ?? 'C:/Program Files/Google/Chrome/Application/chrome.exe';
if (!existsSync(executable)) throw new Error('Provide a local Chromium executable as the second argument');

const families = [
  'system-ui',
  'sans-serif',
  'serif',
  'monospace',
  '"Consolas", monospace',
  '"Segoe UI", system-ui, sans-serif',
  'ui-sans-serif, system-ui, sans-serif, "Apple Color Emoji"',
  '"Noto Serif SC", "Cinzel", "Times New Roman", serif',
  '"Cinzel Decorative", "EB Garamond", "Shippori Mincho", "LXGW NeoZhiSong Plus", "Noto Serif SC", "Times New Roman", serif',
  '"Noto Sans SC", sans-serif',
  'Arial',
];
const texts = ['Hello World 123', 'AVATAR Wave, fly!', '进度 42%', 'Level 7 · 战斗', 'mono_case()', '☠️'];
const cases = [];
for (const family of families) {
  for (const weight of [400, 600, 700]) {
    for (const size of [10, 14, 16, 24]) {
      for (const text of texts) {
        cases.push({family, weight, size, text, letterSpacing: '0', lineHeight: 'normal'});
      }
    }
  }
}
// Spacing and line-height variants on the gallery's main families.
for (const family of ['system-ui', '"Consolas", monospace', '"Noto Serif SC", "Cinzel", "Times New Roman", serif']) {
  for (const [letterSpacing, lineHeight] of [['0.1em', 'normal'], ['2px', '1.5'], ['-0.5px', '20px'], ['0', '1.25'], ['0.05em', '24px']]) {
    for (const size of [12, 16]) {
      cases.push({family, weight: 400, size, text: 'Hello World 123', letterSpacing, lineHeight});
      cases.push({family, weight: 700, size, text: '进度 42%', letterSpacing, lineHeight});
    }
  }
}

const escape = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/"/g, '&quot;');
const spans = cases.map((c, i) =>
  `<div class=row><span id=s${i} style="font-family:${escape(c.family)};font-weight:${c.weight};font-size:${c.size}px;` +
  `letter-spacing:${c.letterSpacing};line-height:${c.lineHeight}">${escape(c.text)}<i></i></span></div>`).join('');
const html = '<!doctype html><html><head><meta charset=utf-8><style>body{margin:0}.row{display:block;white-space:pre}' +
  'span{display:inline-block;white-space:pre}i{display:inline-block;width:0;height:0;vertical-align:baseline}</style></head>' +
  '<body>' + spans + '</body></html>';

const profile = mkdtempSync(join(tmpdir(), 'weft-text-metrics-'));
const child = spawn(executable, ['--headless=new', '--no-first-run', '--disable-background-networking', '--remote-debugging-port=0',
  '--force-device-scale-factor=1', '--window-size=800,600', '--user-data-dir=' + profile, 'about:blank'],
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
  await call('DOM.enable');
  await call('CSS.enable');
  const loaded = new Promise((r) => socket.addEventListener('message', (e) => {
    if (JSON.parse(e.data).method === 'Page.loadEventFired') r();
  }));
  await call('Page.navigate', {url: 'data:text/html;charset=utf-8,' + encodeURIComponent(html)});
  await loaded;
  await call('Runtime.evaluate', {expression: 'document.fonts.ready', awaitPromise: true});
  const measured = JSON.parse((await call('Runtime.evaluate', {
    expression: `JSON.stringify([...document.querySelectorAll('span')].map(s => {
      const r = s.getBoundingClientRect(); const b = s.querySelector('i').getBoundingClientRect();
      return [r.width, r.height, b.top - r.top];
    }))`, returnByValue: true})).result.value);
  const document = await call('DOM.getDocument', {depth: -1});
  const results = [];
  for (let i = 0; i < cases.length; ++i) {
    const node = await call('DOM.querySelector', {nodeId: document.root.nodeId, selector: '#s' + i});
    const fonts = (await call('CSS.getPlatformFontsForNode', {nodeId: node.nodeId})).fonts;
    results.push({...cases[i], width: measured[i][0], height: measured[i][1], baseline: measured[i][2],
      fonts: fonts.map((f) => ({family: f.familyName, postScriptName: f.postScriptName, glyphs: f.glyphCount}))});
  }
  writeFileSync(output, JSON.stringify({browser: version.Browser, dpr: 1, source: 'headless Chrome, installed Windows fonts',
    cases: results}, null, 1) + '\n');
  console.log(`Recorded ${results.length} text metric cases from ${version.Browser} to ${output}`);
} finally {
  socket?.close();
  child.kill();
}
