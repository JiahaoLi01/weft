// Chrome oracle for synthetic oblique text (11.3c): the same words upright and in italics in a face without italics
// (Impact), one above the other, so a test can measure the slant Chrome gives the italics (the shift of each row's ink
// between the two). Writes the case list and a screenshot at device scale 1. Offline: the page is a data URL.
//
//   node tools/browser_scene_oblique_reference.mjs [output directory] [chrome.exe]
import {spawn} from 'node:child_process';
import {writeFileSync, mkdirSync, mkdtempSync, existsSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {resolve, join} from 'node:path';
import {repo} from './gallery_runtime.mjs';

const output = resolve(process.argv[2] ?? resolve(repo, 'tests/fixtures/scene-oblique-reference'));
const executable = process.argv[3] ?? 'C:/Program Files/Google/Chrome/Application/chrome.exe';
if (!existsSync(executable)) throw new Error('Provide a local Chromium executable as the second argument');

const width = 400;
const height = 140;
const text = 'HIJ Slanted';
const style = 'font-family:Impact;font-size:40px;line-height:60px;color:#ffffff';
// Upright at the top, italic below: the same text, the same x.
const lines = [{y: 0, style}, {y: 70, style: style + ';font-style:italic'}];
const page = '<!doctype html><html><head><meta charset=utf-8><style>*{box-sizing:border-box;margin:0;padding:0}' +
  `body{margin:0;width:${width}px;height:${height}px;background:#000;position:relative;overflow:hidden}` +
  '.l{position:absolute;left:20px;white-space:nowrap}</style></head><body>' +
  lines.map((l) => `<div class=l style="top:${l.y}px;${l.style}">${text}</div>`).join('') + '</body></html>';

const profile = mkdtempSync(join(tmpdir(), 'weft-oblique-'));
const child = spawn(executable, ['--headless=new', '--no-first-run', '--disable-background-networking',
  '--remote-debugging-port=0', '--force-device-scale-factor=1', `--window-size=${width},${height}`,
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
  await call('Emulation.setDeviceMetricsOverride', {width, height, deviceScaleFactor: 0, mobile: false});
  const loaded = new Promise((r) => socket.addEventListener('message', (e) => {
    if (JSON.parse(e.data).method === 'Page.loadEventFired') r();
  }));
  await call('Page.navigate', {url: 'data:text/html;charset=utf-8,' + encodeURIComponent(page)});
  await loaded;
  await call('Runtime.evaluate', {expression: 'document.fonts.ready', awaitPromise: true});
  const shot = await call('Page.captureScreenshot', {format: 'png', clip: {x: 0, y: 0, width, height, scale: 1},
    captureBeyondViewport: false});
  mkdirSync(output, {recursive: true});
  writeFileSync(join(output, 'oblique.png'), Buffer.from(shot.data, 'base64'));
  writeFileSync(join(output, 'cases.json'), JSON.stringify({browser: version.Browser, width, height, text, left: 20,
    lines}, null, 1) + '\n');
  console.log(`Recorded upright and synthetic oblique text from ${version.Browser} to ${output}`);
} finally {
  socket?.close();
  child.kill();
}
