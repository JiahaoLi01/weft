// Real Chromium reference for the translucent window page (11.5c) via CDP: GLASS_TESTS.js with its stylesheet and
// keyframes on a #root styled as the gallery card's, at device scale 1 in the case's viewport, the scene's box sizing
// and borders as its UA sheet has them. Clocks: probes/page_reference_clock.js (timers, frames and CSS animations) and
// probes/cluster_reference_clock.js (the scripted pointer). For each case of examples/crpg-gallery/glass-cases.json,
// steps its frames with its inputs and captures, at the capture frames, a screenshot and the page's state (its windows'
// titles and rects). Writes tests/fixtures/glass-reference with a report of the browser, its GPU and the files used.
// Offline: local files only.
//
//   node tools/browser_glass_page_reference.mjs [output directory] [chrome.exe]
import {createServer} from 'node:http';
import {spawn} from 'node:child_process';
import {readFileSync, writeFileSync, mkdirSync, mkdtempSync, existsSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {resolve, extname, join, relative} from 'node:path';
import {repo, sha} from './gallery_runtime.mjs';

const gallery = resolve(repo, 'examples/crpg-gallery');
const specPath = resolve(gallery, 'glass-cases.json');
const spec = JSON.parse(readFileSync(specPath, 'utf8'));
const group = spec.groups[0];
const out = resolve(process.argv[2] ?? resolve(repo, 'tests/fixtures/glass-reference'));
const executable = process.argv[3] ?? 'C:/Program Files/Google/Chrome/Application/chrome.exe';
if (!existsSync(executable)) throw new Error('Provide a local Chromium executable as the second argument');
const clocks = ['probes/page_reference_clock.js', 'probes/cluster_reference_clock.js'];
// #root as the gallery card's (probes/gallery_scene_session.hpp), the case's size; the scene's UA sheet sizes boxes by
// their borders and gives every element a solid border of no width.
const page = '<!doctype html><html><head><meta charset=utf-8><style>*{box-sizing:border-box;border-style:solid;' +
  'border-width:0}html,body{margin:0;background:#000}</style><link rel=stylesheet href=glass-utilities.css>' +
  '<link rel=stylesheet href=glass-keyframes.css></head><body>' +
  `<div id=root style="position:relative;overflow:hidden;width:${group.width}px;height:${group.height}px;` +
  'background-color:#050403;color:#f5e6c8;font-family:system-ui;font-size:16px;line-height:1.5"></div>' +
  '<script src=platform.js></script><script src=GLASS_TESTS.js></script></body></html>';
const mime = {'.html': 'text/html', '.js': 'application/javascript', '.css': 'text/css'};
const used = new Set();
const server = createServer((request, response) => {
  const name = new URL(request.url, 'http://localhost').pathname.slice(1) || 'glass.html';
  if (!/^[A-Za-z0-9_.-]+$/.test(name)) {
    response.writeHead(404);
    response.end();
    return;
  }
  try {
    let data;
    if (name === 'glass.html') {
      data = Buffer.from(page);
    } else {
      const path = name.endsWith('.css') ? resolve(gallery, name) : resolve(gallery, 'native', name);
      data = readFileSync(path);
      used.add(path);
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
const profile = mkdtempSync(join(tmpdir(), 'weft-glass-'));
const child = spawn(executable, ['--headless=new', '--no-first-run', '--disable-background-networking',
  '--remote-debugging-port=0', '--force-device-scale-factor=1', `--window-size=${group.width},${group.height}`,
  '--hide-scrollbars', '--user-data-dir=' + profile, 'about:blank'], {windowsHide: true, stdio: ['ignore', 'ignore', 'pipe']});
const sockets = [];
// A DevTools connection to url: call(method, params), and the events that arrive to on.
const connect = async (url, on = () => {}) => {
  const ws = new WebSocket(url);
  sockets.push(ws);
  await new Promise((r, j) => {
    ws.addEventListener('open', r, {once: true});
    ws.addEventListener('error', j, {once: true});
  });
  let next = 0;
  const requests = new Map();
  ws.addEventListener('message', (event) => {
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
    on(message);
  });
  return (method, params = {}) => new Promise((done, reject) => {
    const id = ++next;
    const timer = setTimeout(() => {
      requests.delete(id);
      reject(new Error('CDP timeout: ' + method));
    }, 20000);
    requests.set(id, {done, reject, timer});
    ws.send(JSON.stringify({id, method, params}));
  });
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
  // The GPUs, the first the one that draws, with their drivers: SystemInfo is the browser's domain.
  const info = await (await connect(version.webSocketDebuggerUrl))('SystemInfo.getInfo');
  const gpu = {devices: info.gpu.devices.filter((d) => d.deviceString)
    .map((d) => ({name: d.deviceString, driver: d.driverVersion})), features: info.gpu.featureStatus ?? {}};
  const tab = (await (await fetch(endpoint + '/json/list')).json()).find((t) => t.type === 'page');
  const errors = [];
  const blocked = [];
  let call;
  call = await connect(tab.webSocketDebuggerUrl, (message) => {
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
  mkdirSync(out, {recursive: true});
  const cases = [];
  for (let index = 0; index < group.count; ++index) {
    const id = group.prefix + '-' + String(index).padStart(2, '0');
    const loaded = new Promise((r) => {
      const poll = async () => (await evaluate("typeof __ceGallery !== 'undefined'").catch(() => false)) ? r() :
        setTimeout(poll, 20);
      setTimeout(poll, 20);
    });
    await call('Page.navigate', {url: origin + '/glass.html'});
    await loaded;
    await evaluate(`__ceGallery.mount('${group.name}',${index})`);
    for (let i = 0; i < 10; ++i) await evaluate('__ceReference.settle()');
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
        const shot = await call('Page.captureScreenshot', {format: 'png', captureBeyondViewport: false,
          fromSurface: true});
        writeFileSync(resolve(out, `${id}-${frame}.png`), Buffer.from(shot.data, 'base64'));
        captures.push({frame, time, state: JSON.parse(await evaluate('__ceGlassPage.state()')),
          errors: await evaluate('__ceGallery.state().errors')});
      }
    }
    cases.push({id, index, captures, input_trace: trace});
  }
  if (errors.length || blocked.length) throw new Error(JSON.stringify({errors, blocked}));
  // The files the captures came from, hashed with LF line ends as Git stores them (tools/verify_gallery_fixture.py).
  const files = {};
  for (const p of [...used, specPath, resolve(repo, 'tools/browser_glass_page_reference.mjs'),
    ...clocks.map((c) => resolve(repo, c))].sort()) {
    files[relative(repo, p).replaceAll('\\', '/')] =
      sha(Buffer.from(readFileSync(p).toString('latin1').replaceAll('\r\n', '\n'), 'latin1'));
  }
  writeFileSync(resolve(out, 'report.json'), JSON.stringify({format: 'gallery-glass-reference-v1',
    browser: version.Browser, gpu, dpr: 1, width: group.width, height: group.height, frames: spec.frames, cases,
    source_files: files}, null, 1) + '\n');
  console.log(`Recorded ${cases.length} cases, ${cases.length * spec.capture_frames.length} captures from ` +
    `${version.Browser} to ${out}`);
} finally {
  for (const ws of sockets) ws.close();
  child.kill();
  await new Promise((r) => server.close(r));
}
