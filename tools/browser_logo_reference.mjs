// Real Chromium reference for the logo pages (examples/logo) via CDP: each variant's page.json (its scripts, stylesheet
// and keyframes) on a #root of the case's size, the scene's box sizing and borders as its UA sheet has them, at device
// scale 1. Clock: probes/page_reference_clock.js (timers, frames and CSS animations from 0, as the scene starts them at
// the page's load). Captures a screenshot at each frame of examples/logo/reference-cases.json (frames are 1/60 s) and
// writes <out>/<variant>-<frame>.png with a report of the browser, its GPU, the captures and the files used
// (tools/verify_logo_fixture.py checks them; tests/logo_reference_tests.cpp draws the same frames with weft). Offline:
// local files only.
//
//   node tools/browser_logo_reference.mjs [output directory, tests/fixtures/logo-reference] [chrome.exe]
import {createServer} from 'node:http';
import {spawn} from 'node:child_process';
import {readFileSync, writeFileSync, mkdirSync, mkdtempSync, existsSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {resolve, extname, join, relative, dirname} from 'node:path';
import {repo, sha} from './gallery_runtime.mjs';

const logo = resolve(repo, 'examples/logo');
const specPath = resolve(logo, 'reference-cases.json');
const spec = JSON.parse(readFileSync(specPath, 'utf8'));
const out = resolve(process.argv[2] ?? resolve(repo, 'tests/fixtures/logo-reference'));
const executable = process.argv[3] ?? 'C:/Program Files/Google/Chrome/Application/chrome.exe';
if (!existsSync(executable)) throw new Error('Provide a local Chromium executable as the second argument');
const clock = 'probes/page_reference_clock.js';

// A variant's page as a browser document: its page.json's files by their paths relative to the variant's folder.
const pageOf = (variant) => {
  const manifest = JSON.parse(readFileSync(resolve(logo, variant, 'page.json'), 'utf8'));
  const link = (file) => `<link rel=stylesheet href="/${variant}/${file}">`;
  return '<!doctype html><html><head><meta charset=utf-8><style>*{box-sizing:border-box;border-style:solid;' +
    `border-width:0}html,body{margin:0;width:${spec.width}px;height:${spec.height}px;overflow:hidden}</style>` +
    link(manifest.stylesheet) + link(manifest.keyframes) + '</head><body>' +
    `<div id=root style="width:${spec.width}px;height:${spec.height}px"></div>` +
    manifest.scripts.map((s) => `<script src="/${variant}/${s}"></script>`).join('') + '</body></html>';
};
const mime = {'.html': 'text/html', '.js': 'application/javascript', '.css': 'text/css'};
const used = new Set();
const server = createServer((request, response) => {
  const name = decodeURIComponent(new URL(request.url, 'http://localhost').pathname.slice(1));
  try {
    let data;
    const page = name.match(/^([a-z]+)\.html$/);
    if (page && spec.variants.includes(page[1])) {
      data = Buffer.from(pageOf(page[1]));
    } else {
      // Files of examples/logo only.
      const path = resolve(logo, name);
      if (relative(logo, path).startsWith('..') || !/^[A-Za-z0-9_./-]+$/.test(name)) throw new Error('outside');
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
const profile = mkdtempSync(join(tmpdir(), 'weft-logo-'));
const child = spawn(executable, ['--headless=new', '--no-first-run', '--disable-background-networking',
  '--remote-debugging-port=0', '--force-device-scale-factor=1', `--window-size=${spec.width},${spec.height}`,
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
  await call('Runtime.enable');
  await call('Page.enable');
  await call('Emulation.setDeviceMetricsOverride', {width: spec.width, height: spec.height, deviceScaleFactor: 1,
    mobile: false});
  await call('Fetch.enable', {patterns: [{urlPattern: 'http*', requestStage: 'Request'}]});
  await call('Page.addScriptToEvaluateOnNewDocument', {source: readFileSync(resolve(repo, clock), 'utf8')});
  mkdirSync(out, {recursive: true});
  const captures = [];
  for (const variant of spec.variants) {
    const loaded = new Promise((r) => {
      const poll = async () => (await evaluate("!!document.querySelector('#root svg')").catch(() => false)) ? r() :
        setTimeout(poll, 20);
      setTimeout(poll, 20);
    });
    await call('Page.navigate', {url: `${origin}/${variant}.html`});
    await loaded;
    await evaluate('__ceReference.settle()');
    for (const frame of spec.frames) {
      const time = frame * (1000 / 60);
      await evaluate(`__ceReference.step(${time})`);
      await evaluate(`__ceReference.styles(${time})`);
      const shot = await call('Page.captureScreenshot', {format: 'png', captureBeyondViewport: false, fromSurface: true});
      const file = `${variant}-${String(frame).padStart(3, '0')}.png`;
      const png = Buffer.from(shot.data, 'base64');
      writeFileSync(resolve(out, file), png);
      captures.push({variant, frame, time, file, sha256: sha(png)});
    }
  }
  if (errors.length || blocked.length) throw new Error(JSON.stringify({errors, blocked}));
  // The files the captures came from, hashed with LF line ends as Git stores them.
  const files = {};
  for (const p of [...used, specPath, resolve(repo, 'tools/browser_logo_reference.mjs'), resolve(repo, clock)].sort()) {
    files[relative(repo, p).replaceAll('\\', '/')] =
      sha(Buffer.from(readFileSync(p).toString('latin1').replaceAll('\r\n', '\n'), 'latin1'));
  }
  writeFileSync(resolve(out, 'report.json'), JSON.stringify({format: 'weft-logo-reference-v1', browser: version.Browser,
    gpu, dpr: 1, width: spec.width, height: spec.height, captures, source_files: files}, null, 1) + '\n');
  console.log(`Recorded ${captures.length} captures from ${version.Browser} to ${out}`);
} finally {
  for (const ws of sockets) ws.close();
  child.kill();
  await new Promise((r) => server.close(r));
}
