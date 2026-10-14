// Chrome oracle for the CSS the CRPG floating UI needs of scene pages (13.4a): each case of
// tests/fixtures/scene-paint-reference/cases.json (text-overflow: ellipsis, text-shadow, filter: drop-shadow() on svg
// icons, inset box-shadow, radial gradients of every shape, size and position, repeating gradients, calc() insets,
// transform-origin keywords, translate3d() and em lengths) drawn by Chrome at its device scale, its nodes as inline styles
// and svg attributes. Writes one screenshot per case and a manifest of the browser, its GPU and the hash of the cases.
// Offline: each page is a data URL; fonts are this machine's installed fonts, as for the scene's text.
//
//   node tools/browser_scene_paint_reference.mjs [output directory] [chrome.exe]
import {spawn} from 'node:child_process';
import {readFileSync, writeFileSync, mkdirSync, mkdtempSync, existsSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {resolve, join} from 'node:path';
import {repo, sha} from './gallery_runtime.mjs';

const output = resolve(process.argv[2] ?? resolve(repo, 'tests/fixtures/scene-paint-reference'));
const executable = process.argv[3] ?? 'C:/Program Files/Google/Chrome/Application/chrome.exe';
if (!existsSync(executable)) throw new Error('Provide a local Chromium executable as the second argument');
const casesPath = resolve(repo, 'tests/fixtures/scene-paint-reference/cases.json');
const casesText = readFileSync(casesPath, 'utf8');
const spec = JSON.parse(casesText);

const escape = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
// A node as markup: a string is text; an element its tag, style, attributes and children (svg ones take the parser's
// namespace from their svg root).
const markup = (n) => {
  if (typeof n === 'string') return escape(n);
  const attributes = Object.entries(n.attributes ?? {}).map(([k, v]) => ` ${k}="${escape(v)}"`).join('');
  const style = n.style ? ` style="${escape(n.style)}"` : '';
  return `<${n.tag}${attributes}${style}>${(n.children ?? []).map(markup).join('')}</${n.tag}>`;
};
const page = (c) => '<!doctype html><html><head><meta charset=utf-8><style>*{box-sizing:border-box;margin:0;padding:0}' +
  'html,body{margin:0;background:#000}</style></head><body>' +
  `<div id=root style="${escape(spec.root + ';' + (c.root ?? ''))}">${c.nodes.map(markup).join('')}</div></body></html>`;

const profile = mkdtempSync(join(tmpdir(), 'weft-scene-paint-'));
const child = spawn(executable, ['--headless=new', '--no-first-run', '--disable-background-networking',
  '--remote-debugging-port=0', '--hide-scrollbars', `--window-size=${spec.width},${spec.height}`,
  '--user-data-dir=' + profile, 'about:blank'], {windowsHide: true, stdio: ['ignore', 'ignore', 'pipe']});
let socket;
let browser;
// A DevTools connection to url: call(method, params) and the events that arrive, for one listener each.
const connect = async (url) => {
  const ws = new WebSocket(url);
  await new Promise((r, j) => {
    ws.addEventListener('open', r, {once: true});
    ws.addEventListener('error', j, {once: true});
  });
  let next = 0;
  const pending = new Map();
  const listeners = [];
  ws.addEventListener('message', (event) => {
    const message = JSON.parse(event.data);
    if (message.id && pending.has(message.id)) {
      const {resolve: done, reject} = pending.get(message.id);
      pending.delete(message.id);
      message.error ? reject(new Error(message.error.message)) : done(message.result);
    } else if (message.method) {
      for (const l of listeners.splice(0)) l(message);
    }
  });
  const call = (method, params = {}) => new Promise((done, reject) => {
    const id = ++next;
    pending.set(id, {resolve: done, reject});
    ws.send(JSON.stringify({id, method, params}));
  });
  return {ws, call, listeners};
};
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
  browser = await connect(version.webSocketDebuggerUrl);
  const info = await browser.call('SystemInfo.getInfo');
  const gpu = {devices: info.gpu.devices.filter((d) => d.deviceString)
    .map((d) => ({name: d.deviceString, driver: d.driverVersion})), features: info.gpu.featureStatus ?? {}};
  const {ws, call, listeners} = await connect(tab.webSocketDebuggerUrl);
  socket = ws;
  await call('Page.enable');
  mkdirSync(output, {recursive: true});
  const recorded = [];
  for (const c of spec.cases) {
    await call('Emulation.setDeviceMetricsOverride', {width: spec.width, height: spec.height,
      deviceScaleFactor: c.scale, mobile: false});
    const loaded = new Promise((r) => {
      const wait = (m) => (m.method === 'Page.loadEventFired' ? r() : listeners.push(wait));
      listeners.push(wait);
    });
    await call('Page.navigate', {url: 'data:text/html;charset=utf-8,' + encodeURIComponent(page(c))});
    await loaded;
    await call('Runtime.evaluate', {expression: 'document.fonts.ready.then(()=>new Promise(r=>requestAnimationFrame(()=>requestAnimationFrame(r))))',
      awaitPromise: true});
    const shot = await call('Page.captureScreenshot', {format: 'png',
      clip: {x: 0, y: 0, width: spec.width, height: spec.height, scale: 1}, captureBeyondViewport: false});
    const bytes = Buffer.from(shot.data, 'base64');
    writeFileSync(join(output, c.name + '.png'), bytes);
    recorded.push({name: c.name, scale: c.scale, sha256: sha(bytes)});
  }
  writeFileSync(join(output, 'manifest.json'), JSON.stringify({format: 'weft-scene-paint-reference-v1',
    browser: version.Browser, gpu, cases_sha256: sha(casesText), captures: recorded}, null, 1) + '\n');
  console.log(`Recorded ${recorded.length} scene paint cases from ${version.Browser} to ${output}`);
} finally {
  socket?.close();
  browser?.ws.close();
  child.kill();
}
