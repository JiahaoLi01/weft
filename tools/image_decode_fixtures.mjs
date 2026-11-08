// The images of tests/fixtures/image_decode, which tools/browser_image_decode_reference.mjs records Chrome's decoding of
// and tests/image_decode_tests.cpp compares weft's with: source images drawn here (an odd-sized picture of gradients,
// circles and fine stripes; a picture with gradient alpha; an asymmetric picture to tell orientations apart), encoded
// by the tools of the vendored libjpeg-turbo and libwebp (built from third_party apart from weft's build, below), and
// libjpeg-turbo's own test JPEGs. Writes the files and sources.json (what made each).
//
//   cmake -S third_party/libjpeg-turbo/source -B <jpeg> -G Ninja -DWITH_SIMD=OFF -DENABLE_SHARED=OFF -DWITH_TESTS=OFF
//   cmake --build <jpeg> --target cjpeg-static
//   cmake -S third_party/libwebp/source -B <webp> -G Ninja -DWEBP_BUILD_CWEBP=ON -DWEBP_BUILD_IMG2WEBP=ON
//   cmake --build <webp> --target cwebp img2webp
//   node tools/image_decode_fixtures.mjs <jpeg>/cjpeg-static <webp>/cwebp <webp>/img2webp
import {execFileSync} from 'node:child_process';
import {copyFileSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

const repo = resolve(fileURLToPath(new URL('..', import.meta.url)));
const out = resolve(repo, 'tests/fixtures/image_decode');
const [cjpeg, cwebp, img2webp] = process.argv.slice(2);
if (!cjpeg || !cwebp || !img2webp) throw new Error('Pass the paths of cjpeg, cwebp and img2webp');
mkdirSync(out, {recursive: true});
const work = mkdtempSync(join(tmpdir(), 'weft-image-fixtures-'));

// A picture as RGBA bytes from pixel(x, y) -> [r, g, b, a].
const picture = (w, h, pixel) => {
  const data = Buffer.alloc(w * h * 4);
  for (let y = 0; y < h; ++y) {
    for (let x = 0; x < w; ++x) data.set(pixel(x, y).map((v) => Math.max(0, Math.min(255, Math.round(v)))), (y * w + x) * 4);
  }
  return {w, h, data};
};
const ppm = (p) => {
  const body = Buffer.alloc(p.w * p.h * 3);
  for (let i = 0; i < p.w * p.h; ++i) p.data.copy(body, i * 3, i * 4, i * 4 + 3);
  return Buffer.concat([Buffer.from(`P6\n${p.w} ${p.h}\n255\n`), body]);
};
const pam = (p) => Buffer.concat([Buffer.from(`P7\nWIDTH ${p.w}\nHEIGHT ${p.h}\nDEPTH 4\nMAXVAL 255\n` +
  'TUPLTYPE RGB_ALPHA\nENDHDR\n'), p.data]);

// 97 x 61: partial MCUs on both axes for every subsampling.
const scene = picture(97, 61, (x, y) => {
  let c = [30 + x * 2, 60 + y * 2.5, 200 - x];
  const d = Math.hypot(x - 64, y - 24);
  if (d < 14) c = [250, 210 - d * 4, 60];
  if (Math.hypot(x - 22, y - 40) < 11) c = [40, 170, 90 + x];
  if (y > 48) c = (x >> 1) % 2 ? [240, 240, 240] : [20, 20, 30];
  if (x > 80 && y < 20) c = (x + y) % 3 ? [200, 30, 40] : [30, 30, 200];
  return [...c, 255];
});
const alpha = picture(64, 48, (x, y) => {
  const a = Math.max(0, 255 - Math.hypot(x - 32, y - 24) * 9);
  return [255 - x * 3, 80 + y * 3, 120, x < 6 ? 0 : x > 56 ? 255 : a];
});
// Red top left, green top right, blue bottom left, yellow bottom right, a white mark near the top left corner.
const quad = picture(40, 24, (x, y) => {
  if (x >= 3 && x < 9 && y >= 3 && y < 7) return [255, 255, 255, 255];
  const q = [[255, 0, 0], [0, 200, 0], [0, 0, 255], [255, 220, 0]][(y < 12 ? 0 : 2) + (x < 20 ? 0 : 1)];
  return [...q, 255];
});
writeFileSync(join(work, 'scene.ppm'), ppm(scene));
writeFileSync(join(work, 'quad.ppm'), ppm(quad));
writeFileSync(join(work, 'scene.pam'), pam({...scene, data: scene.data}));
writeFileSync(join(work, 'alpha.pam'), pam(alpha));
const frame2 = picture(64, 48, (x, y) => [x * 4, 255 - y * 5, 90, 255]);
writeFileSync(join(work, 'frame2.pam'), pam(frame2));

const sources = {};
const jpeg = (name, source, args) => {
  execFileSync(cjpeg, [...args, '-outfile', join(out, name), join(work, source)]);
  sources[name] = `cjpeg ${args.join(' ')} ${source}`;
};
jpeg('baseline-420.jpg', 'scene.ppm', ['-quality', '85']);
jpeg('baseline-422.jpg', 'scene.ppm', ['-quality', '85', '-sample', '2x1']);
jpeg('baseline-440.jpg', 'scene.ppm', ['-quality', '85', '-sample', '1x2']);
jpeg('baseline-444.jpg', 'scene.ppm', ['-quality', '85', '-sample', '1x1']);
jpeg('low-quality.jpg', 'scene.ppm', ['-quality', '20']);
jpeg('grayscale.jpg', 'scene.ppm', ['-grayscale']);
jpeg('rgb.jpg', 'scene.ppm', ['-rgb']);
jpeg('progressive.jpg', 'scene.ppm', ['-progressive']);
jpeg('restart.jpg', 'scene.ppm', ['-restart', '1']);
jpeg('arithmetic.jpg', 'scene.ppm', ['-arithmetic']);
jpeg('lossless.jpg', 'scene.ppm', ['-lossless', '1']);
jpeg('quad.jpg', 'quad.ppm', ['-quality', '95', '-sample', '1x1']);

// Exif orientations 1 to 8: quad.jpg with an APP1 segment of one IFD entry (little-endian TIFF) after its SOI.
const plain = readFileSync(join(out, 'quad.jpg'));
for (let o = 1; o <= 8; ++o) {
  const tiff = Buffer.alloc(26);
  tiff.write('II', 0, 'ascii');
  tiff.writeUInt16LE(42, 2);
  tiff.writeUInt32LE(8, 4);
  tiff.writeUInt16LE(1, 8);
  tiff.writeUInt16LE(0x0112, 10);
  tiff.writeUInt16LE(3, 12);
  tiff.writeUInt32LE(1, 14);
  tiff.writeUInt16LE(o, 18);
  tiff.writeUInt32LE(0, 22);
  const payload = Buffer.concat([Buffer.from('Exif\0\0', 'binary'), tiff]);
  const segment = Buffer.alloc(4);
  segment.writeUInt16BE(0xffe1, 0);
  segment.writeUInt16BE(payload.length + 2, 2);
  const name = `orientation-${o}.jpg`;
  writeFileSync(join(out, name), Buffer.concat([plain.subarray(0, 2), segment, payload, plain.subarray(2)]));
  sources[name] = `quad.jpg with Exif orientation ${o}`;
}

for (const name of ['testorig.jpg', 'testimgint.jpg', 'testimgari.jpg', 'testorig12.jpg']) {
  copyFileSync(resolve(repo, 'third_party/libjpeg-turbo/source/testimages', name), join(out, 'upstream-' + name));
  sources['upstream-' + name] = `libjpeg-turbo testimages/${name}`;
}

const webp = (name, source, args) => {
  execFileSync(cwebp, ['-quiet', ...args, join(work, source), '-o', join(out, name)]);
  sources[name] = `cwebp ${args.join(' ')} ${source}`;
};
webp('lossy.webp', 'scene.pam', ['-q', '80']);
webp('lossy-alpha.webp', 'alpha.pam', ['-q', '80']);
webp('lossy-alpha-lossy.webp', 'alpha.pam', ['-q', '80', '-alpha_q', '50']);
webp('lossless.webp', 'scene.pam', ['-lossless']);
webp('lossless-alpha.webp', 'alpha.pam', ['-lossless', '-exact']);
execFileSync(img2webp, ['-lossless', join(work, 'alpha.pam'), '-d', '100', join(work, 'frame2.pam'), '-o',
  join(out, 'animated.webp')]);
sources['animated.webp'] = 'img2webp -lossless alpha.pam frame2.pam (two frames)';
copyFileSync(resolve(repo, 'third_party/libwebp/source/examples/test.webp'), join(out, 'upstream-test.webp'));
sources['upstream-test.webp'] = 'libwebp examples/test.webp';

writeFileSync(join(out, 'sources.json'), JSON.stringify(sources, null, 1) + '\n');
rmSync(work, {recursive: true, force: true});
console.log(`Wrote ${Object.keys(sources).length} images to ${out}`);
