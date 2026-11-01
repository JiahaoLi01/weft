// Chrome reference for where text at rest lies on the device's pixels (tests/text_phase_tests.cpp): the page of
// tests/fixtures/text-phase-reference ("H" at 16 phases of a CSS px, down and across) on a #root of its size, with the
// scene's box sizing and borders as its UA sheet has them, screenshot at device scale 1 and 2, text antialiased in
// grayscale as weft draws it (--disable-lcd-text), with the inputs' SHA-256 (LF line ends, as weft's test reads them)
// so that a changed input without a new capture fails (tools/verify_text_phase_fixture.py). Offline: the files are
// served from this repository; the font is this machine's Segoe UI, as for the scene's text.
//
//   node tools/browser_text_phase_reference.mjs [output directory, tests/fixtures/text-phase-reference] [chrome.exe]
import {createServer} from 'node:http';
import {spawn} from 'node:child_process';
import {readFileSync, writeFileSync, mkdirSync, mkdtempSync, existsSync} from 'node:fs';
import {createHash} from 'node:crypto';
import {tmpdir} from 'node:os';
import {resolve, extname, join} from 'node:path';
import {repo} from './gallery_runtime.mjs';

const fixture = 'tests/fixtures/text-phase-reference';
const out = resolve(process.argv[2] ?? resolve(repo, fixture));
const executable = process.argv[3] ?? 'C:/Program Files/Google/Chrome/Application/chrome.exe';
if (!existsSync(executable)) throw new Error('Provide a local Chromium executable as the second argument');
const width = 680;
const height = 110;
const scales = [1, 2];
const inputs = [fixture + '/page.js', fixture + '/page.css'];
const digest = (path) => createHash('sha256')
  .update(Buffer.from(readFileSync(resolve(repo, path)).toString('utf8').replace(/\r\n/g, '\n'), 'utf8')).digest('hex');

const page = '<!doctype html><html><head><meta charset=utf-8><style>*{box-sizing:border-box;border-style:solid;' +
  `border-width:0}html,body{margin:0;width:${width}px;height:${height}px;overflow:hidden}</style>` +
  `<link rel=stylesheet href="/${fixture}/page.css"></head><body>` +
  `<div id=root style="width:${width}px;height:${height}px"></div>` +
  `<script src="/${fixture}/page.js"></script></body></html>`;
const mime = {'.js': 'application/javascript', '.css': 'text/css'};
const server = createServer((request, response) => {
  const name = decodeURIComponent(new URL(request.url, 'http://localhost').pathname.slice(1));
  try {
    const data = name === '' ? Buffer.from(page) : readFileSync(resolve(repo, name));
    response.writeHead(200, {'content-type': name === '' ? 'text/html' : mime[extname(name)] ?? 'application/octet-stream'});
    response.end(data);
  } catch {
    response.writeHead(404);
    response.end();
  }
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const origin = `http://127.0.0.1:${server.address().port}/`;

const capture = async (scale) => {
  const profile = mkdtempSync(join(tmpdir(), 'weft-text-phase-'));
  const child = spawn(executable, ['--headless=new', '--no-first-run', '--disable-background-networking',
    '--remote-debugging-port=0', `--force-device-scale-factor=${scale}`, `--window-size=${width},${height}`,
    '--hide-scrollbars', '--disable-lcd-text', '--user-data-dir=' + profile, 'about:blank'],
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
    await call('Emulation.setDeviceMetricsOverride', {width, height, deviceScaleFactor: scale, mobile: false});
    const loaded = new Promise((r) => socket.addEventListener('message', (e) => {
      if (JSON.parse(e.data).method === 'Page.loadEventFired') r();
    }));
    await call('Page.navigate', {url: origin});
    await loaded;
    await call('Runtime.evaluate', {
      expression: 'document.fonts.ready.then(() => new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r))))',
      awaitPromise: true});
    const shot = await call('Page.captureScreenshot', {format: 'png', clip: {x: 0, y: 0, width, height, scale: 1}});
    writeFileSync(join(out, `chrome-${scale}x.png`), Buffer.from(shot.data, 'base64'));
    return version.Browser;
  } finally {
    socket?.close();
    child.kill();
  }
};

try {
  mkdirSync(out, {recursive: true});
  let browser;
  for (const scale of scales) {
    browser = await capture(scale);
  }
  writeFileSync(join(out, 'reference.json'), JSON.stringify({browser, width, height, scales,
    inputs: Object.fromEntries(inputs.map((p) => [p, digest(p)]))}, null, 1) + '\n');
  console.log(`Recorded the text phase page at ${scales.join('x, ')}x from ${browser} to ${out}`);
} finally {
  server.close();
}
