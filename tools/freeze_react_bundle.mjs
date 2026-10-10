// Run after `npm run build:native` in examples/react-vite. No network or lock refresh.
import { readFile, writeFile, copyFile, mkdir } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const digest = bytes => createHash('sha256').update(bytes).digest('hex');
const manifest = JSON.parse(await readFile(resolve(root, 'third_party/manifest.json'), 'utf8'));
const runtime = manifest.dependencies.find(d => d.name === 'react-runtime');
if (!runtime) throw new Error('Missing pinned React runtime');
// Compare every installed runtime byte against both the reviewed lock and source.
for (const [relative, expected] of Object.entries(runtime.files)) {
  const original = await readFile(resolve(root, runtime.source_dir, relative));
  if (digest(original) !== expected) throw new Error(`Pinned runtime changed: ${relative}`);
  if (/^(react|react-dom|scheduler)\//.test(relative)) {
    const installed = await readFile(resolve(root, 'examples/react-vite/node_modules', relative));
    if (!installed.equals(original)) throw new Error(`Installed runtime differs from pinned npm source: ${relative}`);
  }
}
const lock = JSON.parse(await readFile(resolve(root, 'examples/react-vite/package-lock.json'), 'utf8'));
for (const name of ['react', 'react-dom', 'scheduler', 'vite']) {
  const pkg = JSON.parse(await readFile(resolve(root, 'examples/react-vite/node_modules', name, 'package.json'), 'utf8'));
  if (pkg.version !== lock.packages[`node_modules/${name}`].version) throw new Error(`Installed version differs: ${name}`);
}
const out = resolve(root, 'examples/react-vite/native');
await mkdir(out, { recursive: true });
const bundle = await readFile(resolve(root, 'examples/react-vite/dist/native/app.js'));
if (bundle.includes(Buffer.from('__ceReactTrace'))) throw new Error('Debug instrumentation in bundle');
await writeFile(resolve(out, 'app.js'), bundle);
await copyFile(resolve(root, 'third_party/react-runtime/BUNDLED-LICENSES.txt'), resolve(out, 'BUNDLED-LICENSES.txt'));
const inputs = [
  'examples/react-vite/package.json', 'examples/react-vite/package-lock.json',
  'examples/react-vite/vite.config.js', 'examples/react-vite/scripts/prepare.mjs',
  'examples/react-vite/src/main.jsx', 'third_party/react-runtime/metadata.json',
  'tools/freeze_react_bundle.mjs',
];
const outputs = ['examples/react-vite/native/app.js', 'examples/react-vite/native/BUNDLED-LICENSES.txt'];
async function hashes(paths) {
  return Object.fromEntries(await Promise.all(paths.map(async path => [path, digest(await readFile(resolve(root, path)))])));
}
const meta = {
  react: lock.packages['node_modules/react'].version,
  react_dom: lock.packages['node_modules/react-dom'].version,
  scheduler: lock.packages['node_modules/scheduler'].version,
  vite: lock.packages['node_modules/vite'].version,
  node: process.versions.node,
  format: 'Vite library IIFE, production, ES2020, minify:false, no upstream source patches',
  inputs: await hashes(inputs), outputs: await hashes(outputs),
};
await writeFile(resolve(out, 'metadata.json'), JSON.stringify(meta, null, 2) + '\n');
console.log(`Frozen native React bundle ${bundle.length} bytes, SHA256 ${digest(bundle)}; browser reference must be captured independently after app changes.`);
