// Chrome's text fields of right-to-left and mixed text, on tests/fixtures/rtl_field (field.js, field.css), via CDP: for
// each field, the caret at several offsets and several selections, from a screenshot of the field (its caret is
// green, its selection blue): the caret's column and the selection's runs of columns across the field; and where the
// left and right arrow keys take a caret or a selection (selectionStart and selectionEnd after the key). The fonts are
// the repository's, embedded as data URLs. Writes tests/fixtures/rtl_field/chrome-results.json, which
// tests/scene_rtl_field_tests.cpp compares a View's fields with.
//
//   node tools/browser_rtl_field_reference.mjs [output file] [chrome executable]
import {spawn} from 'node:child_process';
import {readFileSync, writeFileSync, mkdtempSync, existsSync, rmSync} from 'node:fs';
import {inflateSync} from 'node:zlib';
import {tmpdir} from 'node:os';
import {resolve, join} from 'node:path';
import {fileURLToPath} from 'node:url';

const repo = resolve(fileURLToPath(new URL('..', import.meta.url)));
const directory = resolve(repo, 'tests/fixtures/rtl_field');
const output = resolve(process.argv[2] ?? resolve(directory, 'chrome-results.json'));
const defaults = ['C:/Program Files/Google/Chrome/Application/chrome.exe',
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'];
const executable = process.argv[3] ?? defaults.find(existsSync);
if (!executable || !existsSync(executable)) throw new Error('Provide a local Chromium executable as the second argument');

const fonts = [
  {family: 'Roboto', path: 'third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf'},
  {family: 'TestArabic', path: 'third_party/harfbuzz/source/perf/fonts/Amiri-Regular.ttf'},
  {family: 'TestHebrew', path: 'third_party/harfbuzz/source/test/subset/data/fonts/NotoIKEAHebrewLatin-Regular.ttf'},
];
const faces = fonts.map((f) => `@font-face{font-family:${f.family};src:url(data:font/ttf;base64,` +
  readFileSync(resolve(repo, f.path)).toString('base64') + `)}`).join('');
const page = '<!doctype html><html><head><meta charset=utf-8><style>' + faces +
  'html,body{margin:0;background:#ffffff}' + readFileSync(resolve(directory, 'field.css'), 'utf8') +
  '</style></head><body><div id=root></div><script>' + readFileSync(resolve(directory, 'field.js'), 'utf8') +
  '</script></body></html>';

// A PNG's pixels as RGBA (8-bit RGB or RGBA, not interlaced).
const png = (bytes) => {
  let at = 8;
  let width = 0;
  let height = 0;
  let channels = 4;
  const data = [];
  while (at < bytes.length) {
    const length = bytes.readUInt32BE(at);
    const type = bytes.toString('ascii', at + 4, at + 8);
    const body = bytes.subarray(at + 8, at + 8 + length);
    if (type === 'IHDR') {
      width = body.readUInt32BE(0);
      height = body.readUInt32BE(4);
      if (body[8] !== 8 || body[12] !== 0 || (body[9] !== 6 && body[9] !== 2)) throw new Error('PNG profile');
      channels = body[9] === 6 ? 4 : 3;
    } else if (type === 'IDAT') {
      data.push(body);
    }
    at += 12 + length;
  }
  const raw = inflateSync(Buffer.concat(data));
  const stride = width * channels;
  const out = Buffer.alloc(width * height * 4);
  let previous = Buffer.alloc(stride);
  for (let y = 0; y < height; ++y) {
    const filter = raw[y * (stride + 1)];
    const line = Buffer.from(raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1)));
    for (let x = 0; x < stride; ++x) {
      const a = x >= channels ? line[x - channels] : 0;
      const b = previous[x];
      const c = x >= channels ? previous[x - channels] : 0;
      const p = a + b - c;
      const pa = Math.abs(p - a);
      const pb = Math.abs(p - b);
      const pc = Math.abs(p - c);
      const paeth = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
      line[x] = (line[x] + [0, a, b, (a + b) >> 1, paeth][filter]) & 255;
    }
    for (let x = 0; x < width; ++x) {
      for (let k = 0; k < 3; ++k) out[(y * width + x) * 4 + k] = line[x * channels + k];
      out[(y * width + x) * 4 + 3] = channels === 4 ? line[x * channels + 3] : 255;
    }
    previous = line;
  }
  return {width, height, data: out};
};

// The runs of columns (from the image's left) with a pixel that test takes.
const columns = (image, test) => {
  const hit = [];
  for (let x = 0; x < image.width; ++x) {
    let any = false;
    for (let y = 0; y < image.height && !any; ++y) {
      const k = (y * image.width + x) * 4;
      any = test(image.data[k], image.data[k + 1], image.data[k + 2]);
    }
    hit.push(any);
  }
  const runs = [];
  for (let x = 0; x < hit.length; ++x) {
    if (!hit[x]) continue;
    if (runs.length && runs[runs.length - 1][1] === x) runs[runs.length - 1][1] = x + 1;
    else runs.push([x, x + 1]);
  }
  return runs;
};

const profile = mkdtempSync(join(tmpdir(), 'weft-rtl-field-'));
const child = spawn(executable, ['--headless=new', '--no-first-run', '--disable-background-networking',
  '--remote-debugging-port=0', '--force-device-scale-factor=1', '--window-size=800,600', '--user-data-dir=' + profile,
  'about:blank'], {windowsHide: true, stdio: ['ignore', 'ignore', 'pipe']});
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
  const evaluate = async (expression) =>
    (await call('Runtime.evaluate', {expression, returnByValue: true, awaitPromise: true})).result.value;
  await call('Page.enable');
  // The page has the focus, so that a focused field shows its caret.
  await call('Emulation.setFocusEmulationEnabled', {enabled: true});
  const tree = await call('Page.getFrameTree');
  await call('Page.setDocumentContent', {frameId: tree.frameTree.frame.id, html: page});
  await evaluate('document.fonts.ready.then(() => document.fonts.size)');
  const count = await evaluate('document.querySelectorAll(".field").length');
  const frame = () => evaluate('new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)))');
  const select = (field, a, b) => evaluate(`(() => { const e = document.querySelectorAll('.field')[${field}]; ` +
    `e.focus(); e.setSelectionRange(${a}, ${b}); return true; })()`);
  const fields = [];
  for (let f = 0; f < count; ++f) {
    const length = await evaluate(`document.querySelectorAll('.field')[${f}].value.length`);
    const rect = await evaluate(`(r => [r.left, r.top, r.width, r.height])(` +
      `document.querySelectorAll('.field')[${f}].getBoundingClientRect())`);
    const shot = async () => {
      await frame();
      const data = await call('Page.captureScreenshot', {format: 'png',
        clip: {x: rect[0], y: rect[1], width: rect[2], height: rect[3], scale: 1}});
      return png(Buffer.from(data.data, 'base64'));
    };
    const carets = [];
    for (const offset of [...new Set([0, 1, 3, length - 1, length])].filter((o) => o >= 0 && o <= length)) {
      await select(f, offset, offset);
      const image = await shot();
      carets.push({offset, columns: columns(image, (r, g, b) => g > 160 && r < 120 && b < 120)});
    }
    const selections = [];
    for (const [a, b] of [[0, 2], [1, length - 1], [2, Math.min(length, 7)]]) {
      if (b <= a) continue;
      await select(f, a, b);
      const image = await shot();
      selections.push({start: a, end: b, columns: columns(image, (r, g, bl) => bl > 160 && r < 120 && g < 120)});
    }
    const arrows = [];
    for (const [a, b] of [[0, 0], [2, 2], [length, length], [1, 4]]) {
      for (const key of ['ArrowLeft', 'ArrowRight']) {
        if (b > length) continue;
        await select(f, a, b);
        const command = key === 'ArrowLeft' ? 'moveLeft' : 'moveRight';
        const code = key === 'ArrowLeft' ? 37 : 39;
        await call('Input.dispatchKeyEvent', {type: 'rawKeyDown', key, code: key, windowsVirtualKeyCode: code,
          commands: [command]});
        await call('Input.dispatchKeyEvent', {type: 'keyUp', key, code: key, windowsVirtualKeyCode: code});
        const after = await evaluate(`(e => [e.selectionStart, e.selectionEnd])(` +
          `document.querySelectorAll('.field')[${f}])`);
        arrows.push({start: a, end: b, key, after});
      }
    }
    fields.push({rect, length, carets, selections, arrows});
  }
  writeFileSync(output, JSON.stringify({browser: version.Browser, dpr: 1,
    source: 'headless Chrome, repository fonts as data URLs', fonts, fields}, null, 1) + '\n');
  console.log(`Recorded ${fields.length} fields from ${version.Browser} to ${output}`);
} finally {
  socket?.close();
  const exited = new Promise((r) => child.once('exit', r));
  child.kill();
  await exited;
  rmSync(profile, {recursive: true, force: true, maxRetries: 5, retryDelay: 200});
}
