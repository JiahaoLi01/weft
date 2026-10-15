// Chrome oracle for the CSS the CRPG floating UI needs of scene pages (13.4a): each case of
// tests/fixtures/scene-paint-reference/cases.json (text-overflow: ellipsis, text-shadow, filter: drop-shadow() on svg
// icons, inset box-shadow, radial gradients of every shape, size and position, repeating gradients, calc() insets,
// transform-origin keywords, translate3d() and em lengths) drawn by Chrome at its device scale, its nodes as inline styles
// and svg attributes. Host textures (-weft-texture(name), the cases' textures, straight sRGB RGBA8) are PNG images in
// url(), whose background layout CSS gives as it gives the scene's. Writes one screenshot per case and a manifest of the
// browser, its GPU and the hash of the cases. Offline: each page is a data URL; fonts are this machine's installed
// fonts, as for the scene's text.
//
//   node tools/browser_scene_paint_reference.mjs [output directory] [chrome.exe]
import {spawn} from 'node:child_process';
import {deflateSync} from 'node:zlib';
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

// A texture as a PNG data URL: 8-bit RGBA rows, no filter, no color chunk (sRGB as browsers take it).
const crcTable = Array.from({length: 256}, (_, n) => {
  let c = n;
  for (let k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  return c >>> 0;
});
const crc32 = (bytes) => {
  let c = 0xffffffff;
  for (const b of bytes) c = crcTable[(c ^ b) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
};
const chunk = (type, data) => {
  const out = Buffer.alloc(12 + data.length);
  out.writeUInt32BE(data.length, 0);
  out.write(type, 4, 'ascii');
  data.copy(out, 8);
  out.writeUInt32BE(crc32(out.subarray(4, 8 + data.length)), 8 + data.length);
  return out;
};
const png = ({width, height, rgba}) => {
  const pixels = Buffer.from(rgba, 'hex');
  if (pixels.length !== width * height * 4) throw new Error('a texture of the wrong size');
  const header = Buffer.alloc(13);
  header.writeUInt32BE(width, 0);
  header.writeUInt32BE(height, 4);
  header.set([8, 6, 0, 0, 0], 8);
  const rows = Buffer.concat(Array.from({length: height}, (_, y) =>
    Buffer.concat([Buffer.from([0]), pixels.subarray(y * width * 4, (y + 1) * width * 4)])));
  const file = Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', header),
    chunk('IDAT', deflateSync(rows)), chunk('IEND', Buffer.alloc(0))]);
  return 'data:image/png;base64,' + file.toString('base64');
};
const textures = Object.fromEntries(Object.entries(spec.textures ?? {}).map(([name, t]) => [name, png(t)]));
// A style with each host texture as its image.
const imaged = (style) => style.replace(/-weft-texture\(([^)]*)\)/g, (_, name) => {
  const url = textures[name.trim()];
  if (!url) throw new Error('no texture ' + name);
  return `url("${url}")`;
});
const escape = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
// A node as markup: a string is text; an element its tag, style, attributes and children (svg ones take the parser's
// namespace from their svg root).
const markup = (n) => {
  if (typeof n === 'string') return escape(n);
  const attributes = Object.entries(n.attributes ?? {}).map(([k, v]) => ` ${k}="${escape(v)}"`).join('');
  const style = n.style ? ` style="${escape(imaged(n.style))}"` : '';
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
