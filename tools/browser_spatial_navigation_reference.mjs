// Chrome's spatial navigation (--enable-spatial-navigation) on tests/fixtures/spatial_navigation (spatnav.js,
// spatnav.css, scenarios.json), via CDP: each scenario loads the page anew in an 800 x 600 viewport at device scale 1
// and plays its steps as Chrome on Windows gets them (a key's raw keydown, its character, its keyup; mouse moves,
// presses and releases), then records the events the page logged and its state after each step. Smooth scrolling is
// off, so a key's scroll is done when the step ends. Writes tests/fixtures/spatial_navigation/chrome-results.json,
// which tests/spatial_navigation_tests.cpp compares a View's run with. Offline: local files only.
//
//   node tools/browser_spatial_navigation_reference.mjs [output file] [chrome.exe]
import {createServer} from 'node:http';
import {spawn} from 'node:child_process';
import {readFileSync, writeFileSync, mkdtempSync, existsSync, rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {resolve, join, extname} from 'node:path';
import {fileURLToPath} from 'node:url';

const repo = resolve(fileURLToPath(new URL('..', import.meta.url)));
const directory = resolve(repo, 'tests/fixtures/spatial_navigation');
const spec = JSON.parse(readFileSync(resolve(directory, 'scenarios.json'), 'utf8'));
const out = resolve(process.argv[2] ?? resolve(directory, 'chrome-results.json'));
const executable = process.argv[3] ?? 'C:/Program Files/Google/Chrome/Application/chrome.exe';
if (!existsSync(executable)) throw new Error('Provide a local Chromium executable as the second argument');
const page = '<!doctype html><html><head><meta charset=utf-8><link rel=stylesheet href=spatnav.css></head>' +
  '<body style="margin:0"><div id=root></div><script src=spatnav.js></script></body></html>';
const mime = {'.html': 'text/html', '.js': 'application/javascript', '.css': 'text/css'};
const server = createServer((request, response) => {
  const name = new URL(request.url, 'http://localhost').pathname.slice(1) || 'spatnav.html';
  try {
    const data = name === 'spatnav.html' ? Buffer.from(page) :
      /^spatnav\.(js|css)$/.test(name) ? readFileSync(resolve(directory, name)) : null;
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
const profile = mkdtempSync(join(tmpdir(), 'weft-spatnav-'));
const child = spawn(executable, ['--headless=new', '--no-first-run', '--disable-background-networking',
  '--remote-debugging-port=0', '--force-device-scale-factor=1', `--window-size=${spec.width},${spec.height}`,
  '--hide-scrollbars', '--disable-smooth-scrolling', '--enable-spatial-navigation', '--user-data-dir=' + profile,
  'about:blank'],
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
  // A headless page has no system focus; the scenarios type into a focused window.
  await call('Emulation.setFocusEmulationEnabled', {enabled: true});

  // CDP's modifier bits: Alt 1, Ctrl 2, Meta 4, Shift 8.
  const bits = {A: 1, C: 2, M: 4, S: 8};
  const modifierKeys = {Shift: 'S', Control: 'C', Alt: 'A', Meta: 'M'};
  const results = {};
  for (const scenario of spec.scenarios) {
    await call('Page.navigate', {url: origin + '/spatnav.html'});
    for (let i = 0; i < 200 && !await evaluate("typeof __spatnav !== 'undefined'").catch(() => false); ++i) await wait(10);
    await call('Page.bringToFront');
    await wait(50);
    await evaluate('__spatnav.take()');
    // Modifiers held by down steps: a modifier key's own keydown has its flag, its keyup does not.
    const held = new Set();
    const key = async (type, name, step) => {
      const k = spec.keys[name];
      const own = modifierKeys[k.key];
      if (own && type === 'down') held.add(own);
      if (own && type === 'up') held.delete(own);
      let modifiers = 0;
      for (const m of new Set([...held, ...(step.mods ?? '')])) modifiers |= bits[m];
      const params = {key: k.key, code: k.code, windowsVirtualKeyCode: k.vk, nativeVirtualKeyCode: k.vk, modifiers,
        autoRepeat: !!step.repeat && type !== 'up', isKeypad: k.code.startsWith('Numpad'),
        location: /^(Shift|Control|Alt|Meta)Left$/.test(k.code) ? 1 : /^(Shift|Control|Alt|Meta)Right$/.test(k.code) ? 2 : 0};
      if (type === 'down') {
        await call('Input.dispatchKeyEvent', {type: 'rawKeyDown', ...params});
      } else if (type === 'char') {
        await call('Input.dispatchKeyEvent', {type: 'char', ...params, text: k.text, unmodifiedText: k.text});
      } else {
        await call('Input.dispatchKeyEvent', {type: 'keyUp', ...params});
      }
    };
    const steps = [];
    for (const step of scenario.steps) {
      if (step.press) {
        await key('down', step.press, step);
        if (spec.keys[step.press].text) await key('char', step.press, step);
        await key('up', step.press, step);
      } else if (step.down) {
        await key('down', step.down, step);
        if (spec.keys[step.down].text) await key('char', step.down, step);
      } else if (step.up) {
        await key('up', step.up, step);
      } else if (step.click) {
        const [x, y] = await evaluate(`(r => [r.left + r.width / 2, r.top + r.height / 2])(document.getElementById(${JSON.stringify(step.click)}).getBoundingClientRect())`);
        await call('Input.dispatchMouseEvent', {type: 'mouseMoved', x, y, button: 'none', buttons: 0});
        await call('Input.dispatchMouseEvent', {type: 'mousePressed', x, y, button: 'left', buttons: 1, clickCount: 1});
        await call('Input.dispatchMouseEvent', {type: 'mouseReleased', x, y, button: 'left', buttons: 0, clickCount: 1});
      } else if (step.focus) {
        await evaluate(`document.getElementById(${JSON.stringify(step.focus)}).focus()`);
      } else if (step.blur) {
        await evaluate('document.activeElement && document.activeElement.blur()');
      } else if (step.script) {
        await evaluate(step.script);
      } else {
        throw new Error('unknown step ' + JSON.stringify(step));
      }
      await wait(30);
      steps.push({step, events: await evaluate('__spatnav.take()'), state: await evaluate('__spatnav.state()')});
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
