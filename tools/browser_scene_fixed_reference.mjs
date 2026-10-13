// Chrome oracle for position: fixed in scenes (11.3c): one page in a 600 x 400 viewport whose fixed boxes sit under
// an absolutely positioned box (px and percentage insets of the viewport, a shrink-to-fit box of text), under a
// transformed box and a box with will-change: transform (their containing blocks), and inside a box that clips but
// holds them. Records every element's bounding client rect in tree order. Offline: the page is a data URL; the
// tree is written with the rects, so the test builds the same page.
//
//   node tools/browser_scene_fixed_reference.mjs [output.json] [chrome.exe]
import {spawn} from 'node:child_process';
import {writeFileSync, mkdtempSync, existsSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {resolve, join} from 'node:path';
import {repo} from './gallery_runtime.mjs';

const output = resolve(process.argv[2] ?? resolve(repo, 'tests/fixtures/scene_fixed_reference.json'));
const executable = process.argv[3] ?? 'C:/Program Files/Google/Chrome/Application/chrome.exe';
if (!existsSync(executable)) throw new Error('Provide a local Chromium executable as the second argument');

const width = 600;
const height = 400;
const el = (style, ...children) => ({tag: 'div', style, children});
const font = 'font-family:system-ui;font-size:14px;line-height:20px;';
const tree = el('position:relative;width:600px;height:400px;background-color:#101010;' + font,
  el('position:absolute;left:20px;top:30px;width:200px;height:100px;padding:5px;border:3px solid #888888',
    el('position:fixed;left:10px;top:12px;width:50px;height:20px;background-color:#d4a843'),
    el('position:fixed;right:10%;bottom:15px;width:30%;height:10%;background-color:#60a5fa'),
    el('margin-left:40px;height:30px',
      el('position:fixed;left:300px;top:340px;padding:4px;color:#e0e0e0', 'Pinned tooltip text'))),
  el('position:absolute;left:300px;top:40px;width:200px;height:120px;transform:translate(5px, 6px);' +
    'background-color:#303030',
    el('padding:10px',
      el('position:fixed;left:7px;top:9px;width:40px;height:40px;background-color:#c084fc'))),
  el('position:absolute;left:300px;top:200px;width:150px;height:100px;will-change:transform;' +
    'background-color:#202838',
    el('position:fixed;right:0;bottom:0;width:20px;height:20px;background-color:#fb923c')),
  el('position:absolute;left:20px;top:200px;width:240px;height:120px;overflow:hidden;background-color:#282018',
    el('padding:20px',
      el('position:fixed;left:40px;top:230px;width:100px;height:30px;z-index:5;background-color:#e0e0e0'))),
  // A static box with will-change: transform contains its fixed and absolute descendants.
  el('position:absolute;left:470px;top:200px;width:120px;height:120px;background-color:#183028',
    el('margin:10px;height:80px;will-change:transform;background-color:#284038',
      el('position:fixed;right:0;bottom:0;width:15px;height:15px;background-color:#a3e635'),
      el('position:absolute;left:0;top:0;width:10px;height:10px;background-color:#f472b6'))));

const escape = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/"/g, '&quot;');
const html = (n) => typeof n === 'string' ? escape(n) :
  `<${n.tag} style="${escape(n.style)}">${n.children.map(html).join('')}</${n.tag}>`;
const page = '<!doctype html><html><head><meta charset=utf-8><style>*{box-sizing:border-box;border:0 solid;margin:0;' +
  `padding:0}body{margin:0;width:${width}px;height:${height}px;overflow:hidden}</style></head><body>` + html(tree) +
  '</body></html>';
const measure = `JSON.stringify([...document.body.querySelectorAll('div')].map((e) => {
  const r = e.getBoundingClientRect();
  return [r.left, r.top, r.width, r.height];
}))`;

const profile = mkdtempSync(join(tmpdir(), 'weft-fixed-'));
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
