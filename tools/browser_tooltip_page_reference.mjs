// Real Chromium reference for the nested tooltip page (11.3e) via CDP: the page's reference bundle
// (TOOLTIP_REFERENCE.js: the captured component with the original canvas smoke) in the pinned Tailwind gallery.css,
// body as the page's root over the gallery's backdrop, at device scale 1 in the case's viewport. Clocks:
// probes/page_reference_clock.js (timers, frames and CSS animations, an animation starting when it first appears)
// and probes/cluster_reference_clock.js (the scripted pointer). Steps the frames of examples/crpg-gallery/tooltip-cases.json
// with its inputs and captures, at its capture frames, a screenshot, every element's bounding rect and the computed
// style it paints with, and the page's state. Writes tests/fixtures/tooltip-reference with a manifest of what it used. Offline: local files only; the smoke's
// textures are the captured bitmaps.
//
//   node tools/browser_tooltip_page_reference.mjs [output directory] [chrome.exe]
import {createServer} from 'node:http';
import {spawn} from 'node:child_process';
import {readFileSync, writeFileSync, mkdirSync, mkdtempSync, existsSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {resolve, extname, join, relative} from 'node:path';
import {repo, sha} from './gallery_runtime.mjs';

const directory = resolve(repo, 'examples/crpg-gallery/native');
const textures = resolve(repo, 'tests/fixtures/crpg-gallery-assets/UI/public');
const specPath = resolve(repo, 'examples/crpg-gallery/tooltip-cases.json');
const spec = JSON.parse(readFileSync(specPath, 'utf8'));
const group = spec.groups[0];
const out = resolve(process.argv[2] ?? resolve(repo, 'tests/fixtures/tooltip-reference'));
const executable = process.argv[3] ?? 'C:/Program Files/Google/Chrome/Application/chrome.exe';
if (!existsSync(executable)) throw new Error('Provide a local Chromium executable as the second argument');
const clocks = ['probes/page_reference_clock.js', 'probes/cluster_reference_clock.js'];
// body is the page's root: the gallery's backdrop, as the native session sets it. gallery.css ends with the fixed
// gallery's rules for #root (800 x 600, its own background), which this page does not use: #root fills body, as the
// page's own rule has it.
const page = '<!doctype html><html><head><meta charset=utf-8><link rel=stylesheet href=gallery.css></head>' +
  '<body style="margin:0;position:relative;overflow:hidden;background-color:#050403">' +
  '<div id=root style="position:static;width:100%;height:100%;background:transparent"></div>' +
  '<script src=platform.js></script><script src=TOOLTIP_REFERENCE.js></script></body></html>';
const mime = {'.html': 'text/html', '.js': 'application/javascript', '.css': 'text/css', '.png': 'image/png'};
const used = new Set();
const server = createServer((request, response) => {
  const name = new URL(request.url, 'http://localhost').pathname.slice(1) || 'tooltip.html';
  if (!/^[A-Za-z0-9_.-]+$/.test(name)) {
    response.writeHead(404);
    response.end();
    return;
  }
  try {
    let data;
    if (name === 'tooltip.html') {
      data = Buffer.from(page);
    } else if (name === 'cloud.png' || name === 'tiling-noise.png') {
      data = readFileSync(resolve(textures, name));
      used.add(resolve(textures, name));
    } else {
      data = readFileSync(resolve(directory, name));
      used.add(resolve(directory, name));
    }
    response.writeHead(200, {'Content-Type': mime[extname(name)] || 'text/plain'});
    response.end(data);
  } catch {
    response.writeHead(404);
    response.end();
  }
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const origin = 'http://127.0.0.1:' + server.address().port;
const profile = mkdtempSync(join(tmpdir(), 'weft-tooltip-'));
const child = spawn(executable, ['--headless=new', '--no-first-run', '--disable-background-networking',
  '--remote-debugging-port=0', '--force-device-scale-factor=1', `--window-size=${group.width},${group.height}`,
  '--hide-scrollbars', '--user-data-dir=' + profile, 'about:blank'], {windowsHide: true, stdio: ['ignore', 'ignore', 'pipe']});
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
  const requests = new Map();
  const errors = [];
  const blocked = [];
  const call = (method, params = {}) => new Promise((done, reject) => {
    const id = ++next;
    const timer = setTimeout(() => {
      requests.delete(id);
      reject(new Error('CDP timeout: ' + method));
    }, 20000);
    requests.set(id, {done, reject, timer});
    socket.send(JSON.stringify({id, method, params}));
  });
  socket.addEventListener('message', (event) => {
    const message = JSON.parse(event.data);
    if (message.id) {
      const q = requests.get(message.id);
      if (q) {
        requests.delete(message.id);
        clearTimeout(q.timer);
        message.error ? q.reject(new Error(JSON.stringify(message.error))) : q.done(message.result);
      }
      return;
    }
    if (message.method === 'Runtime.exceptionThrown') {
      errors.push(message.params.exceptionDetails.exception?.description ?? message.params.exceptionDetails.text);
    }
    if (message.method === 'Runtime.consoleAPICalled' && message.params.type === 'error') {
      errors.push(message.params.args.map((a) => a.value ?? a.description).join(' '));
    }
    if (message.method === 'Fetch.requestPaused') {
      const p = message.params;
      if (new URL(p.request.url).origin === origin) {
        call('Fetch.continueRequest', {requestId: p.requestId}).catch(() => {});
      } else {
        blocked.push(p.request.url);
        call('Fetch.failRequest', {requestId: p.requestId, errorReason: 'BlockedByClient'}).catch(() => {});
      }
    }
  });
  const evaluate = async (expression) => {
    const r = await call('Runtime.evaluate', {expression, returnByValue: true, awaitPromise: true});
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
    return r.result.value;
  };
  const wait = (ms) => new Promise((r) => setTimeout(r, ms));
  await call('Runtime.enable');
  await call('Page.enable');
  await call('Emulation.setDeviceMetricsOverride', {width: group.width, height: group.height, deviceScaleFactor: 1,
    mobile: false});
  await call('Fetch.enable', {patterns: [{urlPattern: 'http*', requestStage: 'Request'}]});
  await call('Page.addScriptToEvaluateOnNewDocument', {source: clocks.map((c) => readFileSync(resolve(repo, c), 'utf8'))
    .join('\n')});
  await call('Page.navigate', {url: origin + '/tooltip.html'});
  for (let i = 0; i < 100 && !await evaluate("typeof __ceGallery !== 'undefined'"); ++i) await wait(20);
  if (!await evaluate("typeof __ceGallery !== 'undefined'")) throw new Error('bundle did not load: ' + errors.join('; '));
  await evaluate(`__ceGallery.mount('${group.name}',0)`);
  // React's tasks and the smoke's texture loads finish in real time while the document clock stays at 0: the loader's
  // hidden images leave body once both have loaded.
  for (let i = 0; i < 200; ++i) {
    await evaluate('__ceReference.settle()');
    if (i > 30 && await evaluate("document.querySelectorAll('body > img').length === 0 && " +
      "document.querySelectorAll('canvas').length === 3")) break;
    await wait(5);
  }
  if (await evaluate("document.querySelectorAll('body > img').length") !== 0) throw new Error('smoke textures did not load');
  for (let i = 0; i < 10; ++i) await evaluate('__ceReference.settle()');
  await evaluate('__ceReference.step(0)');
  // The computed style an element paints with: its colors as the 8-bit sRGB a canvas draws them in (Chrome serializes
  // the page's colors as oklch and oklab), opacity, its borders' widths, styles and colors, and its font's weight and
  // style.
  await evaluate(`(() => {
    const canvas = document.createElement('canvas');
    canvas.width = canvas.height = 1;
    const context = canvas.getContext('2d', {willReadFrequently: true});
    const rgba = (value) => {
      if (!CSS.supports('color', value)) throw new Error('color ' + value);
      context.clearRect(0, 0, 1, 1);
      context.fillStyle = value;
      context.fillRect(0, 0, 1, 1);
      return [...context.getImageData(0, 0, 1, 1).data];
    };
    const sides = ['top', 'right', 'bottom', 'left'];
    globalThis.__cePainted = (n) => {
      const s = getComputedStyle(n);
      return {background: rgba(s.backgroundColor), color: rgba(s.color), opacity: Number(s.opacity),
        border_widths: sides.map((k) => parseFloat(s.getPropertyValue('border-' + k + '-width'))),
        border_styles: sides.map((k) => s.getPropertyValue('border-' + k + '-style')),
        border_colors: sides.map((k) => rgba(s.getPropertyValue('border-' + k + '-color'))),
        font_weight: Number(s.fontWeight), font_style: s.fontStyle};
    };
  })()`);
  const id = group.prefix + '-00';
  mkdirSync(out, {recursive: true});
  const captures = [];
  const trace = [];
  for (let frame = 0; frame < spec.frames; ++frame) {
    const time = frame * (1000 / 60);
    await evaluate(`__ceReference.step(${time})`);
    for (const input of (spec.inputs[id] || []).filter((i) => i.frame === frame)) {
      trace.push({frame, ...await evaluate('__ceClusterInput(' + JSON.stringify(input) + ')')});
      await evaluate(`__ceReference.step(${time})`);
      await wait(1);
      await evaluate(`__ceReference.step(${time})`);
    }
    if (spec.capture_frames.includes(frame)) {
      await evaluate(`__ceReference.styles(${time})`);
      const geometry = await evaluate(`[...document.body.querySelectorAll('*')].filter(n=>!n.closest('defs')&&n.localName!=='script').map(n=>{const r=n.getBoundingClientRect();return {tag:n.localName,rect:[r.x,r.y,r.width,r.height],style:__cePainted(n)}})`);
      writeFileSync(resolve(out, `${id}-${frame}-geometry.json`), JSON.stringify(geometry, null, 1) + '\n');
      const shot = await call('Page.captureScreenshot', {format: 'png', captureBeyondViewport: false, fromSurface: true});
      writeFileSync(resolve(out, `${id}-${frame}.png`), Buffer.from(shot.data, 'base64'));
      const state = await evaluate('({gallery:__ceGallery.state(),body:document.body.children.length,' +
        "pinned:document.querySelectorAll('[data-pinned-tooltip]').length})");
      captures.push({frame, time, state});
    }
  }
  if (errors.length || blocked.length) throw new Error(JSON.stringify({errors, blocked}));
  const files = {};
  for (const p of [...used, specPath, resolve(repo, 'tools/browser_tooltip_page_reference.mjs'),
    ...clocks.map((c) => resolve(repo, c))].sort()) {
    files[relative(repo, p).replaceAll('\\', '/')] = sha(readFileSync(p));
  }
  writeFileSync(resolve(out, 'report.json'), JSON.stringify({format: 'gallery-tooltip-reference-v1',
    browser: version.Browser, dpr: 1, id, width: group.width, height: group.height, frames: spec.frames, captures,
    input_trace: trace, source_files: files}, null, 1) + '\n');
  console.log(`Recorded ${captures.length} captures of ${id} from ${version.Browser} to ${out}`);
} finally {
  socket?.close();
  child.kill();
  await new Promise((r) => server.close(r));
}
