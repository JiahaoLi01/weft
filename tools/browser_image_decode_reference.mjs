// Chrome oracle for image decoding (src/view/view_images.cpp, src/view/view_image_codecs.cpp): every image of
// tests/fixtures/image_decode (tools/image_decode_fixtures.mjs), served from a local HTTP server, decoded by an img
// (img.decode()) and drawn at its natural size into a 2D canvas, whose pixels getImageData reads (straight alpha,
// from the canvas's premultiplied ones; Chrome applies an image's Exif orientation when it draws it). An image Chrome
// does not decode is recorded with the error decode() gave. Writes tests/fixtures/image_decode/chrome-results.json,
// which tests/image_decode_tests.cpp compares weft's decoding with.
//
//   node tools/browser_image_decode_reference.mjs [output.json] [chrome executable]
import {spawn} from 'node:child_process';
import {createServer} from 'node:http';
import {existsSync, mkdtempSync, readdirSync, readFileSync, rmSync, writeFileSync} from 'node:fs';
import {createHash} from 'node:crypto';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

const repo = resolve(fileURLToPath(new URL('..', import.meta.url)));
const directory = resolve(repo, 'tests/fixtures/image_decode');
const output = resolve(process.argv[2] ?? resolve(directory, 'chrome-results.json'));
const defaults = ['C:/Program Files/Google/Chrome/Application/chrome.exe',
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'];
const executable = process.argv[3] ?? defaults.find(existsSync);
if (!executable || !existsSync(executable)) throw new Error('Provide a local Chromium executable as the second argument');

const names = readdirSync(directory).filter((n) => /\.(jpg|webp|png)$/.test(n)).sort();
const page = '<!doctype html><html><head><meta charset=utf-8></head><body><script>' +
  'window.decodeImage = async (name) => {' +
  '  const img = new Image(); img.src = name;' +
  '  try { await img.decode(); } catch (e) { return {error: e.name}; }' +
  '  const w = img.naturalWidth, h = img.naturalHeight;' +
  '  const c = document.createElement("canvas"); c.width = w; c.height = h;' +
  '  const ctx = c.getContext("2d", {willReadFrequently: true});' +
  '  ctx.drawImage(img, 0, 0);' +
  '  const data = ctx.getImageData(0, 0, w, h).data;' +
  '  let s = ""; for (let i = 0; i < data.length; i += 8192) s += String.fromCharCode(...data.subarray(i, i + 8192));' +
  '  return {width: w, height: h, rgba: btoa(s)};' +
  '};</script></body></html>';

const server = createServer((request, response) => {
  const name = decodeURIComponent(new URL(request.url, 'http://localhost').pathname.slice(1));
  if (name === 'page.html') {
    response.writeHead(200, {'content-type': 'text/html; charset=utf-8'});
    response.end(page);
    return;
  }
  if (!names.includes(name)) {
    response.writeHead(404);
    response.end();
    return;
  }
  // A content type that does not say the format: Chrome sniffs an image's bytes.
  response.writeHead(200, {'content-type': 'application/octet-stream', 'cache-control': 'no-store'});
  response.end(readFileSync(resolve(directory, name)));
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const origin = `http://127.0.0.1:${server.address().port}/`;

const profile = mkdtempSync(join(tmpdir(), 'weft-image-decode-'));
const child = spawn(executable, ['--headless=new', '--no-first-run', '--disable-background-networking',
  '--remote-debugging-port=0', '--force-device-scale-factor=1', '--force-color-profile=srgb',
  '--user-data-dir=' + profile, 'about:blank'], {windowsHide: true, stdio: ['ignore', 'ignore', 'pipe']});
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
  const loaded = [];
  socket.addEventListener('message', (event) => {
    const message = JSON.parse(event.data);
    if (message.id && pending.has(message.id)) {
      const {resolve: done, reject} = pending.get(message.id);
      pending.delete(message.id);
      message.error ? reject(new Error(message.error.message)) : done(message.result);
    } else if (message.method === 'Page.loadEventFired') {
      loaded.shift()?.();
    }
  });
  const call = (method, params = {}) => new Promise((done, reject) => {
    const id = ++next;
    pending.set(id, {resolve: done, reject});
    socket.send(JSON.stringify({id, method, params}));
  });
  await call('Page.enable');
  const ready = new Promise((r) => loaded.push(r));
  await call('Page.navigate', {url: origin + 'page.html'});
  await ready;
  const images = [];
  for (const name of names) {
    const result = (await call('Runtime.evaluate', {expression: `decodeImage(${JSON.stringify(name)})`,
      returnByValue: true, awaitPromise: true})).result.value;
    images.push({name, sha256: createHash('sha256').update(readFileSync(resolve(directory, name))).digest('hex'),
      ...result});
  }
  writeFileSync(output, JSON.stringify({browser: version.Browser,
    source: 'headless Chrome, images from a local HTTP server, drawn into a 2D canvas', images}, null, 1) + '\n');
  const failed = images.filter((i) => i.error).map((i) => `${i.name} (${i.error})`);
  console.log(`Recorded ${images.length} images from ${version.Browser} to ${output}; not decoded: ` +
    (failed.join(', ') || 'none'));
} finally {
  socket?.close();
  server.close();
  const exited = new Promise((r) => child.once('exit', r));
  child.kill();
  await exited;
  rmSync(profile, {recursive: true, force: true, maxRetries: 5, retryDelay: 200});
}
