// Chrome's dotted and dashed border patterns for tests/scene_border_tests.cpp: one bottom border per row (dotted 1-4 px,
// dashed 1-3 px, over a range of lengths), white over black at device scale 1, printed as the test's table rows ('#' a
// pixel of at least 200 in the border's rows, '+' above 30, '.' the rest). Offline: the page is a data URL.
//
//   node tools/browser_border_patterns.mjs [chrome.exe] > rows.txt
import {spawn} from 'node:child_process';
import {mkdtempSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {inflateSync} from 'node:zlib';

const executable = process.argv[2] ?? 'C:/Program Files/Google/Chrome/Application/chrome.exe';
const rows = [];
for (const [style, width] of [['dotted', 1], ['dotted', 2], ['dotted', 3], ['dotted', 4], ['dashed', 1], ['dashed', 2],
  ['dashed', 3]]) {
  for (const length of [7, 8, 9, 10, 11, 12, 13, 20, 21, 22, 23, 24, 25, 37, 38, 39, 40]) rows.push({style, width, length});
}
const rowHeight = 12;
const page = '<!doctype html><html><head><style>*{margin:0;padding:0;box-sizing:border-box}body{background:#000}' +
  '.r{position:absolute;left:10px}</style></head><body>' +
  rows.map((r, i) => `<div class=r style="top:${i * rowHeight}px;width:${r.length}px;height:${r.width + 4}px;` +
    `border-bottom:${r.width}px ${r.style} #fff"></div>`).join('') + '</body></html>';
const height = rows.length * rowHeight + 10;

// An 8-bit RGB(A) PNG's pixels.
function decode(png) {
  let at = 8;
  let width = 0;
  let h = 0;
  let channels = 0;
  const data = [];
  while (at < png.length) {
    const length = png.readUInt32BE(at);
    const type = png.toString('latin1', at + 4, at + 8);
    const body = png.subarray(at + 8, at + 8 + length);
    if (type === 'IHDR') {
      width = body.readUInt32BE(0);
      h = body.readUInt32BE(4);
      channels = {2: 3, 6: 4}[body[9]];
    } else if (type === 'IDAT') {
      data.push(body);
    }
    at += 12 + length;
  }
  const raw = inflateSync(Buffer.concat(data));
  const stride = width * channels;
  const out = Buffer.alloc(h * stride);
  for (let y = 0; y < h; ++y) {
    const filter = raw[y * (stride + 1)];
    for (let x = 0; x < stride; ++x) {
      const v = raw[y * (stride + 1) + 1 + x];
      const a = x >= channels ? out[y * stride + x - channels] : 0;
      const b = y ? out[(y - 1) * stride + x] : 0;
      const c = x >= channels && y ? out[(y - 1) * stride + x - channels] : 0;
      let p = 0;
      if (filter === 1) p = a;
      else if (filter === 2) p = b;
      else if (filter === 3) p = (a + b) >> 1;
      else if (filter === 4) {
        const pa = Math.abs(b - c);
        const pb = Math.abs(a - c);
        const pc = Math.abs(a + b - 2 * c);
        p = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
      }
      out[y * stride + x] = (v + p) & 255;
    }
  }
  return {width, channels, pixels: out};
}

const profile = mkdtempSync(join(tmpdir(), 'weft-border-patterns-'));
const child = spawn(executable, ['--headless=new', '--no-first-run', '--remote-debugging-port=0', '--force-device-scale-factor=1',
  `--window-size=100,${height}`, '--user-data-dir=' + profile, 'about:blank'], {windowsHide: true, stdio: ['ignore', 'ignore', 'pipe']});
let socket;
try {
  const endpoint = await new Promise((resolveEndpoint, reject) => {
    let text = '';
    const limit = setTimeout(() => reject(new Error('Chromium startup timeout')), 20000);
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
  await new Promise((r) => socket.addEventListener('open', r, {once: true}));
  let next = 0;
  const pending = new Map();
  socket.addEventListener('message', (event) => {
    const message = JSON.parse(event.data);
    if (message.id && pending.has(message.id)) {
      pending.get(message.id)(message.result);
      pending.delete(message.id);
    }
  });
  const call = (method, params = {}) => new Promise((done) => {
    const id = ++next;
    pending.set(id, done);
    socket.send(JSON.stringify({id, method, params}));
  });
  await call('Page.enable');
  await call('Emulation.setDeviceMetricsOverride', {width: 100, height, deviceScaleFactor: 1, mobile: false});
  const loaded = new Promise((r) => socket.addEventListener('message', (e) => {
    if (JSON.parse(e.data).method === 'Page.loadEventFired') r();
  }));
  await call('Page.navigate', {url: 'data:text/html;charset=utf-8,' + encodeURIComponent(page)});
  await loaded;
  const shot = await call('Page.captureScreenshot', {format: 'png'});
  const image = decode(Buffer.from(shot.data, 'base64'));
  console.log(`            // ${version.Browser}`);
  rows.forEach((r, i) => {
    const top = i * rowHeight + 4;
    let line = '';
    for (let x = 10; x < 10 + r.length; ++x) {
      let v = 0;
      for (let y = top; y < top + r.width; ++y) v = Math.max(v, image.pixels[(y * image.width + x) * image.channels]);
      line += v > 200 ? '#' : v > 30 ? '+' : '.';
    }
    console.log(`            {"${r.style}", ${r.width}, ${r.length}, "${line}"},`);
  });
} finally {
  socket?.close();
  child.kill();
}
