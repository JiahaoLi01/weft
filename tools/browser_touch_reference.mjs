// Chrome's touch behaviour on tests/fixtures/touch (touch.js, touch.css, scenarios.json), via CDP with touch emulation:
// each scenario loads the page anew in an 800 x 600 viewport at device scale 1 and plays its steps as touch input
// (Input.dispatchTouchEvent with every active touch point, its timestamps the steps' times so that velocities are
// exact; the steps' gaps are waited too, so that the browser's tap, long press and fling timers run), then records the
// events the page logged and its state after each step. Writes tests/fixtures/touch/chrome-results.json, which
// tests/touch_tests.cpp compares a View's run with. Offline: local files only.
//
//   node tools/browser_touch_reference.mjs [output file] [chrome executable]
import {createServer} from 'node:http';
import {spawn} from 'node:child_process';
import {readFileSync, writeFileSync, mkdtempSync, existsSync, rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {resolve, join, extname} from 'node:path';
import {fileURLToPath} from 'node:url';

const repo = resolve(fileURLToPath(new URL('..', import.meta.url)));
const directory = resolve(repo, 'tests/fixtures/touch');
const spec = JSON.parse(readFileSync(resolve(directory, 'scenarios.json'), 'utf8'));
const out = resolve(process.argv[2] ?? resolve(directory, 'chrome-results.json'));
const defaults = ['C:/Program Files/Google/Chrome/Application/chrome.exe',
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'];
const executable = process.argv[3] ?? defaults.find(existsSync);
if (!executable || !existsSync(executable)) throw new Error('Provide a local Chromium executable as the second argument');
const page = '<!doctype html><html><head><meta charset=utf-8><link rel=stylesheet href=touch.css></head>' +
  '<body style="margin:0"><div id=root></div><script src=touch.js></script></body></html>';
const mime = {'.html': 'text/html', '.js': 'application/javascript', '.css': 'text/css'};
const server = createServer((request, response) => {
  const name = new URL(request.url, 'http://localhost').pathname.slice(1) || 'touch.html';
  try {
    const data = name === 'touch.html' ? Buffer.from(page) :
      /^touch\.(js|css)$/.test(name) ? readFileSync(resolve(directory, name)) : null;
    if (!data) throw new Error(name);
    response.writeHead(200, {'Content-Type': mime[extname(name)]});
    response.end(data);
  } catch {
    response.writeHead(404);
    response.end();
  }
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const origin = 'http://127.0.0.1:' + server.address().port;
const profile = mkdtempSync(join(tmpdir(), 'weft-touch-'));
const child = spawn(executable, ['--headless=new', '--no-first-run', '--disable-background-networking',
  '--remote-debugging-port=0', '--force-device-scale-factor=1', `--window-size=${spec.width},${spec.height}`,
  '--hide-scrollbars', '--user-data-dir=' + profile, 'about:blank'],
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
  const requests = new Map();
  const errors = [];
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
  });
  const evaluate = async (expression) => {
    const r = await call('Runtime.evaluate', {expression, returnByValue: true, awaitPromise: true});
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
    return r.result.value;
  };
  const wait = (ms) => new Promise((r) => setTimeout(r, ms));
  await call('Runtime.enable');
  await call('Page.enable');
  await call('Emulation.setDeviceMetricsOverride', {width: spec.width, height: spec.height, deviceScaleFactor: 1,
    mobile: false});
  await call('Emulation.setTouchEmulationEnabled', {enabled: true, maxTouchPoints: 5});
  await call('Emulation.setFocusEmulationEnabled', {enabled: true});

  const results = {};
  for (const scenario of spec.scenarios) {
    await call('Page.navigate', {url: origin + '/touch.html'});
    for (let i = 0; i < 200 && !await evaluate("typeof __touch !== 'undefined'").catch(() => false); ++i) await wait(10);
    await call('Page.bringToFront');
    await wait(100);
    await evaluate('__touch.take()');
    // The active touches by id: where they are.
    const active = new Map();
    const points = () => Array.from(active, ([id, [x, y]]) => ({x, y, id, radiusX: spec.radius, radiusY: spec.radius,
      force: 0}));
    let time = Date.now() / 1000;
    const steps = [];
    for (const step of scenario.steps) {
      const dt = step.dt ?? 16;
      if (step.wait !== undefined) {
        await wait(step.wait);
        time += step.wait / 1000;
      } else {
        await wait(dt);
        time += dt / 1000;
        await evaluate('__touch.mark()');
        if (step.start) {
          for (const s of step.start) {
            const [left, top, width, height] = await evaluate(`__touch.box(${JSON.stringify(s.at)})`);
            active.set(s.id, [left + (s.dx ?? width / 2), top + (s.dy ?? height / 2)]);
          }
          await call('Input.dispatchTouchEvent', {type: 'touchStart', touchPoints: points(), timestamp: time});
        } else if (step.move) {
          for (const m of step.move) {
            const [x, y] = active.get(m.id);
            active.set(m.id, [x + m.by[0], y + m.by[1]]);
          }
          await call('Input.dispatchTouchEvent', {type: 'touchMove', touchPoints: points(), timestamp: time});
        } else if (step.end) {
          // CDP's touchEnd releases the points it lists.
          const ended = points().filter((p) => step.end.includes(p.id));
          for (const id of step.end) active.delete(id);
          await call('Input.dispatchTouchEvent', {type: 'touchEnd', touchPoints: ended, timestamp: time});
        } else if (step.cancel) {
          active.clear();
          await call('Input.dispatchTouchEvent', {type: 'touchCancel', touchPoints: [], timestamp: time});
        } else {
          throw new Error('unknown step ' + JSON.stringify(step));
        }
      }
      await wait(30);
      steps.push({step, events: await evaluate('__touch.take()'), state: await evaluate('__touch.state()')});
    }
    results[scenario.name] = steps;
  }
  if (errors.length) throw new Error('page errors: ' + errors.join('; '));
  writeFileSync(out, JSON.stringify({chrome: version.Browser, scenarios: results}, null, 1) + '\n');
  console.log(`wrote ${out} (${version.Browser})`);
} finally {
  socket?.close();
  child.kill();
  server.close();
  await new Promise((r) => setTimeout(r, 300));
  try { rmSync(profile, {recursive: true, force: true}); } catch {}
}
