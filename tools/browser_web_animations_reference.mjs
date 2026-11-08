// Chrome oracle for Web Animations (element.animate(), Animation, KeyframeEffect; src/script/js/
// scene_platform_bootstrap.js's timing model, src/scene/scene.cpp's script animations): the steps of
// tests/fixtures/web_animations/animations.js run in headless Chrome, which log states, timing, computed values at set
// times and the order of events and promises (window.__log). Writes tests/fixtures/web_animations/chrome-results.json,
// which tests/web_animations_tests.cpp compares a View's log with.
//
//   node tools/browser_web_animations_reference.mjs [output.json] [chrome executable]
import {spawn} from 'node:child_process';
import {existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

const repo = resolve(fileURLToPath(new URL('..', import.meta.url)));
const directory = resolve(repo, 'tests/fixtures/web_animations');
const output = resolve(process.argv[2] ?? resolve(directory, 'chrome-results.json'));
const defaults = ['C:/Program Files/Google/Chrome/Application/chrome.exe',
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'];
const executable = process.argv[3] ?? defaults.find(existsSync);
if (!executable || !existsSync(executable)) throw new Error('Provide a local Chromium executable as the second argument');

const page = '<!doctype html><html><head><meta charset=utf-8><style>html,body{margin:0}#root{position:relative}' +
  '</style></head><body><div id=root></div><script>' + readFileSync(resolve(directory, 'animations.js'), 'utf8') +
  '</script></body></html>';

const profile = mkdtempSync(join(tmpdir(), 'weft-web-animations-'));
const child = spawn(executable, ['--headless=new', '--no-first-run', '--disable-background-networking',
  '--remote-debugging-port=0', '--force-device-scale-factor=1', '--window-size=800,600', '--user-data-dir=' + profile,
  'about:blank'], {windowsHide: true, stdio: ['ignore', 'ignore', 'pipe']});
let socket;
try {
  const endpoint = await new Promise((resolveEndpoint, reject) => {
    let text = '';
    const limit = setTimeout(() => reject(new Error('Chromium startup timeout')), 20000);
    child.on('error', reject);
    child.stderr.on('data', (c) => {
      text += c;
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
  const evaluate = async (expression) =>
    (await call('Runtime.evaluate', {expression, returnByValue: true, awaitPromise: true})).result.value;
  await call('Page.enable');
  await call('Emulation.setFocusEmulationEnabled', {enabled: true});
  const tree = await call('Page.getFrameTree');
  await call('Page.setDocumentContent', {frameId: tree.frameTree.frame.id, html: page});
  await evaluate('window.__run()');
  const log = await evaluate('JSON.stringify(window.__log)');
  writeFileSync(output, JSON.stringify({browser: version.Browser, 
    log: JSON.parse(log)}, null, 1) + '\n');
  console.log(`Recorded ${JSON.parse(log).length} log entries from ${version.Browser} to ${output}`);
} finally {
  socket?.close();
  const exited = new Promise((r) => child.once('exit', r));
  child.kill();
  await exited;
  rmSync(profile, {recursive: true, force: true, maxRetries: 5, retryDelay: 200});
}
