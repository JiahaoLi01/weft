// Chrome reference for the KaTeX page (examples/katex): KaTeX 0.16.47 (third_party/katex) renders the page's formulas
// with output 'html' into a #root of the page's size, its stylesheet katex.css then page.css, the scene's box sizing
// and borders as its UA sheet has them, its fonts the TrueType files beside katex.css (Chrome passes over the woff2
// and woff sources it cannot find). Records screenshots at device scale 1 and 2, text antialiased in grayscale as weft
// draws it (--disable-lcd-text: the comparison sees where glyphs are and their shapes, not Chrome's subpixel color
// fringes), and at each scale every element's bounding client rect under #root (tree order, with its tag and class):
// Chrome lays a page out at its device scale, so its 2x rects are not its 1x ones. With the inputs' SHA-256 (text files
// with LF line ends, as weft's test reads them), so that a changed input without a new capture fails
// (tests/katex_reference_tests.cpp). Offline: the files are served from this repository.
//
//   node tools/browser_katex_reference.mjs [output directory, tests/fixtures/katex-reference] [chrome.exe]
import {createServer} from 'node:http';
import {spawn} from 'node:child_process';
import {readFileSync, writeFileSync, mkdirSync, mkdtempSync, existsSync, readdirSync} from 'node:fs';
import {createHash} from 'node:crypto';
import {tmpdir} from 'node:os';
import {resolve, extname, join} from 'node:path';
import {repo} from './gallery_runtime.mjs';

const out = resolve(process.argv[2] ?? resolve(repo, 'tests/fixtures/katex-reference'));
const executable = process.argv[3] ?? 'C:/Program Files/Google/Chrome/Application/chrome.exe';
if (!existsSync(executable)) throw new Error('Provide a local Chromium executable as the second argument');
const width = 1000;
const height = 1552;
const inputs = ['third_party/katex/katex.min.js', 'third_party/katex/katex.css', 'examples/katex/page.js',
  'examples/katex/page.css',
  ...readdirSync(resolve(repo, 'third_party/katex/fonts')).sort().map((f) => 'third_party/katex/fonts/' + f)];
const digest = (path) => {
  let bytes = readFileSync(resolve(repo, path));
  if (/\.(js|css)$/.test(path)) bytes = Buffer.from(bytes.toString('utf8').replace(/\r\n/g, '\n'), 'utf8');
  return createHash('sha256').update(bytes).digest('hex');
};

const page = '<!doctype html><html><head><meta charset=utf-8><style>*{box-sizing:border-box;border-style:solid;' +
  `border-width:0}html,body{margin:0;width:${width}px;height:${height}px;overflow:hidden}</style>` +
  '<link rel=stylesheet href="/third_party/katex/katex.css"><link rel=stylesheet href="/examples/katex/page.css">' +
  `</head><body><div id=root style="width:${width}px;height:${height}px"></div>` +
  '<script src="/third_party/katex/katex.min.js"></script><script src="/examples/katex/page.js"></script>' +
  '</body></html>';
const mime = {'.js': 'application/javascript', '.css': 'text/css', '.ttf': 'font/ttf'};
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
const measure = `JSON.stringify([...document.getElementById('root').querySelectorAll('*')].map((e) => {
  const r = e.getBoundingClientRect();
  return [e.localName, e.getAttribute('class') || '', r.left, r.top, r.width, r.height];
}))`;

const capture = async (scale) => {
  const profile = mkdtempSync(join(tmpdir(), 'weft-katex-'));
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
    const rects = JSON.parse((await call('Runtime.evaluate', {expression: measure, returnByValue: true})).result.value);
    const shot = await call('Page.captureScreenshot', {format: 'png', clip: {x: 0, y: 0, width, height, scale: 1}});
    writeFileSync(join(out, `chrome-${scale}x.png`), Buffer.from(shot.data, 'base64'));
    return {browser: version.Browser, rects};
  } finally {
    socket?.close();
    child.kill();
  }
};

try {
  mkdirSync(out, {recursive: true});
  const one = await capture(1);
  const two = await capture(2);
  const reference = {browser: one.browser, width, height, scales: [1, 2],
    inputs: Object.fromEntries(inputs.map((p) => [p, digest(p)])), rects: one.rects, scaled_rects: {2: two.rects}};
  writeFileSync(join(out, 'reference.json'), JSON.stringify(reference) + '\n');
  console.log(`Recorded ${one.rects.length} element rects and screenshots at 1x and 2x from ${one.browser} to ${out}`);
} finally {
  server.close();
}
