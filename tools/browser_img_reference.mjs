// Chrome oracle for the img element (src/scene/scene.cpp, src/layout/flex.cpp, src/script/script_dom.cpp), on the
// images of tests/fixtures/img, which it writes when they are missing (PNG files of colored quadrants). A local HTTP
// server serves the images and the pages, so that an img's src is the same relative path in Chrome and in the scene.
//   - layout: cases of img elements (natural sizes, width and height attributes, CSS sizes, min and max sizes,
//     aspect-ratio, box-sizing, blocks, inline images on a line of Ahem text and their vertical-align, flex, grid,
//     absolute positioning, right to left) and random trees of them. Every element's border box relative to the
//     case's block, and each img's naturalWidth, naturalHeight, width and height.
//   - paint: object-fit and object-position on an image of four quadrants (red, green, blue, yellow): from a
//     screenshot of each img, where each quadrant's color lies in the img's border box.
//   - script: tests/fixtures/img/script.js run on a page of its own: what an img reports (complete, its natural and
//     IDL sizes) as its src changes and it loads, the order of its load event, decode(), new Image() and errors.
// Writes tests/fixtures/img/chrome-results.json, which tests/scene_img_tests.cpp compares the scene and a View with.
//
//   node tools/browser_img_reference.mjs [output.json] [chrome executable]
import {spawn} from 'node:child_process';
import {createServer} from 'node:http';
import {readFileSync, writeFileSync, mkdtempSync, existsSync, rmSync} from 'node:fs';
import {createHash} from 'node:crypto';
import {deflateSync, inflateSync} from 'node:zlib';
import {tmpdir} from 'node:os';
import {resolve, join} from 'node:path';
import {fileURLToPath} from 'node:url';

const repo = resolve(fileURLToPath(new URL('..', import.meta.url)));
const directory = resolve(repo, 'tests/fixtures/img');
const output = resolve(process.argv[2] ?? resolve(directory, 'chrome-results.json'));
const defaults = ['C:/Program Files/Google/Chrome/Application/chrome.exe',
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'];
const executable = process.argv[3] ?? defaults.find(existsSync);
if (!executable || !existsSync(executable)) throw new Error('Provide a local Chromium executable as the second argument');

// The test images: w x h pixels in four quadrants, red top left, green top right, blue bottom left, yellow bottom
// right (a quadrant of an odd side takes the extra row or column on its right or bottom).
const crcTable = Array.from({length: 256}, (_, n) => {
  let c = n;
  for (let k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  return c >>> 0;
});
const crc32 = (bytes) => {
  let c = 0xffffffff;
  for (const b of bytes) c = crcTable[(c ^ b) & 255] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
};
const chunk = (type, body) => {
  const out = Buffer.alloc(12 + body.length);
  out.writeUInt32BE(body.length, 0);
  out.write(type, 4, 'ascii');
  body.copy(out, 8);
  out.writeUInt32BE(crc32(out.subarray(4, 8 + body.length)), 8 + body.length);
  return out;
};
const quadrants = [[255, 0, 0], [0, 255, 0], [0, 0, 255], [255, 255, 0]];
const encode = (w, h) => {
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; ++y) {
    raw[y * (w * 3 + 1)] = 0;
    for (let x = 0; x < w; ++x) {
      const q = quadrants[(y < Math.floor(h / 2) ? 0 : 2) + (x < Math.floor(w / 2) ? 0 : 1)];
      raw.set(q, y * (w * 3 + 1) + 1 + x * 3);
    }
  }
  const header = Buffer.alloc(13);
  header.writeUInt32BE(w, 0);
  header.writeUInt32BE(h, 4);
  header[8] = 8;
  header[9] = 2;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', header),
    chunk('IDAT', deflateSync(raw, {level: 9})), chunk('IEND', Buffer.alloc(0))]);
};
const images = {'quad-40x30.png': [40, 30], 'quad-16x16.png': [16, 16], 'tall-20x50.png': [20, 50],
  'wide-120x40.png': [120, 40], 'big-300x200.png': [300, 200], 'dot-1x1.png': [1, 1]};
const bytes = {};
for (const [name, [w, h]] of Object.entries(images)) {
  const path = resolve(directory, name);
  if (!existsSync(path)) writeFileSync(path, encode(w, h));
  bytes[name] = readFileSync(path);
}

// An element: tag, style, attributes and children (a string is a text node).
const el = (tag, style, children = [], attributes = {}) => ({tag, style, attributes, children});
const div = (style, ...children) => el('div', style, children);
const img = (src, style = '', attributes = {}) => el('img', style, [], {src, ...attributes});
const Q = 'quad-40x30.png';
const S = 'quad-16x16.png';
const T = 'tall-20x50.png';
const W = 'wide-120x40.png';
const B = 'big-300x200.png';
const rtl = {dir: 'rtl'};
const cases = [
  {name: 'natural sizes', width: 400, children: [div('', img(Q), img(S), img(T), img(W), img('dot-1x1.png'))]},
  {name: 'natural sizes as blocks', width: 400, children: [img(Q, 'display:block'), img(T, 'display:block'),
    img(W, 'display:block'), img(B, 'display:block')]},
  {name: 'width and height attributes', width: 400, children: [div('',
    img(Q, '', {width: '80'}), img(Q, '', {height: '60'}), img(Q, '', {width: '50', height: '50'}),
    img(T, '', {width: '30'}), img(W, '', {height: '10'}), img(Q, '', {width: '50%'}), img(Q, '', {width: '0'}),
    img(Q, '', {width: '12.5', height: '7.25'}), img(Q, '', {width: '20px'}))]},
  {name: 'css sizes', width: 400, children: [div('',
    img(Q, 'width:80px'), img(Q, 'height:15px'), img(Q, 'width:20px;height:50px'), img(T, 'width:25%'),
    img(W, 'height:50%'), img(Q, 'width:auto;height:auto', {width: '90', height: '9'}),
    img(Q, 'width:60px', {height: '10'}), img(Q, 'height:auto', {width: '60', height: '10'}))]},
  {name: 'padding, border and box-sizing', width: 400, children: [div('',
    img(Q, 'padding:4px;border:2px solid #000'), img(Q, 'padding:4px 8px;border:1px solid #000;width:60px'),
    img(Q, 'box-sizing:content-box;padding:5px;border:3px solid #000;width:60px'),
    img(T, 'padding:2px;height:40px'), img(T, 'box-sizing:content-box;padding:2px;height:40px'),
    img(Q, 'padding:10%'), img(Q, 'padding:3px;aspect-ratio:1;width:40px'),
    img(Q, 'box-sizing:content-box;padding:3px;aspect-ratio:1;width:40px'),
    img(Q, 'padding:5px;min-width:70px'), img(Q, 'box-sizing:content-box;padding:5px;max-width:30px'))]},
  {name: 'max-width 100%', width: 150, children: [img(B, 'max-width:100%'), img(B, 'max-width:100%;height:auto'),
    img(B, 'max-width:100%', {width: '300', height: '200'}), img(B, 'max-width:100%;height:auto',
      {width: '300', height: '200'}), div('width:90px', img(W, 'max-width:100%;display:block'))]},
  {name: 'min and max sizes', width: 400, children: [div('',
    img(Q, 'min-width:80px'), img(Q, 'max-width:20px'), img(Q, 'min-height:60px'), img(Q, 'max-height:15px'),
    img(Q, 'min-width:80px;max-height:40px'), img(T, 'max-width:10px;min-height:60px'),
    img(W, 'max-width:60px;max-height:30px'), img(S, 'min-width:30px;min-height:40px'),
    img(Q, 'width:100px;max-height:30px'), img(Q, 'height:50px;max-width:30px'))]},
  {name: 'aspect-ratio', width: 400, children: [div('',
    img(Q, 'aspect-ratio:1'), img(Q, 'width:50px;aspect-ratio:2 / 1'), img(Q, 'height:30px;aspect-ratio:1 / 2'),
    img(Q, 'aspect-ratio:auto 1 / 1'), img(Q, 'width:60px;aspect-ratio:auto 1 / 1'))]},
  {name: 'block images', width: 300, children: [img(Q, 'display:block;margin:0 auto'),
    img(Q, 'display:block;margin-left:auto'), img(Q, 'display:block;margin-left:10px;margin-right:auto'),
    img(Q, 'display:block;width:50%;margin:0 auto'), div('padding:0 20px', img(B, 'display:block;max-width:100%'))]},
  {name: 'block images right to left', width: 300, attributes: rtl, children: [img(Q, 'display:block'),
    img(Q, 'display:block;margin:0 auto'), img(Q, 'display:block;margin-right:15px'),
    div('', img(Q), ' ', img(S))]},
  {name: 'inline images in text', width: 300, children: [
    div('', 'ab ', img(Q), ' cd ', img(S), ' ef'),
    div('line-height:20px', 'ab ', img(S), ' cd'),
    div('', 'ab', img(Q, 'vertical-align:middle'), 'cd', img(S, 'vertical-align:top'), 'ef',
      img(S, 'vertical-align:bottom')),
    div('', 'x', img(S, 'vertical-align:text-top'), 'x', img(S, 'vertical-align:text-bottom'), 'x',
      img(S, 'vertical-align:-4px'), 'x', img(S, 'vertical-align:5px')),
    div('font-size:20px', 'x', img(S, 'vertical-align:sub'), 'x', img(S, 'vertical-align:super')),
    div('', img(T, 'margin:0 5px')), div('', img(Q, 'padding:3px;border:2px solid #000'), 'ab')]},
  {name: 'images wrapping in a line', width: 120, children: [div('', img(Q), ' ', img(Q), ' ', img(Q), ' ',
    img(Q), ' ', img(W)), div('', img(Q), img(Q), img(Q), img(Q))]},
  {name: 'flex row', width: 400, children: [
    div('display:flex', img(Q), img(T), img(W)),
    div('display:flex;height:60px', img(Q), img(S, 'width:30px')),
    div('display:flex;align-items:flex-start;height:60px', img(Q), img(T)),
    div('display:flex;align-items:center', img(Q), img(T)),
    div('display:flex;width:100px', img(W), img(Q)),
    div('display:flex;width:100px', img(W, 'min-width:0'), img(Q, 'min-width:0')),
    div('display:flex;gap:5px', img(Q, 'height:20px'), img(Q, 'width:20px'), img(Q, '', {width: '30'}))]},
  // Flexible lengths (CSS Flexbox 1 §9.7): the free space from the flex base sizes of the items not frozen at their
  // hypothetical main sizes, then the items whose minimum or maximum sizes the total violation names frozen, in a
  // loop. Yoga took the free space from the bases within their minimum and maximum sizes.
  {name: 'flex grow from zero', width: 400, children: [div('display:flex', img(Q, 'flex:1'), img(S, 'flex:1')),
    div('display:flex', img(Q, 'flex:1;min-width:0'), img(S))]},
  {name: 'flexible lengths', width: 300, children: [
    div('display:flex;width:200px', div('flex:1;min-width:150px;height:10px'), div('flex:1;height:10px')),
    div('display:flex;width:200px', div('flex:1;max-width:50px;height:10px'), div('flex:1;height:10px')),
    div('display:flex;width:200px', div('flex:0 1 300px;max-width:100px;height:10px'),
      div('flex:0 1 300px;height:10px')),
    div('display:flex;width:200px', div('flex:0 1 200px;min-width:150px;height:10px'),
      div('flex:0 1 200px;height:10px')),
    div('display:flex;width:200px', div('flex:0.25;height:10px'), div('flex:0.25;height:10px')),
    div('display:flex;width:200px', div('flex:1;padding:0 20px;height:10px'), div('flex:1;height:10px')),
    div('display:flex;width:300px', img(Q, 'flex:1'), img(W, 'flex:2'), img(S, 'flex:1;max-width:20px')),
    div('display:flex;flex-wrap:wrap;width:250px', div('flex:1 0 120px;max-width:150px;height:10px'),
      div('flex:1 0 120px;height:10px'), div('flex:1;min-width:30px;height:10px')),
    div('display:flex;flex-direction:column;height:100px', div('flex:1;min-height:70px'), div('flex:1'))]},
  // Items that all shrink to their minimum sizes: Yoga took their scaled shrink factors out of the line's total one
  // by one, left a residue of float rounding of the wrong sign and grew them by the free space over it, to 1e8 px.
  {name: 'flex items shrunk to their minimum sizes', width: 300, children: [110, 101, 104].map((w, i) =>
    div(`display:flex;width:${w}px`, div('width:94px;min-width:0;display:flex',
      div('width:66px;min-width:66px;height:10px'), div('width:100%;height:10px')),
    div(`width:${[97, 77, 97][i]}px;height:10px`)))},
  {name: 'flex column', width: 300, children: [
    div('display:flex;flex-direction:column', img(Q), img(W)),
    div('display:flex;flex-direction:column;align-items:flex-start', img(Q), img(W)),
    div('display:flex;flex-direction:column;align-items:center', img(Q, 'width:60px')),
    div('display:flex;flex-direction:column;height:50px', img(Q), img(Q)),
    div('display:flex;flex-direction:column;width:100px', img(B)),
    div('display:flex;flex-direction:column;align-items:flex-start;width:100px', img(B, 'max-width:100%'))]},
  {name: 'grid', width: 300, children: [
    div('display:grid;grid-template-columns:100px 1fr', img(Q), img(W), img(T), img(S)),
    div('display:grid;grid-template-columns:30px 30px', img(Q), img(B)),
    div('display:grid;grid-template-columns:100px 100px;justify-items:stretch;align-items:stretch', img(Q), img(T)),
    div('display:grid;grid-template-columns:1fr;height:80px', img(Q)),
    div('display:grid;grid-template-columns:120px', img(Q, 'width:100%'), img(B, 'max-width:100%'))]},
  {name: 'absolute', width: 300, style: 'position:relative;height:200px', children: [
    img(Q, 'position:absolute;left:5px;top:0'), img(Q, 'position:absolute;right:5px;top:40px'),
    img(Q, 'position:absolute;left:10px;right:10px;top:80px'),
    img(Q, 'position:absolute;top:0;bottom:0;left:100px'),
    img(Q, 'position:absolute;left:150px;top:120px;width:50%')]},
  // The auto margins of an absolutely positioned box with both insets of an axis (CSS 2 §10.3.7, §10.3.8, §10.6.4,
  // §10.6.5): they share the space the insets leave it, an auto margin alone takes all of it; across, where that
  // space is negative, the start margin is 0. Yoga took them as 0.
  {name: 'absolute centered by auto margins', width: 300, style: 'position:relative;height:60px', children: [
    img(Q, 'position:absolute;left:10px;right:10px;top:0;width:auto;margin:0 auto'),
    img(Q, 'position:absolute;top:0;bottom:0;left:60px;margin:auto 0'),
    img(Q, 'position:absolute;top:0;bottom:0;left:0;right:0;margin:auto'),
    div('position:absolute;left:10px;right:10px;top:0;width:100px;height:10px;margin:0 auto'),
    div('position:absolute;left:10px;right:10px;top:12px;width:400px;height:10px;margin:0 auto'),
    div('position:absolute;left:10px;right:10px;top:24px;width:100px;height:10px;margin-left:auto'),
    div('position:absolute;left:10px;right:10px;top:36px;width:100px;height:10px;margin:0 auto 0 20px'),
    div('position:absolute;left:10px;right:10px;top:48px;width:400px;height:10px;margin-left:auto'),
    div('position:absolute;left:0;right:0;top:0;max-width:100px;height:10px;margin:0 auto'),
    div('position:absolute;left:10px;top:12px;width:100px;height:10px;margin:0 auto'),
    div('position:absolute;left:10px;right:10px;top:24px;width:50%;height:10px;margin:0 auto'),
    div('position:absolute;left:10px;right:10px;top:36px;width:100px;height:10px;padding:0 10px;' +
      'border-width:0 5px;margin:0 auto'),
    div('position:absolute;top:10px;bottom:10px;left:100px;width:10px;height:20px;margin:auto 0'),
    div('position:absolute;top:10px;bottom:10px;left:120px;width:10px;height:100px;margin:auto 0'),
    div('position:absolute;top:10px;bottom:10px;left:140px;width:10px;height:20px;margin-top:auto')]},
  {name: 'absolute centered by auto margins right to left', width: 300, attributes: rtl,
    style: 'position:relative;height:60px', children: [
      div('position:absolute;left:10px;right:10px;top:0;width:100px;height:10px;margin:0 auto'),
      div('position:absolute;left:10px;right:10px;top:12px;width:400px;height:10px;margin:0 auto'),
      div('position:absolute;left:10px;right:10px;top:24px;width:100px;height:10px;margin-right:auto'),
      div('position:absolute;left:10px;right:10px;top:36px;width:100px;height:10px'),
      img(Q, 'position:absolute;left:10px;right:10px;top:12px;margin:0 auto')]},
  {name: 'absolute centered by auto margins in a flex container', width: 300,
    style: 'position:relative;height:80px;display:flex;justify-content:flex-end;align-items:center;padding:10px;' +
      'border:5px solid #000', children: [
      div('position:absolute;left:0;right:0;top:0;bottom:0;width:100px;height:20px;margin:auto'),
      img(Q, 'position:absolute;left:10px;right:10px;top:5px;margin:0 auto'), img(S)]},
  {name: 'shrink to fit', width: 300, children: [
    el('span', 'display:inline-block;padding:2px;background:#ccc', [img(Q), img(S)]),
    div('position:relative;height:60px', div('position:absolute;padding:3px', img(W))),
    div('display:flex', div('', img(Q), 'ab'), div('', img(B, 'max-width:100%')))]},
  {name: 'no src', width: 300, children: [div('', 'a', el('img', '', [], {}), 'b',
    el('img', '', [], {width: '20', height: '10'}), 'c', el('img', 'width:30px;height:12px;display:block'))]},
];

// Random trees (a seeded generator, so that the fixture is reproducible): blocks of text and images, flex and grid
// containers, images of random attributes and sizes.
let seed = 20261107;
const random = (n) => {
  seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
  return (seed >>> 8) % n;
};
const pick = (list) => list[random(list.length)];
const randomImage = (block = false) => {
  const attributes = {};
  if (!random(4)) attributes.width = String(10 + random(80));
  if (!random(4)) attributes.height = String(8 + random(50));
  let style = '';
  if (!random(4)) style += `width:${pick([`${16 + random(80)}px`, '50%', '100%', '25%'])};`;
  if (!random(5)) style += `height:${pick([`${8 + random(50)}px`, 'auto'])};`;
  if (!random(5)) style += `max-width:${pick(['100%', '60px', '50%'])};`;
  if (!random(7)) style += `min-width:${pick(['30px', '80px'])};`;
  if (!random(7)) style += `max-height:${pick(['20px', '40px'])};`;
  if (!random(8)) style += `min-height:${pick(['30px', '60px'])};`;
  if (!random(7)) style += `aspect-ratio:${pick(['1', '2 / 1', 'auto 1 / 2'])};`;
  if (!random(5)) style += `padding:${random(4) * 2}px;border:${random(3)}px solid #000;`;
  if (!random(7)) style += 'box-sizing:border-box;';
  if (block || !random(5)) style += 'display:block;';
  if (!random(6)) style += `vertical-align:${pick(['middle', 'top', 'bottom', 'text-top', '-3px', 'text-bottom'])};`;
  if (!random(6)) style += `margin:0 ${pick(['4px', 'auto', '0'])} 0 ${pick(['2px', 'auto', '0'])};`;
  return img(pick([Q, S, T, W, B]), style, attributes);
};
const words = ['ab', 'cde', 'f', 'ghij', 'kl mn'];
const container = (depth) => {
  const kind = random(depth ? 3 : 4);
  const children = [];
  for (let k = 0, n = 1 + random(4); k < n; ++k) {
    if (depth < 1 && random(4) === 0) children.push(container(depth + 1));
    else if (kind === 0 && random(3) === 0) children.push(pick(words) + ' ');
    else children.push(randomImage());
  }
  const common = (random(3) ? '' : `width:${60 + random(200)}px;`) + (random(4) ? '' : 'direction:rtl;') +
    (random(4) ? '' : `padding:0 ${random(3) * 5}px;`);
  if (kind === 0) return div(common + (random(3) ? '' : 'line-height:' + pick(['20px', '1.5']) + ';'), ...children);
  if (kind === 1) {
    return div(common + 'display:flex;' + pick(['', 'flex-direction:column;', 'flex-wrap:wrap;']) +
      pick(['', 'align-items:flex-start;', 'align-items:center;', 'align-items:flex-end;']) +
      (random(3) ? '' : `height:${40 + random(80)}px;`) + (random(2) ? `gap:${random(6)}px;` : ''),
    ...children.filter((c) => typeof c !== 'string'));
  }
  if (kind === 2) {
    return div(common + 'display:grid;grid-template-columns:' + pick(['60px 1fr', 'repeat(3, 40px)', '1fr 1fr']) +
      ';' + pick(['', 'justify-items:center;', 'align-items:end;', 'justify-items:stretch;']),
    ...children.filter((c) => typeof c !== 'string'));
  }
  return div(common, ...children.filter((c) => typeof c !== 'string').map(() => randomImage(true)));
};
for (let i = 0; i < 120; ++i) {
  const children = [];
  for (let k = 0, n = 1 + random(3); k < n; ++k) children.push(container(0));
  cases.push({name: 'random ' + i, width: 120 + random(250), children});
}

// object-fit and object-position: an image of quadrants in a box of another shape, so that each fit shows.
const fits = [];
for (const [src, size] of [[Q, 'width:80px;height:80px'], [T, 'width:90px;height:40px'],
  [W, 'width:50px;height:70px'], [S, 'width:60px;height:30px']]) {
  for (const fit of ['fill', 'contain', 'cover', 'none', 'scale-down']) {
    fits.push({src, style: `${size};object-fit:${fit}`});
  }
}
for (const position of ['left top', 'right bottom', '25% 75%', '10px 5px', 'right 4px bottom 6px', 'center top']) {
  fits.push({src: Q, style: `width:80px;height:80px;object-fit:none;object-position:${position}`});
  fits.push({src: T, style: `width:90px;height:40px;object-fit:contain;object-position:${position}`});
}
fits.push({src: Q, style: 'width:80px;height:60px;padding:5px 10px;border:3px solid #000;object-fit:contain;' +
  'background:#fff'});
fits.push({src: Q, style: 'width:80px;height:60px;padding:4px;object-fit:cover;border-radius:12px'});
fits.push({src: B, style: 'width:70px;height:70px;object-fit:none;object-position:-20px -30px'});

const escape = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/"/g, '&quot;');
const attributes = (n) => Object.entries(n.attributes ?? {}).map(([k, v]) => ` ${k}="${escape(v)}"`).join('');
const html = (n) => {
  if (typeof n === 'string') return escape(n);
  if (n.tag === 'img') return `<img style="${escape(n.style)}"${attributes(n)}>`;
  return `<${n.tag} style="${escape(n.style)}"${attributes(n)}>${n.children.map(html).join('')}</${n.tag}>`;
};
const ahem = readFileSync(resolve(repo, 'third_party/ahem/Ahem.ttf'));
// The scene's UA sheet gives every element box-sizing border-box and solid borders of no width.
const head = '<!doctype html><html><head><meta charset=utf-8><style>@font-face{font-family:Ahem;src:url(Ahem.ttf)}' +
  '*{box-sizing:border-box;border:0 solid}body{margin:0;font:10px Ahem;background:#fff}';
const layoutPage = head + '.row{position:relative;height:420px}</style></head><body>' +
  cases.map((c, i) => `<div class=row><div id=case${i} style="width:${c.width}px;${escape(c.style ?? '')}"` +
    `${attributes(c)}>` + c.children.map(html).join('') + '</div></div>').join('') + '</body></html>';
const paintPage = head + '.fit{display:block;margin:10px;background:#fff}</style></head><body>' +
  fits.map((f) => `<img class=fit src="${f.src}" style="${escape(f.style)}">`).join('') + '</body></html>';
const scriptPage = head + '</style></head><body><div id=root></div><script src="script.js"></script></body></html>';

const measure = `(() => {
  const out = [];
  for (let i = 0; ; ++i) {
    const root = document.getElementById('case' + i);
    if (!root) break;
    const origin = root.getBoundingClientRect();
    const elements = [...root.querySelectorAll('*')].map((e) => {
      const r = e.getBoundingClientRect();
      const o = {tag: e.localName, rect: [r.left - origin.left, r.top - origin.top, r.width, r.height]};
      if (e.localName === 'img') o.image = [e.naturalWidth, e.naturalHeight, e.width, e.height, e.complete];
      return o;
    });
    out.push({width: origin.width, height: origin.height, elements});
  }
  return JSON.stringify(out);
})()`;

// A PNG's pixels as RGBA (8-bit RGB or RGBA, not interlaced).
const png = (data) => {
  let at = 8;
  let width = 0;
  let height = 0;
  let channels = 4;
  const parts = [];
  while (at < data.length) {
    const length = data.readUInt32BE(at);
    const type = data.toString('ascii', at + 4, at + 8);
    const body = data.subarray(at + 8, at + 8 + length);
    if (type === 'IHDR') {
      width = body.readUInt32BE(0);
      height = body.readUInt32BE(4);
      if (body[8] !== 8 || body[12] !== 0 || (body[9] !== 6 && body[9] !== 2)) throw new Error('PNG profile');
      channels = body[9] === 6 ? 4 : 3;
    } else if (type === 'IDAT') {
      parts.push(body);
    }
    at += 12 + length;
  }
  const raw = inflateSync(Buffer.concat(parts));
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

// Where each quadrant's color lies in an image: [left, top, right, bottom] of the pixels within 40 levels of it on
// each channel (null for a color not there).
const regions = (image) => quadrants.map((q) => {
  let box = null;
  for (let y = 0; y < image.height; ++y) {
    for (let x = 0; x < image.width; ++x) {
      const k = (y * image.width + x) * 4;
      if (Math.abs(image.data[k] - q[0]) <= 40 && Math.abs(image.data[k + 1] - q[1]) <= 40 &&
          Math.abs(image.data[k + 2] - q[2]) <= 40) {
        if (!box) box = [x, y, x + 1, y + 1];
        box = [Math.min(box[0], x), Math.min(box[1], y), Math.max(box[2], x + 1), Math.max(box[3], y + 1)];
      }
    }
  }
  return box;
});

const files = {...bytes, 'Ahem.ttf': ahem, 'script.js': readFileSync(resolve(directory, 'script.js')),
  'layout.html': Buffer.from(layoutPage), 'paint.html': Buffer.from(paintPage),
  'script.html': Buffer.from(scriptPage)};
const server = createServer((request, response) => {
  const name = decodeURIComponent(new URL(request.url, 'http://localhost').pathname.slice(1));
  const body = files[name];
  if (!body) {
    response.writeHead(404);
    response.end();
    return;
  }
  const type = name.endsWith('.png') ? 'image/png' : name.endsWith('.html') ? 'text/html; charset=utf-8' :
    name.endsWith('.js') ? 'text/javascript' : 'font/ttf';
  response.writeHead(200, {'content-type': type, 'cache-control': 'no-store'});
  response.end(body);
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const origin = `http://127.0.0.1:${server.address().port}/`;

const profile = mkdtempSync(join(tmpdir(), 'weft-img-'));
const child = spawn(executable, ['--headless=new', '--no-first-run', '--disable-background-networking',
  '--remote-debugging-port=0', '--force-device-scale-factor=1', '--window-size=1000,800', '--user-data-dir=' + profile,
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
  const evaluate = async (expression) =>
    (await call('Runtime.evaluate', {expression, returnByValue: true, awaitPromise: true})).result.value;
  const open = async (name) => {
    const done = new Promise((r) => loaded.push(r));
    await call('Page.navigate', {url: origin + name});
    await done;
    await evaluate('document.fonts.ready.then(() => Promise.all([...document.images].map((i) => ' +
      'i.decode().catch(() => null)))).then(() => new Promise((r) => requestAnimationFrame(() => ' +
      'requestAnimationFrame(r)))).then(() => true)');
  };
  await call('Page.enable');

  await open('layout.html');
  const measured = JSON.parse(await evaluate(measure));
  const layout = cases.map((c, i) => ({...c, chrome: measured[i]}));

  await open('paint.html');
  const rects = await evaluate('[...document.querySelectorAll(".fit")].map((e) => { const r = ' +
    'e.getBoundingClientRect(); return [r.left + scrollX, r.top + scrollY, r.width, r.height]; })');
  const paint = [];
  for (let k = 0; k < fits.length; ++k) {
    const r = rects[k];
    const shot = await call('Page.captureScreenshot', {format: 'png', captureBeyondViewport: true,
      clip: {x: r[0], y: r[1], width: r[2], height: r[3], scale: 1}});
    paint.push({...fits[k], rect: [0, 0, r[2], r[3]],
      quadrants: regions(png(Buffer.from(shot.data, 'base64')))});
  }

  await open('script.html');
  const script = await evaluate('new Promise((r) => { const t = () => window.__imgLog ? r(window.__imgLog) : ' +
    'setTimeout(t, 20); t(); })');

  writeFileSync(output, JSON.stringify({browser: version.Browser, dpr: 1,
    source: 'headless Chrome, images and Ahem from a local HTTP server',
    font: {family: 'Ahem', path: 'third_party/ahem/Ahem.ttf',
      sha256: createHash('sha256').update(ahem).digest('hex')},
    images: Object.fromEntries(Object.entries(bytes).map(([name, b]) =>
      [name, createHash('sha256').update(b).digest('hex')])),
    cases: layout, paint, script}, null, 1) + '\n');
  console.log(`Recorded ${layout.length} layout cases, ${paint.length} fits and ${script.length} script entries ` +
    `from ${version.Browser} to ${output}`);
} finally {
  socket?.close();
  server.close();
  const exited = new Promise((r) => child.once('exit', r));
  child.kill();
  await exited;
  rmSync(profile, {recursive: true, force: true, maxRetries: 5, retryDelay: 200});
}
