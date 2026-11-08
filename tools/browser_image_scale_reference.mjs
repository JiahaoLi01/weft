// Chrome oracle for drawing images at other sizes (mipmaps and image-rendering: src/scene/scene_native.cpp's mip
// chains, src/render/shaders/scene.hlsl's fittedTexture): test images of fine detail (one-pixel stripes, a checkerboard,
// a translucent stripe image, a pixel-art image) written to tests/fixtures/image_scale when missing, and a picture
// (photo.png, examples/img/landscape.png); drawn by img elements and element backgrounds at fixed places and sizes,
// smaller and larger than they are, smooth (the default) and pixelated; one screenshot of the page at device scale 1.
// Writes the page's script (page.js, which builds the cases in #root), the cases' places (cases.json) and the
// screenshot (chrome.png), which tests/image_scale_metal_tests.mm compares the Metal pass's drawing of the same page
// with, case by case.
//
//   node tools/browser_image_scale_reference.mjs [chrome executable]
import {spawn} from 'node:child_process';
import {createServer} from 'node:http';
import {existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync} from 'node:fs';
import {deflateSync} from 'node:zlib';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

const repo = resolve(fileURLToPath(new URL('..', import.meta.url)));
const directory = resolve(repo, 'tests/fixtures/image_scale');
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
const images = {
  // Left: one-pixel black and white columns; right: two-pixel red and blue rows; below: a grey ramp and a diagonal.
  'stripes.png': () => png(256, 256, (x, y) => {
    if (y >= 192) return Math.abs(x - (y - 192) * 4) < 1 ? [0, 160, 0, 255] : [x, x, x, 255];
    if (x < 128) return x % 2 ? [255, 255, 255, 255] : [0, 0, 0, 255];
    return (y >> 1) % 2 ? [220, 30, 30, 255] : [30, 30, 220, 255];
  }),
  'checker.png': () => png(64, 64, (x, y) => ((x + y) % 2 ? [255, 255, 255, 255] : [0, 0, 0, 255])),
  // Stripes of color and of alpha: premultiplied averaging keeps a transparent stripe's color out.
  'alpha.png': () => png(128, 128, (x, y) => (x % 2 ? [255, 0, 0, 255] : [0, 255, 0, x < 64 ? 0 : 128])),
  'pixel.png': () => png(8, 8, (x, y) => [x * 36, y * 36, (x + y) % 2 ? 255 : 60, 255]),
};
for (const [name, make] of Object.entries(images)) {
  const path = resolve(directory, name);
  if (!existsSync(path)) writeFileSync(path, make());
}
const photo = resolve(directory, 'photo.png');
if (!existsSync(photo)) writeFileSync(photo, readFileSync(resolve(repo, 'examples/img/landscape.png')));

// Each case: an element (an img of src, or a div whose background image is src) at [left, top, width, height] of #root,
// with more style, and the style of a wrapper around it (an inherited property).
const cases = [];
let x = 0;
let y = 0;
let row = 0;
const add = (name, src, w, h, style = '', more = {}) => {
  if (x + w > 1000) {
    x = 0;
    y += row + 8;
    row = 0;
  }
  cases.push({name, tag: 'img', src, rect: [x, y, w, h], style, ...more});
  x += w + 8;
  row = Math.max(row, h);
};
const pixelated = 'image-rendering:pixelated';
for (const s of [256, 200, 128, 100, 64, 50, 32, 23, 16]) add(`stripes ${s}`, 'stripes.png', s, s);
for (const s of [64, 32, 21, 16, 8]) add(`checker ${s}`, 'checker.png', s, s);
for (const [w, h] of [[240, 160], [120, 80], [97, 65], [60, 40], [30, 20]]) add(`photo ${w}`, 'photo.png', w, h);
for (const s of [128, 64, 40, 16]) add(`alpha ${s}`, 'alpha.png', s, s);
add('pixel 64 auto', 'pixel.png', 64, 64);
add('pixel 64 pixelated', 'pixel.png', 64, 64, pixelated);
add('pixel 61 pixelated', 'pixel.png', 61, 61, pixelated);
add('pixel 64 crisp-edges', 'pixel.png', 64, 64, 'image-rendering:crisp-edges');
add('pixel 5 pixelated', 'pixel.png', 5, 5, pixelated);
add('stripes 100 pixelated', 'stripes.png', 100, 100, pixelated);
add('photo 60 pixelated', 'photo.png', 60, 40, pixelated);
add('pixel 64 smooth', 'pixel.png', 64, 64, 'image-rendering:smooth');
add('pixel 64 high-quality', 'pixel.png', 64, 64, 'image-rendering:high-quality');
add('pixel 64 optimize-contrast', 'pixel.png', 64, 64, 'image-rendering:-webkit-optimize-contrast');
add('pixel 64 inherited', 'pixel.png', 64, 64, '', {parent: pixelated});
add('background stripes 100', 'stripes.png', 100, 100, 'background-size:100px 100px', {tag: 'div'});
add('background stripes tiled 50', 'stripes.png', 150, 100, 'background-size:50px 50px', {tag: 'div'});
add('background pixel pixelated 24', 'pixel.png', 120, 72, 'background-size:24px 24px;' + pixelated, {tag: 'div'});
add('background photo 97', 'photo.png', 97, 65, 'background-size:100% 100%', {tag: 'div'});
const height = y + row + 8;
writeFileSync(resolve(directory, 'cases.json'), JSON.stringify({width: 1000, height, cases}, null, 1) + '\n');
writeFileSync(resolve(directory, 'page.js'), `// Generated by tools/browser_image_scale_reference.mjs: the cases of cases.json in #root.
const cases = ${JSON.stringify(cases.map((c) => [c.tag, c.src, c.rect, c.style, c.parent ?? '']))};
const root = document.getElementById('root');
for (const [tag, src, [left, top, width, height], style, parent] of cases) {
  const e = document.createElement(tag);
  const place = 'position:absolute;display:block;left:' + left + 'px;top:' + top + 'px;width:' + width + 'px;height:' +
    height + 'px;';
  if (tag === 'img') {
    e.setAttribute('src', src);
  }
  e.setAttribute('style', place + (tag === 'img' ? '' : 'background-image:url(' + src + ');') + style);
  if (parent) {
    const wrapper = document.createElement('div');
    wrapper.setAttribute('style', parent);
    wrapper.appendChild(e);
    root.appendChild(wrapper);
  } else {
    root.appendChild(e);
  }
}
`);

// The page the test builds in a View: its stylesheet (image_scale_metal_tests.mm) and page.js.
const page = '<!doctype html><html><head><meta charset=utf-8><style>html,body{margin:0;background:#ffffff}' +
  '#root{position:relative}</style></head><body><div id=root></div><script src=page.js></script></body></html>';
const files = {'page.html': Buffer.from(page)};
for (const name of [...Object.keys(images), 'photo.png', 'page.js']) files[name] = readFileSync(resolve(directory, name));
const server = createServer((request, response) => {
  const name = decodeURIComponent(new URL(request.url, 'http://localhost').pathname.slice(1));
  const body = files[name];
  if (!body) {
    response.writeHead(404);
    response.end();
    return;
  }
  response.writeHead(200, {'content-type': name.endsWith('.html') ? 'text/html' : name.endsWith('.js') ?
    'text/javascript' : 'image/png'});
  response.end(body);
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const origin = `http://127.0.0.1:${server.address().port}/`;

const profile = mkdtempSync(join(tmpdir(), 'weft-image-scale-'));
const child = spawn(executable, ['--headless=new', '--no-first-run', '--disable-background-networking',
  '--remote-debugging-port=0', '--force-device-scale-factor=1', '--force-color-profile=srgb',
  `--window-size=1000,${height}`, '--user-data-dir=' + profile, 'about:blank'],
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
  await call('Emulation.setDeviceMetricsOverride', {width: 1000, height, deviceScaleFactor: 1, mobile: false});
  const ready = new Promise((r) => loaded.push(r));
  await call('Page.navigate', {url: origin + 'page.html'});
  await ready;
  await call('Runtime.evaluate', {expression: 'Promise.all([...document.images].map((i) => i.decode())).then(() => ' +
    'new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r))))', awaitPromise: true});
  const shot = await call('Page.captureScreenshot', {format: 'png', clip: {x: 0, y: 0, width: 1000, height, scale: 1}});
  writeFileSync(resolve(directory, 'chrome.png'), Buffer.from(shot.data, 'base64'));
  writeFileSync(resolve(directory, 'browser.txt'), version.Browser + '\n');
  console.log(`Recorded ${cases.length} cases from ${version.Browser} to ${directory}`);
} finally {
  socket?.close();
  server.close();
  const exited = new Promise((r) => child.once('exit', r));
  child.kill();
  await exited;
  rmSync(profile, {recursive: true, force: true, maxRetries: 5, retryDelay: 200});
}
