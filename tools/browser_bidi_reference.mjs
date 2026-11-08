// Chrome oracle for bidirectional text and complex scripts in scene inline formatting contexts
// (src/scene/scene_inline.cpp, src/scene/scene_text.cpp): small blocks of Arabic, Hebrew, Thai and Devanagari text
// mixed with Latin, inline boxes and atomic inlines, under direction, unicode-bidi, the dir attribute, bdi and bdo, in
// a block of a given width. Records the block's height, every element's client rects (an inline element's fragments,
// split where reordering separates its content) and every text node's fragments (Range.getClientRects: a rect per
// line and per run of one bidi level) and each of its code points' rect. Portable: the fonts are files in the repository (HarfBuzz's test fonts),
// embedded as data URLs, so that the scene test (tests/scene_bidi_tests.cpp) shapes with the same faces.
//
//   node tools/browser_bidi_reference.mjs [output.json] [chrome executable]
import {spawn} from 'node:child_process';
import {readFileSync, writeFileSync, mkdtempSync, existsSync, rmSync} from 'node:fs';
import {createHash} from 'node:crypto';
import {inflateSync} from 'node:zlib';
import {tmpdir} from 'node:os';
import {resolve, join} from 'node:path';
import {fileURLToPath} from 'node:url';

const repo = resolve(fileURLToPath(new URL('..', import.meta.url)));
const output = resolve(process.argv[2] ?? resolve(repo, 'tests/fixtures/scene_bidi_reference.json'));
const defaults = ['C:/Program Files/Google/Chrome/Application/chrome.exe',
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'];
const executable = process.argv[3] ?? defaults.find(existsSync);
if (!executable || !existsSync(executable)) throw new Error('Provide a local Chromium executable as the second argument');

// The faces: a family name each, as the scene test's catalog names them.
const fonts = [
  {family: 'Roboto', path: 'third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf'},
  {family: 'TestArabic', path: 'third_party/harfbuzz/source/perf/fonts/Amiri-Regular.ttf'},
  {family: 'TestHebrew', path: 'third_party/harfbuzz/source/test/subset/data/fonts/NotoIKEAHebrewLatin-Regular.ttf'},
  {family: 'TestThai', path: 'third_party/harfbuzz/source/test/fuzzing/fonts/kanit.ttf'},
  {family: 'TestDevanagari', path: 'third_party/harfbuzz/source/test/subset/data/fonts/NotoSansDevanagari-Regular.ttf'},
];
const files = fonts.map((f) => ({...f, bytes: readFileSync(resolve(repo, f.path))}));

// A case: the block's width, style and attributes, and its children: a string is a text node, an object an element.
const el = (tag, style, children, attributes = {}) => ({tag, style, attributes, children});
const span = (style, ...children) => el('span', style, children);
const family = 'font-family:Roboto, TestArabic, TestHebrew, TestThai, TestDevanagari;';
const base = family + 'font-size:20px;line-height:normal';
const fixed = family + 'font-size:20px;line-height:30px';
const box = 'padding:0 6px 0 2px;margin:0 3px 0 1px;border-left:2px solid #888;border-right:4px solid #444';
const cases = [
  {name: 'arabic paragraph', width: 220, style: base, attributes: {dir: 'rtl'},
    children: ['مرحبا بالعالم، هذه فقرة عربية طويلة تلتف على عدة أسطر في صندوق ضيق.']},
  {name: 'hebrew with latin and digits', width: 260, style: fixed, attributes: {dir: 'rtl'},
    children: ['שלום world 123 עולם, version 2.0 של התוכנה (beta) עכשיו!']},
  {name: 'arabic in a left-to-right line', width: 400, style: fixed,
    children: ['Price: ', span('', '١٢٣ ريال'), ' (about 33 USD) and اللغة العربية here.']},
  {name: 'direction property', width: 300, style: fixed + ';direction:rtl',
    children: ['Hello, عالم! 42']},
  {name: 'neutral at the end', width: 300, style: fixed, attributes: {dir: 'rtl'},
    children: ['Hello!']},
  {name: 'spans across levels', width: 360, style: fixed, attributes: {dir: 'rtl'},
    children: ['أول ', span(box, 'English ', span('border-left:1px solid red;padding-left:3px', 'inner عربي'), ' tail'),
      ' آخر']},
  {name: 'rtl span in ltr', width: 400, style: fixed,
    children: ['before ', el('span', box, ['עברית abc 12'], {dir: 'rtl'}), ' after']},
  {name: 'rtl span wraps', width: 150, style: fixed,
    children: ['ab ', el('span', box, ['אחת שתיים שלוש ארבע'], {dir: 'rtl'}), ' cd']},
  {name: 'bdi', width: 400, style: fixed,
    children: ['User ', el('bdi', '', ['إيمان']), ': 3 posts, user ', el('bdi', '', ['Bob']), ': 5']},
  {name: 'no bdi', width: 400, style: fixed,
    children: ['User ', span('', 'إيمان'), ': 3 posts, user ', span('', 'Bob'), ': 5']},
  {name: 'bdo', width: 400, style: fixed,
    children: ['ltr ', el('bdo', '', ['abc def'], {dir: 'rtl'}), ' end']},
  {name: 'embed', width: 400, style: fixed,
    children: ['x ', span('direction:rtl;unicode-bidi:embed', 'a b c'), ' y']},
  {name: 'isolate-override', width: 400, style: fixed,
    children: ['x ', span('direction:rtl;unicode-bidi:isolate-override', 'ab cd'), ' y']},
  {name: 'block override', width: 300, style: fixed + ';direction:rtl;unicode-bidi:bidi-override',
    children: ['abc def', el('br', '', []), 'ghi']},
  {name: 'plaintext paragraphs', width: 300, style: fixed + ';unicode-bidi:plaintext',
    children: ['עברית ואז abc', el('br', '', []), 'Latin then עברית']},
  {name: 'dir auto', width: 300, style: fixed, attributes: {dir: 'auto'},
    children: ['مرحبا Hello']},
  {name: 'brackets', width: 400, style: fixed, attributes: {dir: 'rtl'},
    children: ['קישור (link) [שני] ו-{אחר} <x>']},
  {name: 'text-align end', width: 300, style: fixed + ';text-align:end', attributes: {dir: 'rtl'},
    children: ['עברית']},
  {name: 'text-align left', width: 300, style: fixed + ';text-align:left', attributes: {dir: 'rtl'},
    children: ['עברית']},
  {name: 'text-align center', width: 300, style: fixed + ';text-align:center', attributes: {dir: 'rtl'},
    children: ['עברית ואנגלית']},
  {name: 'overflowing rtl line', width: 60, style: fixed, attributes: {dir: 'rtl'},
    children: ['ארוךארוךארוךארוך קצר']},
  {name: 'inline block in rtl', width: 300, style: fixed, attributes: {dir: 'rtl'},
    children: ['قبل ', span('display:inline-block;width:40px;height:20px;margin-left:5px;margin-right:9px'), ' بعد abc']},
  {name: 'atomic between latin in rtl', width: 300, style: fixed, attributes: {dir: 'rtl'},
    children: ['abc ', span('display:inline-block;width:30px;height:10px'), ' def']},
  {name: 'thai', width: 400, style: base,
    children: ['ภาษาไทย สวัสดีครับ ที่นี่ กรุงเทพฯ']},
  {name: 'devanagari', width: 200, style: base,
    children: ['नमस्ते दुनिया, यह क्षेत्र हिन्दी में लिखा गया है।']},
  {name: 'mixed scripts', width: 400, style: base,
    children: ['Latin ไทย हिन्दी עברית العربية 2024']},
  {name: 'arabic lam alef and marks', width: 400, style: base, attributes: {dir: 'rtl'},
    children: ['لا إله إلا الله، مُحَمَّدٌ']},
  {name: 'arabic numbers', width: 400, style: fixed, attributes: {dir: 'rtl'},
    children: ['العدد ١٢٣٤ و 5678 و ٣.١٤']},
  {name: 'nested isolates', width: 420, style: fixed,
    children: ['a ', el('span', '', ['ב ', el('span', '', ['c ד'], {dir: 'ltr'}), ' ה'], {dir: 'rtl'}), ' f']},
];

// Text across elements: Blink shapes adjacent text of one font and direction together (a span that only changes the
// color), and shapes every run in its paragraph's context (an Arabic letter joins across a span that breaks
// shaping, one with padding or another font size).
const red = 'color:#cc0000';
cases.push(
  {name: 'arabic word split by a color span', width: 400, style: fixed, attributes: {dir: 'rtl'},
    children: ['كت', span(red, 'اب'), ' جديد ومـ', span(red, 'ـكتبة')]},
  {name: 'arabic word split by a padded span', width: 400, style: fixed, attributes: {dir: 'rtl'},
    children: ['مكت', span('padding:0 4px', 'ب'), 'ة كبيرة']},
  {name: 'arabic word split by a larger span', width: 400, style: fixed, attributes: {dir: 'rtl'},
    children: ['سل', span('font-size:26px', 'ام'), ' عليكم']},
  {name: 'latin kerning across spans', width: 400, style: fixed,
    children: ['A', span(red, 'V'), 'A WA', span(red, 'Y'), ' To']},
  {name: 'arabic mark in a span', width: 400, style: fixed, attributes: {dir: 'rtl'},
    children: ['م', span(red, 'ُ'), 'حمد']},
  {name: 'devanagari split by a span', width: 400, style: base,
    children: ['नम', span(red, 'स्ते'), ' दुनि', span(red, 'या')]},
);

// text-overflow: ellipsis on lines that are reordered or right to left: the block's color (red) is the ellipsis's
// only, the text is black, so that a screenshot shows where each is (ink). Recorded as the x ranges of red and of
// dark pixels in the block.
const clipped = family + 'font-size:20px;line-height:30px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;' +
  'color:#ff0000';
const ink = (text) => span('color:#000000', text);
const ellipsisCases = [
  {name: 'ellipsis rtl', width: 150, style: clipped, attributes: {dir: 'rtl'},
    children: [ink('שלום עולם, זו שורה ארוכה מאוד')]},
  {name: 'ellipsis arabic', width: 140, style: clipped, attributes: {dir: 'rtl'},
    children: [ink('مرحبا بالعالم هذا سطر طويل جدا')]},
  {name: 'ellipsis ltr text in rtl line', width: 150, style: clipped, attributes: {dir: 'rtl'},
    children: [ink('abcdefghijklmnopqrstuvwxyz')]},
  {name: 'ellipsis rtl text in ltr line', width: 150, style: clipped,
    children: [ink('ab שלום עולם ארוך מאוד')]},
  {name: 'ellipsis mixed rtl line', width: 170, style: clipped, attributes: {dir: 'rtl'},
    children: [ink('שלום world עולם 12345 ארוך')]},
  {name: 'ellipsis ltr', width: 150, style: clipped, children: [ink('The quick brown fox jumps over')]},
];
for (const c of ellipsisCases) c.ink = true;
cases.push(...ellipsisCases);

// Random cases (a seeded generator, so that the fixture is reproducible): words of every script mixed with digits and
// punctuation in text nodes, spans of random direction, unicode-bidi and edges, bdi, inline blocks and breaks, in
// blocks of random direction, alignment and width, most of them narrow enough to wrap. Thai words are single
// dictionary words (Chrome breaks Thai inside words by its dictionary, the scene does not yet).
let seed = 20261107;
const random = (n) => {
  seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
  return (seed >>> 8) % n;
};
const pick = (list) => list[random(list.length)];
const words = {
  latin: ['alpha', 'beta', 'gamma', 'delta', 'word', 'test', 'Chrome', 'weft', 'is', 'a'],
  hebrew: ['שלום', 'עולם', 'מילה', 'בדיקה', 'ספר', 'של', 'ו'],
  arabic: ['مرحبا', 'عالم', 'كلمة', 'اختبار', 'كتاب', 'لا', 'في', 'مُحَمَّد'],
  digits: ['123', '4.5', '2024', '١٢٣', '٤٫٥', '10%', '$7', '-3'],
  marks: ['(', ')', '!', ',', '-', ':', '?', '"', '[', ']'],
  thai: ['ภาษา', 'ไทย', 'ครับ', 'บ้าน'],
  devanagari: ['नमस्ते', 'दुनिया', 'हिन्दी', 'क्षेत्र'],
};
const scripts = ['latin', 'latin', 'hebrew', 'hebrew', 'arabic', 'arabic', 'digits', 'marks', 'thai', 'devanagari'];
const phrase = (n) => {
  let s = '';
  let thai = false;
  for (let k = 0; k < n; ++k) {
    const script = pick(scripts);
    const w = pick(words[script]);
    // Two Thai words always apart: Chrome would break between them by its dictionary.
    s += (k && (random(5) || (thai && script === 'thai')) ? ' ' : '') + w;
    thai = script === 'thai';
  }
  return s;
};
const spanStyles = ['', '', box, 'border-left:3px solid #c00;padding-right:5px', 'direction:rtl;unicode-bidi:embed',
  'direction:ltr;unicode-bidi:isolate', 'direction:rtl;unicode-bidi:isolate', 'direction:rtl;unicode-bidi:bidi-override',
  'unicode-bidi:plaintext', 'direction:rtl;unicode-bidi:isolate-override;' + box];
// Adjacent strings are one text node in the page's HTML.
const merge = (children) => children.reduce((out, c) => {
  if (typeof c === 'string' && typeof out[out.length - 1] === 'string') out[out.length - 1] += c;
  else out.push(c);
  return out;
}, []);
const inline = (depth) => {
  const kind = random(10);
  if (kind < 4 || depth > 2) return (random(2) ? ' ' : '') + phrase(1 + random(4)) + (random(2) ? ' ' : '');
  if (kind < 7) {
    const attributes = random(4) === 0 ? {dir: pick(['rtl', 'ltr'])} : {};
    return el('span', pick(spanStyles), merge([inline(depth + 1), ...(random(2) ? [inline(depth + 1)] : [])]),
      attributes);
  }
  if (kind === 7) return el('bdi', '', [phrase(1 + random(3))]);
  if (kind === 8) return span('display:inline-block;width:' + (10 + random(30)) + 'px;height:12px;margin-left:' +
    random(6) + 'px');
  return el('br', '', []);
};
for (let i = 0; i < 200; ++i) {
  const children = [];
  for (let k = 0, n = 2 + random(5); k < n; ++k) children.push(inline(0));
  const attributes = random(3) ? {dir: pick(['rtl', 'rtl', 'ltr', 'auto'])} : {};
  const align = pick(['', '', ';text-align:end', ';text-align:center', ';text-align:left', ';text-align:right']);
  cases.push({name: 'random ' + i, width: 90 + random(260), style: fixed + align, attributes, children: merge(children)});
}

const escape = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/"/g, '&quot;');
const attributes = (n) => Object.entries(n.attributes ?? {}).map(([k, v]) => ` ${k}="${escape(v)}"`).join('');
const html = (n) => {
  if (typeof n === 'string') return escape(n);
  if (n.tag === 'br') return '<br>';
  return `<${n.tag} style="${escape(n.style)}"${attributes(n)}>${n.children.map(html).join('')}</${n.tag}>`;
};
const faces = files.map((f) => `@font-face{font-family:${f.family};src:url(data:font/ttf;base64,` +
  f.bytes.toString('base64') + `)}`).join('');
// Each case's block at the top left of its own positioned row; the box model of the scene's UA sheet.
const page = '<!doctype html><html><head><meta charset=utf-8><style>' + faces +
  '*{box-sizing:border-box;border:0 solid;margin:0;padding:0}html,body{margin:0;background:#ffffff}.row{position:relative;height:300px}' +
  '</style></head><body>' +
  cases.map((c, i) => `<div class=row><div id=case${i} style="width:${c.width}px;${escape(c.style)}"` +
    `${attributes(c)}>` + c.children.map(html).join('') + '</div></div>').join('') + '</body></html>';

const measure = `(() => {
  const out = [];
  for (let i = 0; ; ++i) {
    const root = document.getElementById('case' + i);
    if (!root) break;
    const origin = root.getBoundingClientRect();
    const rel = (r) => [r.left - origin.left, r.top - origin.top, r.width, r.height];
    const elements = [];
    const texts = [];
    const walk = (n) => {
      for (const c of n.childNodes) {
        if (c.nodeType === 1) {
          elements.push({tag: c.localName, rects: [...c.getClientRects()].map(rel)});
          walk(c);
        } else if (c.nodeType === 3) {
          const range = document.createRange();
          range.selectNodeContents(c);
          const fragments = [...range.getClientRects()].filter((r) => r.width > 0).map(rel);
          // Each code point's rect where it has one: [UTF-16 offset, left, width, top].
          const chars = [];
          for (let k = 0; k < c.data.length;) {
            const size = c.data.codePointAt(k) > 0xffff ? 2 : 1;
            range.setStart(c, k);
            range.setEnd(c, k + size);
            const rects = [...range.getClientRects()].filter((r) => r.width > 0);
            if (rects.length === 1) chars.push([k, ...rel(rects[0])]);
            k += size;
          }
          texts.push({fragments, chars});
        }
      }
    };
    walk(root);
    out.push({width: origin.width, height: origin.height, elements, texts});
  }
  return JSON.stringify(out);
})()`;

// A PNG's pixels as RGBA (8-bit RGB or RGBA, not interlaced), for the screenshots.
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

const profile = mkdtempSync(join(tmpdir(), 'weft-bidi-'));
const child = spawn(executable, ['--headless=new', '--no-first-run', '--disable-background-networking',
  '--remote-debugging-port=0', '--force-device-scale-factor=1', '--window-size=1000,800', '--user-data-dir=' + profile,
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
  await call('Page.enable');
  const tree = await call('Page.getFrameTree');
  await call('Page.setDocumentContent', {frameId: tree.frameTree.frame.id, html: page});
  await call('Runtime.evaluate', {expression: 'document.fonts.ready.then(() => document.fonts.size)',
    awaitPromise: true});
  const loaded = (await call('Runtime.evaluate', {returnByValue: true, expression:
    `[...document.fonts].map((f) => f.family + ':' + f.status).join(',')`})).result.value;
  if (loaded.includes('error') || loaded.includes('unloaded') && !loaded.includes('loaded')) {
    throw new Error('fonts did not load: ' + loaded);
  }
  const measured = JSON.parse((await call('Runtime.evaluate', {expression: measure, returnByValue: true})).result.value);
  // Each ink case's block, captured: the x ranges (from the block's left) of red and of dark pixels.
  for (let i = 0; i < cases.length; ++i) {
    if (!cases[i].ink) continue;
    const box = JSON.parse((await call('Runtime.evaluate', {returnByValue: true, expression:
      `JSON.stringify((r => [r.left + scrollX, r.top + scrollY, r.width, r.height])(` +
      `document.getElementById('case${i}').getBoundingClientRect()))`})).result.value);
    const shot = await call('Page.captureScreenshot', {format: 'png', captureBeyondViewport: true,
      clip: {x: box[0], y: box[1], width: box[2], height: box[3], scale: 1}});
    const image = png(Buffer.from(shot.data, 'base64'));
    const range = (test) => {
      let lo = Infinity;
      let hi = -Infinity;
      for (let y = 0; y < image.height; ++y) {
        for (let x = 0; x < image.width; ++x) {
          const k = (y * image.width + x) * 4;
          if (test(image.data[k], image.data[k + 1], image.data[k + 2])) {
            lo = Math.min(lo, x);
            hi = Math.max(hi, x + 1);
          }
        }
      }
      return lo <= hi ? [lo, hi] : null;
    };
    measured[i].ink = {ellipsis: range((r, g, b) => r > 160 && g < 120 && b < 120),
      text: range((r, g, b) => r < 110 && g < 110 && b < 110)};
  }
  const results = cases.map((c, i) => ({...c, chrome: measured[i]}));
  writeFileSync(output, JSON.stringify({browser: version.Browser, dpr: 1,
    source: 'headless Chrome, repository fonts as data URLs',
    fonts: files.map((f) => ({family: f.family, path: f.path,
      sha256: createHash('sha256').update(f.bytes).digest('hex')})),
    font_status: loaded, cases: results}, null, 1) + '\n');
  console.log(`Recorded ${results.length} bidi cases from ${version.Browser} to ${output}`);
} finally {
  socket?.close();
  const exited = new Promise((r) => child.once('exit', r));
  child.kill();
  await exited;
  rmSync(profile, {recursive: true, force: true, maxRetries: 5, retryDelay: 200});
}
