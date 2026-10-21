// Chrome's text fields on tests/fixtures/text_field (field.js, field.css, scenarios.json), via CDP: each scenario loads
// the page anew in an 800 x 600 viewport at device scale 1 and plays its steps as Chrome on Windows gets them (a key's
// raw keydown, its character, its keyup; text typed key by key; mouse presses, clicks of a count and drags at points of
// an element; an input method's composition and its commit), then records the events the page logged and the fields'
// values and selections after each step, and an image of an element where a step asks for one. Writes
// tests/fixtures/text_field/chrome-results.json and chrome/<name>.png, which tests/text_field_tests.cpp compares a View's
// run with. Offline: local files only.
//
//   node tools/browser_text_field_reference.mjs [output file] [chrome.exe]
import {createServer} from 'node:http';
import {spawn} from 'node:child_process';
import {readFileSync, writeFileSync, mkdtempSync, mkdirSync, existsSync, rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {resolve, join, extname, dirname} from 'node:path';
import {fileURLToPath} from 'node:url';

const repo = resolve(fileURLToPath(new URL('..', import.meta.url)));
const directory = resolve(repo, 'tests/fixtures/text_field');
const spec = JSON.parse(readFileSync(resolve(directory, 'scenarios.json'), 'utf8'));
const out = resolve(process.argv[2] ?? resolve(directory, 'chrome-results.json'));
const shots = resolve(dirname(out), 'chrome');
const executable = process.argv[3] ?? 'C:/Program Files/Google/Chrome/Application/chrome.exe';
if (!existsSync(executable)) throw new Error('Provide a local Chromium executable as the second argument');
const page = '<!doctype html><html><head><meta charset=utf-8><link rel=stylesheet href=field.css></head>' +
  '<body style="margin:0"><div id=root></div><script src=field.js></script></body></html>';
const mime = {'.html': 'text/html', '.js': 'application/javascript', '.css': 'text/css'};
const server = createServer((request, response) => {
  const name = new URL(request.url, 'http://localhost').pathname.slice(1) || 'field.html';
  try {
    const data = name === 'field.html' ? Buffer.from(page) :
      /^field\.(js|css)$/.test(name) ? readFileSync(resolve(directory, name)) : null;
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
const profile = mkdtempSync(join(tmpdir(), 'weft-field-'));
const child = spawn(executable, ['--headless=new', '--no-first-run', '--disable-background-networking',
  '--remote-debugging-port=0', '--force-device-scale-factor=1', `--window-size=${spec.width},${spec.height}`,
  '--hide-scrollbars', '--disable-smooth-scrolling', '--user-data-dir=' + profile, 'about:blank'],
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
  // A character typed on a US keyboard: its key, code and virtual key (a letter's, a digit's, a space's; others take
  // the key alone).
  const typed = (c) => {
    if (/^[a-z]$/i.test(c)) {
      const upper = c.toUpperCase();
      return {key: c, code: 'Key' + upper, vk: upper.charCodeAt(0), text: c, shift: c !== c.toLowerCase()};
    }
    if (/^[0-9]$/.test(c)) return {key: c, code: 'Digit' + c, vk: c.charCodeAt(0), text: c};
    if (c === ' ') return {key: ' ', code: 'Space', vk: 32, text: ' '};
    return {key: c, code: '', vk: 0, text: c};
  };
  const results = {};
  mkdirSync(shots, {recursive: true});
  for (const scenario of spec.scenarios) {
    await call('Page.navigate', {url: origin + '/field.html'});
    for (let i = 0; i < 200 && !await evaluate("typeof __field !== 'undefined'").catch(() => false); ++i) await wait(10);
    await call('Page.bringToFront');
    await wait(50);
    await evaluate('__field.take()');
    // Modifiers held by down steps: a modifier key's own keydown has its flag, its keyup does not.
    const held = new Set();
    const send = async (type, k, step) => {
      const own = modifierKeys[k.key];
      if (own && type === 'down') held.add(own);
      if (own && type === 'up') held.delete(own);
      let modifiers = 0;
      for (const m of new Set([...held, ...(step.mods ?? ''), ...(k.shift ? 'S' : '')])) modifiers |= bits[m];
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
    const press = async (k, step) => {
      await send('down', k, step);
      if (k.text && !(step.mods ?? '').includes('C')) await send('char', k, step);
      await send('up', k, step);
    };
    const mouse = async (type, [x, y], buttons, clickCount) => {
      await call('Input.dispatchMouseEvent', {type, x, y, button: type === 'mouseMoved' && !buttons ? 'none' : 'left',
        buttons, clickCount});
    };
    const steps = [];
    for (const step of scenario.steps) {
      if (step.press) {
        await press(spec.keys[step.press], step);
      } else if (step.down) {
        const k = spec.keys[step.down];
        await send('down', k, step);
        if (k.text) await send('char', k, step);
      } else if (step.up) {
        await send('up', spec.keys[step.up], step);
      } else if (step.type !== undefined) {
        for (const c of step.type) await press(typed(c), step);
      } else if (step.click) {
        // count clicks at a point of the element (its border box's left and top plus at; its centre by default).
        const at = step.at ? await evaluate(`__field.at(${JSON.stringify(step.click)}, ${step.at[0]}, ${step.at[1]})`) :
          await evaluate(`(r => [r[0] + r[2] / 2, r[1] + r[3] / 2])(__field.rect(${JSON.stringify(step.click)}))`);
        await mouse('mouseMoved', at, 0, 0);
        for (let n = 1; n <= (step.count ?? 1); ++n) {
          await mouse('mousePressed', at, 1, n);
          await mouse('mouseReleased', at, 0, n);
        }
      } else if (step.drag) {
        const from = await evaluate(`__field.at(${JSON.stringify(step.drag)}, ${step.from[0]}, ${step.from[1]})`);
        const to = await evaluate(`__field.at(${JSON.stringify(step.drag)}, ${step.to[0]}, ${step.to[1]})`);
        await mouse('mouseMoved', from, 0, 0);
        await mouse('mousePressed', from, 1, 1);
        await mouse('mouseMoved', to, 1, 0);
        await mouse('mouseReleased', to, 0, 1);
      } else if (step.ime !== undefined) {
        // The composition and its selection in it (UTF-16 offsets), as an input method sets it.
        const [selectionStart, selectionEnd] = step.selection ?? [step.ime.length, step.ime.length];
        await call('Input.imeSetComposition', {text: step.ime, selectionStart, selectionEnd});
      } else if (step.commit !== undefined) {
        await call('Input.insertText', {text: step.commit});
      } else if (step.focus) {
        await evaluate(`document.getElementById(${JSON.stringify(step.focus)}).focus()`);
      } else if (step.blur) {
        await evaluate('document.activeElement && document.activeElement.blur()');
      } else if (step.script) {
        // In a block of its own: its declarations do not stay in the page's global scope.
        await evaluate('{' + step.script + '}');
      } else if (step.shot) {
        // Settle where a caret blinks: its on phase starts at the last edit or move, 0.5 s on, 0.5 s off.
        const [x, y, width, height] = await evaluate(`__field.rect(${JSON.stringify(step.of)})`);
        const r = await call('Page.captureScreenshot', {format: 'png', clip: {x, y, width, height, scale: 1},
          captureBeyondViewport: false});
        writeFileSync(resolve(shots, step.shot + '.png'), Buffer.from(r.data, 'base64'));
      } else if (step.wait) {
        // Time passing: past the system's double-click time, or a caret's blink.
        await wait(step.wait);
      } else if (step.read) {
        // A value the page computes, recorded with the step (getComputedStyle and the like).
        steps.push({step, value: await evaluate(step.read)});
        continue;
      } else {
        throw new Error('unknown step ' + JSON.stringify(step));
      }
      await wait(30);
      steps.push({step, events: await evaluate('__field.take()'), state: await evaluate('__field.state()')});
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
