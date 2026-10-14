// The CRPG UI's floating UI page (13.4, docs/engine-communication.md), offline, as tools/build_tooltip_page.mjs builds
// the tooltip page: the unchanged harness (examples/crpg-gallery/src/harness.tsx) with one group, FLOATING_TESTS, of two
// cases over the captured, unmodified instance components (HealthBar, DamageNumber, AbnormalFlagFloating, QuestMarker),
// their type registry, palette, icons, labels and i18n:
//   0  the captured, unmodified FloatingUIOverlay and its store, which take the engine's whole batch every frame
//      (__floating_ui_batch through window.__ue_dispatch, the CRPG bridge);
//   1  examples/crpg-gallery/src/floating/AnchoredFloatingUIOverlay.tsx: the same components in the same wrapper,
//      which follows the engine's anchors, with instances coming and going by events and their fields by a view model.
// The settings store is the page's own (src/floating/settings-store.ts): the overlay reads only its UI scale, and the
// original store reads localStorage, which the page's host does not have. Zustand is the pinned third_party/zustand. No
// original file is changed.
//
//   node tools/build_floating_page.mjs
import {readFileSync, writeFileSync, existsSync} from 'node:fs';
import {resolve, relative} from 'node:path';
import {createRequire} from 'node:module';
import {repo, runtime, sha, verifyRuntime, restoreRuntimeLinks} from './gallery_runtime.mjs';

verifyRuntime();
restoreRuntimeLinks();
const require = createRequire(resolve(runtime, 'package.json'));
const esbuild = require('esbuild');
const fixture = resolve(repo, 'tests/fixtures/crpg-gallery/UI/src');
const page = resolve(repo, 'examples/crpg-gallery/src/floating');
const entry = resolve(repo, 'examples/crpg-gallery/src/harness.tsx');
const zustand = resolve(repo, 'third_party/zustand/source/esm/index.mjs');
const out = resolve(repo, 'examples/crpg-gallery/native');
const labels = ['CRPG: Floating UI (JSON batches)', 'CRPG: Floating UI (anchors and events)'];
const inputs = {};
const name = (p) => relative(repo, p).replaceAll('\\', '/');
const remember = (p) => inputs[name(p)] = sha(readFileSync(p));
for (const p of [entry, resolve(repo, 'tools/build_floating_page.mjs'), resolve(repo, 'tools/gallery_runtime.mjs'),
  resolve(runtime, 'manifest.json')]) {
  remember(p);
}
// A captured source by its @/ path: a module of the fixture, .ts or .tsx, or a folder's index.
const captured = (path) => {
  for (const suffix of ['.ts', '.tsx', '/index.ts', '/index.tsx']) {
    const file = resolve(fixture, path + suffix);
    if (existsSync(file)) {
      return file;
    }
  }
  throw new Error('no captured source for @/' + path);
};
const result = await esbuild.build({entryPoints: [entry], absWorkingDir: repo, bundle: true, write: false,
  format: 'iife', platform: 'browser', target: 'es2020', jsx: 'automatic', nodePaths: [resolve(runtime, 'node_modules')],
  define: {'process.env.NODE_ENV': '"production"'}, metafile: true, legalComments: 'inline', minify: false,
  plugins: [{name: 'floating-page', setup(build) {
    // The harness's registry: the two overlays as the cases of FLOATING_TESTS.
    build.onResolve({filter: /^ce-gallery-original$/}, () => ({path: 'floating-page', namespace: 'floating-page'}));
    build.onLoad({filter: /.*/, namespace: 'floating-page'}, () => ({
      contents: `import {createElement} from 'react';\n` +
        `import {FloatingUIOverlay} from '@/components/FloatingUIOverlay';\n` +
        `import {AnchoredFloatingUIOverlay} from './AnchoredFloatingUIOverlay';\n` +
        `export const __ceGalleryGroups={FLOATING_TESTS:[` +
        `{label:${JSON.stringify(labels[0])},render:()=>createElement(FloatingUIOverlay)},` +
        `{label:${JSON.stringify(labels[1])},render:()=>createElement(AnchoredFloatingUIOverlay)}]};\n` +
        `export const __ceOriginalGallery=()=>null;\n`,
      loader: 'tsx', resolveDir: page}));
    build.onResolve({filter: /^@\/stores\/settings-store$/}, () => ({path: resolve(page, 'settings-store.ts')}));
    build.onResolve({filter: /^@\//}, (args) => ({path: captured(args.path.slice(2))}));
    build.onResolve({filter: /^zustand$/}, () => ({path: zustand}));
  }}]});
for (const input of Object.keys(result.metafile.inputs)) {
  if (!input.startsWith('floating-page:')) {
    remember(resolve(repo, input));
  }
}
const bytes = result.outputFiles[0].contents;
writeFileSync(resolve(out, 'FLOATING_TESTS.js'), bytes);
writeFileSync(resolve(out, 'floating-metadata.json'), JSON.stringify({format: 'gallery-floating-bundle-v1',
  node: process.version, esbuild: esbuild.version, group: 'FLOATING_TESTS', labels,
  options: {target: 'es2020', format: 'iife', production: true,
    original_components: 'unchanged FloatingUIOverlay, floating-ui store, instance components, registry and configs',
    page: {anchored_overlay: 'src/floating/AnchoredFloatingUIOverlay.tsx', settings_store: 'src/floating/settings-store.ts'},
    zustand: 'third_party/zustand 5.0.15'},
  inputs, outputs: {[name(resolve(out, 'FLOATING_TESTS.js'))]: sha(bytes)}}, null, 2) + '\n');
console.log(`Built the floating UI page from ${Object.keys(inputs).length} inputs`);
