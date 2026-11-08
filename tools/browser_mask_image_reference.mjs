// Chrome oracle for CSS masks (src/scene/scene_mask.cpp's layers, the mask entries of scene.hlsl's clip chain): boxes
// with a border, padding, a background and a child that overflows them, each masked by gradient layers (linear,
// radial, conic, repeating) sized, positioned, repeated, clipped and placed by mask-size, -position, -repeat, -clip and
// -origin, as alpha or luminance, composited by mask-composite, through the mask and -webkit-mask shorthands and the
// -webkit-mask-* names, with opacity, corner radii, transforms, clip-path and nesting; and image layers (url() of PNG
// files it writes there, and a data: URL) sized by their natural size, contain, cover and lengths, as alpha and
// luminance, repeated and alone, with gradient layers. Writes to
// tests/fixtures/mask_image the page's script (page.js, which builds the cases in #root), the cases' places
// (cases.json), a screenshot of the page at device scale 1 (chrome.png) and, for points over every case, which element
// a press there reaches (hits.json): what tests/mask_image_metal_tests.mm and tests/mask_image_tests.cpp compare a
// View's drawing and hits with.
//
//   node tools/browser_mask_image_reference.mjs [chrome executable]
import {spawn} from 'node:child_process';
import {createServer} from 'node:http';
import {existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync} from 'node:fs';
import {deflateSync} from 'node:zlib';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

const repo = resolve(fileURLToPath(new URL('..', import.meta.url)));
const directory = resolve(repo, 'tests/fixtures/mask_image');
const defaults = ['C:/Program Files/Google/Chrome/Application/chrome.exe',
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'];
const executable = process.argv[2] ?? defaults.find(existsSync);
if (!executable || !existsSync(executable)) throw new Error('Provide a local Chromium executable as the first argument');
mkdirSync(directory, {recursive: true});

const crcTable = Array.from({length: 256}, (_, n) => {
  let c = n;
  for (let k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  return c >>> 0;
});
const crc32 = (b) => {
  let c = 0xffffffff;
  for (const x of b) c = crcTable[(c ^ x) & 255] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
};
const chunk = (type, body) => {
  const o = Buffer.alloc(12 + body.length);
  o.writeUInt32BE(body.length, 0);
  o.write(type, 4, 'ascii');
  body.copy(o, 8);
  o.writeUInt32BE(crc32(o.subarray(4, 8 + body.length)), 8 + body.length);
  return o;
};
// An RGBA PNG of w x h from pixel(x, y) -> [r, g, b, a].
const png = (w, h, pixel) => {
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; ++y) {
    for (let x = 0; x < w; ++x) raw.set(pixel(x, y), y * (w * 4 + 1) + 1 + x * 4);
  }
  const header = Buffer.alloc(13);
  header.writeUInt32BE(w, 0);
  header.writeUInt32BE(h, 4);
  header[8] = 8;
  header[9] = 6;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', header),
    chunk('IDAT', deflateSync(raw, {level: 9})), chunk('IEND', Buffer.alloc(0))]);
};
// Mask images, written when missing: a ring with a dot (an icon), a wide ellipse, an opaque image of a gray ramp and
// red, green, blue and white bands (luminance), a checkerboard of 4 px squares and one-pixel stripes (fine detail
// drawn smaller); antialiased edges.
const cover = (d) => Math.max(0, Math.min(1, 0.5 - d));
const images = {
  'icon.png': () => png(48, 48, (x, y) => {
    const r = Math.hypot(x + 0.5 - 24, y + 0.5 - 24);
    const ring = Math.min(cover(r - 20), cover(10 - r));
    const a = Math.max(ring, cover(r - 5));
    return [0, 0, 0, Math.round(255 * a)];
  }),
  'wide.png': () => png(64, 32, (x, y) => {
    const d = Math.hypot((x + 0.5 - 32) / 30, (y + 0.5 - 16) / 14) - 1;
    return [0, 0, 0, Math.round(255 * cover(d * 14))];
  }),
  'lum.png': () => png(64, 64, (x, y) => {
    if (x < 32) {
      const v = Math.round(255 * (1 - x / 31));
      return [v, v, v, 255];
    }
    return [[255, 0, 0, 255], [0, 255, 0, 255], [0, 0, 255, 255], [255, 255, 255, 255]][Math.floor((x - 32) / 8)];
  }),
  'checker.png': () => png(16, 16, (x, y) => [0, 0, 0, ((x >> 2) + (y >> 2)) % 2 ? 0 : 255]),
  'stripes.png': () => png(128, 128, (x, y) => [0, 0, 0, x % 2 ? 0 : 255]),
};
for (const [name, make] of Object.entries(images)) {
  if (!existsSync(resolve(directory, name))) writeFileSync(resolve(directory, name), make());
}
const checker = 'data:image/png;base64,' + readFileSync(resolve(directory, 'checker.png')).toString('base64');

// Each case: a box of 100 x 100 px content, 8 px padding, a 6 px border (128 px border box) at the top left of a cell,
// a child that overflows it at its bottom right (opaque over the opaque box: a mask of the box's group, not of each
// item, leaves no box showing through the child), with its mask (and more style). Cells are 170 px apart.
const cell = 170;
const box = 'position:absolute;box-sizing:content-box;width:100px;height:100px;padding:8px;border:6px solid #f59e0b;' +
  'background:#3b82f6;';
const solid = 'linear-gradient(#000, #000)';
const cases = [
  ['none', ''],
  ['linear to bottom', 'mask-image:linear-gradient(to bottom, #000, transparent)'],
  ['webkit linear 90deg', '-webkit-mask-image:linear-gradient(90deg, #000 30%, transparent 70%)'],
  ['radial circle', 'mask-image:radial-gradient(circle, #000 40%, transparent 41%)'],
  ['radial ellipse at', 'mask-image:radial-gradient(ellipse at 30% 30%, #000, transparent 70%)'],
  ['repeating linear', 'mask-image:repeating-linear-gradient(45deg, #000 0 10px, transparent 10px 20px)'],
  ['conic', 'mask-image:conic-gradient(#000, transparent)'],
  ['tiled dots', 'mask-image:radial-gradient(circle, #000 50%, transparent 52%);mask-size:32px 32px'],
  ['one dot centered', 'mask-image:radial-gradient(circle, #000 60%, transparent 62%);mask-size:48px 48px;' +
    'mask-repeat:no-repeat;mask-position:center'],
  ['repeat-x', 'mask-image:radial-gradient(circle, #000 60%, transparent 62%);mask-size:30px 30px;' +
    'mask-repeat:repeat-x'],
  ['corner square', `mask-image:${solid};mask-size:50% 50%;mask-repeat:no-repeat;mask-position:right bottom`],
  ['luminance white to black', 'mask-image:linear-gradient(#fff, #000);mask-mode:luminance'],
  ['luminance colors', 'mask-image:linear-gradient(90deg, #f00, #0f0, #00f);mask-mode:luminance'],
  ['translucent stops', 'mask-image:linear-gradient(rgba(0,0,0,.2), rgba(0,0,0,1))'],
  ['origin content-box', 'mask-image:radial-gradient(circle, #000 70%, transparent 72%);mask-origin:content-box;' +
    'mask-repeat:no-repeat'],
  ['clip content-box', `mask-image:${solid};mask-clip:content-box`],
  ['clip padding-box', `mask-image:${solid};mask-clip:padding-box`],
  ['no-clip', `mask-image:${solid};mask-clip:no-clip`],
  ['two layers add', 'mask-image:linear-gradient(to right, #000, transparent), linear-gradient(to bottom, #000, transparent)'],
  ['subtract', 'mask-image:linear-gradient(to right, #000, transparent), linear-gradient(to bottom, #000, transparent);' +
    'mask-composite:subtract'],
  ['intersect', 'mask-image:linear-gradient(to right, #000, transparent), linear-gradient(to bottom, #000, transparent);' +
    'mask-composite:intersect'],
  ['exclude border trick', `mask:${solid} content-box, ${solid};mask-composite:exclude`],
  ['webkit composite xor', `-webkit-mask:${solid} content-box, ${solid};-webkit-mask-composite:xor`],
  ['shorthand', 'mask:radial-gradient(circle, #000 60%, transparent 62%) center / 50% 50% no-repeat'],
  ['webkit shorthand', '-webkit-mask:linear-gradient(#000, transparent) 0 0 / 100% 50% repeat-y'],
  ['opacity', 'mask-image:linear-gradient(to bottom, #000, transparent);opacity:.5'],
  ['rounded', 'mask-image:linear-gradient(to right, #000, transparent);border-radius:30px'],
  ['rotated', 'mask-image:radial-gradient(circle, #000 50%, transparent 70%);transform:rotate(15deg)'],
  ['with clip-path', 'mask-image:linear-gradient(to right, #000, transparent);clip-path:circle(50%)'],
  ['nested', 'mask-image:linear-gradient(to bottom, #000, transparent)', 'mask-image:radial-gradient(circle, #000 30%, transparent 70%)'],
  ['image repeat', 'mask-image:url(icon.png)'],
  ['image contain', 'mask:url(icon.png) center / contain no-repeat'],
  ['image small', 'mask:url(icon.png) center / 24px no-repeat'],
  ['webkit image stretched', '-webkit-mask-image:url(icon.png);-webkit-mask-size:100% 100%'],
  ['image luminance', 'mask:url(lum.png) 0 0 / 100% 100% no-repeat;mask-mode:luminance'],
  ['image alpha opaque', 'mask:url(lum.png) center / 50% no-repeat'],
  ['image repeat-x', 'mask:url(checker.png) 0 0 / 32px 32px repeat-x'],
  ['image intrinsic tiles', 'mask:url(checker.png)'],
  ['image and gradient', 'mask:url(icon.png) center / 64px no-repeat, linear-gradient(to right, #000, transparent)'],
  ['gradient minus image', 'mask:linear-gradient(#000, #000), url(icon.png) center / 96px no-repeat;mask-composite:exclude'],
  ['image cover', 'mask:url(wide.png) center / cover no-repeat'],
  ['image contain wide', 'mask:url(wide.png) center / contain no-repeat'],
  ['image one size', 'mask:url(wide.png) center / 100px no-repeat'],
  ['image rotated', 'mask:url(icon.png) center / contain no-repeat;transform:rotate(20deg)'],
  ['image data url', `mask-image:url("${checker}")`],
  ['image downscaled', 'mask:url(icon.png) center / 17px no-repeat'],
  ['image tiles downscaled', 'mask:url(checker.png) 0 0 / 7px 7px'],
  ['image stripes downscaled', 'mask:url(stripes.png) center / 50px no-repeat'],
].map(([name, style, child = null], i) => ({name, style, child, rect: [(i % 5) * cell, Math.floor(i / 5) * cell, cell, cell]}));
const width = 5 * cell;
const height = Math.ceil(cases.length / 5) * cell;
// Points to press in each cell: a 7 x 7 grid over it, off the grid of the shapes' round numbers.
const offsets = [7, 29, 51, 73, 97, 119, 141];
const points = cases.flatMap((c, i) => offsets.flatMap((y) => offsets.map((x) => [i, c.rect[0] + x + 0.5, c.rect[1] + y + 0.5])));
writeFileSync(resolve(directory, 'cases.json'), JSON.stringify({width, height, cases, points}, null, 1) + '\n');
writeFileSync(resolve(directory, 'page.js'), `// Generated by tools/browser_mask_image_reference.mjs: the cases of cases.json in #root. A press logs the id of the
// element it reaches in window.__hits.
const cases = ${JSON.stringify(cases.map((c) => [c.rect[0], c.rect[1], c.style, c.child]))};
const box = ${JSON.stringify(box)};
const root = document.getElementById('root');
cases.forEach(([left, top, style, child], i) => {
  const host = document.createElement('div');
  host.setAttribute('style', 'position:absolute;left:' + left + 'px;top:' + top + 'px;width:${cell}px;height:${cell}px');
  host.id = 'cell' + i;
  const e = document.createElement('div');
  e.id = 'case' + i;
  e.setAttribute('style', box + 'left:0;top:0;' + style);
  const inner = document.createElement('div');
  inner.id = 'child' + i;
  inner.setAttribute('style', 'position:absolute;left:80px;top:80px;width:64px;height:64px;background:#ef4444;' +
    (child ? child : ''));
  e.appendChild(inner);
  host.appendChild(e);
  root.appendChild(host);
});
window.__hits = [];
document.addEventListener('mousedown', (event) => window.__hits.push(event.target.id || event.target.tagName));
`);

const page = '<!doctype html><html><head><meta charset=utf-8><style>html,body{margin:0;background:#ffffff}' +
  '#root{position:relative}</style></head><body><div id=root></div><script src=page.js></script></body></html>';
const files = {'page.html': Buffer.from(page), 'page.js': readFileSync(resolve(directory, 'page.js'))};
for (const name of Object.keys(images)) files[name] = readFileSync(resolve(directory, name));
const server = createServer((request, response) => {
  const name = decodeURIComponent(new URL(request.url, 'http://localhost').pathname.slice(1));
  const body = files[name];
  if (!body) {
    response.writeHead(404);
    response.end();
    return;
  }
  response.writeHead(200, {'content-type': name.endsWith('.html') ? 'text/html' : name.endsWith('.png') ? 'image/png' :
    'text/javascript'});
  response.end(body);
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const origin = `http://127.0.0.1:${server.address().port}/`;

const profile = mkdtempSync(join(tmpdir(), 'weft-mask-image-'));
const child = spawn(executable, ['--headless=new', '--no-first-run', '--disable-background-networking',
  '--remote-debugging-port=0', '--force-device-scale-factor=1', '--force-color-profile=srgb',
  `--window-size=${width},${height}`, '--user-data-dir=' + profile, 'about:blank'],
{windowsHide: true, stdio: ['ignore', 'ignore', 'pipe']});
let socket;
try {
  const endpoint = await new Promise((resolveEndpoint, reject) => {
    let text = '';
    const limit = setTimeout(() => reject(new Error('Chromium startup timeout')), 20000);
    child.on('error', reject);
    child.stderr.on('data', (c) => {
      text += c;
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
  const loaded = [];
  socket.addEventListener('message', (event) => {
    const message = JSON.parse(event.data);
    if (message.id && pending.has(message.id)) {
      const {resolve: done, reject} = pending.get(message.id);
      pending.delete(message.id);
      message.error ? reject(new Error(message.error.message)) : done(message.result);
    } else if (message.method === 'Page.loadEventFired') {
      loaded.shift()?.();
    }
  });
  const call = (method, params = {}) => new Promise((done, reject) => {
    const id = ++next;
    pending.set(id, {resolve: done, reject});
    socket.send(JSON.stringify({id, method, params}));
  });
  await call('Page.enable');
  await call('Emulation.setDeviceMetricsOverride', {width, height, deviceScaleFactor: 1, mobile: false});
  const ready = new Promise((r) => loaded.push(r));
  await call('Page.navigate', {url: origin + 'page.html'});
  await ready;
  await call('Runtime.evaluate', {expression: 'new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)))',
    awaitPromise: true});
  const shot = await call('Page.captureScreenshot', {format: 'png', clip: {x: 0, y: 0, width, height, scale: 1}});
  writeFileSync(resolve(directory, 'chrome.png'), Buffer.from(shot.data, 'base64'));
  for (const [, x, y] of points) {
    await call('Input.dispatchMouseEvent', {type: 'mousePressed', x, y, button: 'left', buttons: 1, clickCount: 1});
    await call('Input.dispatchMouseEvent', {type: 'mouseReleased', x, y, button: 'left', buttons: 0, clickCount: 1});
  }
  const hits = (await call('Runtime.evaluate', {expression: 'JSON.stringify(window.__hits)', returnByValue: true}))
    .result.value;
  writeFileSync(resolve(directory, 'hits.json'), JSON.stringify({browser: version.Browser, hits: JSON.parse(hits)}) +
    '\n');
  console.log(`Recorded ${cases.length} cases and ${points.length} presses from ${version.Browser} to ${directory}`);
} finally {
  socket?.close();
  server.close();
  const exited = new Promise((r) => child.once('exit', r));
  child.kill();
  await exited;
  rmSync(profile, {recursive: true, force: true, maxRetries: 5, retryDelay: 200});
}
