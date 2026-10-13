// Chrome oracle for scene dotted and dashed borders (11.3c): boxes and an inline element with dotted and dashed sides
// of several widths and lengths, on a dark page, captured at device scale 1 and 2. Writes the case list and the two
// screenshots; the native test lays out the same boxes and compares the border pixels. Offline: the page is a data URL.
// Each scale is its own browser started at that device scale factor, which lays the page out zoomed as a HiDPI screen
// does (emulating the scale over DevTools instead magnifies the page drawn at scale 1: dots laid out in CSS px).
//
//   node tools/browser_scene_border_reference.mjs [output directory] [chrome.exe]
import {spawn} from 'node:child_process';
import {writeFileSync, mkdirSync, mkdtempSync, existsSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {resolve, join} from 'node:path';
import {repo} from './gallery_runtime.mjs';

const output = resolve(process.argv[2] ?? resolve(repo, 'tests/fixtures/scene-border-reference'));
const executable = process.argv[3] ?? 'C:/Program Files/Google/Chrome/Application/chrome.exe';
if (!existsSync(executable)) throw new Error('Provide a local Chromium executable as the second argument');

const width = 400;
const height = 300;
// Each case: a box at (x, y) of w x h with the style; or an inline element in a text line.
const cases = [
  {name: 'dotted 1 bottom odd', x: 10, y: 10, w: 37, h: 12, style: 'border-bottom:1px dotted #d4a843'},
  {name: 'dotted 1 bottom even', x: 60, y: 10, w: 38, h: 12, style: 'border-bottom:1px dotted #d4a843'},
  {name: 'dotted 1 all', x: 110, y: 10, w: 40, h: 20, style: 'border:1px dotted #e0e0e0'},
  {name: 'dotted 2 bottom', x: 170, y: 10, w: 50, h: 12, style: 'border-bottom:2px dotted #60a5fa'},
  {name: 'dotted 3 bottom', x: 240, y: 10, w: 51, h: 12, style: 'border-bottom:3px dotted #60a5fa'},
  {name: 'dotted 4 bottom', x: 310, y: 10, w: 60, h: 12, style: 'border-bottom:4px dotted #c084fc'},
  {name: 'dashed 1 bottom', x: 10, y: 60, w: 50, h: 12, style: 'border-bottom:1px dashed #d4a843'},
  {name: 'dashed 2 all', x: 80, y: 50, w: 60, h: 30, style: 'border:2px dashed #e0e0e0'},
  {name: 'dashed 3 bottom', x: 160, y: 60, w: 61, h: 12, style: 'border-bottom:3px dashed #60a5fa'},
  {name: 'dotted 1 left', x: 240, y: 50, w: 12, h: 41, style: 'border-left:1px dotted #d4a843'},
  {name: 'mixed sides', x: 280, y: 50, w: 70, h: 30,
    style: 'border-top:1px dotted #d4a843;border-bottom:2px dashed #60a5fa;border-left:1px solid #e0e0e0'},
  // Off the pixel grid: the box snaps to whole device pixels before the pattern is laid out.
  {name: 'dotted 1 fractional', x: 10.5, y: 95.25, w: 40.5, h: 10, style: 'border-bottom:1px dotted #e0e0e0'},
  {name: 'dashed 2 fractional', x: 70.25, y: 95.5, w: 45.5, h: 10, style: 'border-bottom:2px dashed #e0e0e0'},
  {name: 'dotted 3 right', x: 140, y: 90, w: 12, h: 25, style: 'border-right:3px dotted #c084fc'},
  {name: 'dashed 1 all with background', x: 170, y: 90, w: 50, h: 20,
    style: 'border:1px dashed #e0e0e0;background-color:#3a2a10'},
  // Round dots over a long side: each dot's place along it, where the pattern's period accumulates.
  {name: 'dotted 4 long', x: 10, y: 200, w: 381, h: 12, style: 'border-bottom:4px dotted #c084fc'},
];
// A keyword-like span at a fractional x in a line of text: a 1px dotted bottom border in currentColor.
const inline = {name: 'inline keyword', x: 10, y: 120, w: 380, h: 24,
  style: 'font-family:system-ui;font-size:14px;line-height:1.625;color:#d4d4d8',
  html: 'Deals <span style="border-bottom:1px dotted currentColor;color:#fb923c;font-weight:600">Fire Damage</span> on hit.'};

const page = '<!doctype html><html><head><meta charset=utf-8><style>*{box-sizing:border-box;margin:0;padding:0}' +
  `body{margin:0;width:${width}px;height:${height}px;background:#101010;position:relative;overflow:hidden}` +
  '.c{position:absolute}</style></head><body>' +
  cases.map((c) => `<div class=c style="left:${c.x}px;top:${c.y}px;width:${c.w}px;height:${c.h}px;${c.style}"></div>`)
    .join('') +
  `<div class=c style="left:${inline.x}px;top:${inline.y}px;width:${inline.w}px;${inline.style}">${inline.html}</div>` +
  '</body></html>';

// The page at one device scale factor in its own browser; returns the screenshot and the browser's version.
async function capture(scale) {
  const profile = mkdtempSync(join(tmpdir(), 'weft-borders-'));
  const child = spawn(executable, ['--headless=new', '--no-first-run', '--disable-background-networking',
    '--remote-debugging-port=0', `--force-device-scale-factor=${scale}`, `--window-size=${width},${height}`,
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
    // The viewport exactly the page's size; a scale factor of 0 keeps the browser's own.
    await call('Emulation.setDeviceMetricsOverride', {width, height, deviceScaleFactor: 0, mobile: false});
    const loaded = new Promise((r) => socket.addEventListener('message', (e) => {
      if (JSON.parse(e.data).method === 'Page.loadEventFired') r();
    }));
    await call('Page.navigate', {url: 'data:text/html;charset=utf-8,' + encodeURIComponent(page)});
    await loaded;
    await call('Runtime.evaluate', {expression: 'document.fonts.ready', awaitPromise: true});
    const viewport = await call('Runtime.evaluate', {expression: '[devicePixelRatio, innerWidth, innerHeight]',
      returnByValue: true});
    const [ratio, innerWidth, innerHeight] = viewport.result.value;
    if (ratio !== scale || innerWidth !== width || innerHeight !== height) {
      throw new Error(`viewport ${innerWidth}x${innerHeight} at ${ratio}, expected ${width}x${height} at ${scale}`);
    }
    const shot = await call('Page.captureScreenshot', {format: 'png', clip: {x: 0, y: 0, width, height, scale: 1},
      captureBeyondViewport: false});
    return {png: Buffer.from(shot.data, 'base64'), browser: version.Browser};
  } finally {
    socket?.close();
    child.kill();
  }
}

mkdirSync(output, {recursive: true});
let browser = '';
for (const scale of [1, 2]) {
  const shot = await capture(scale);
  writeFileSync(join(output, `borders-${scale}.png`), shot.png);
  browser = shot.browser;
}
writeFileSync(join(output, 'cases.json'), JSON.stringify({browser, width, height, cases, inline}, null, 1) + '\n');
console.log(`Recorded ${cases.length + 1} border cases at scale 1 and 2 from ${browser} to ${output}`);
