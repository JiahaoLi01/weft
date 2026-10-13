// The CRPG UI's nested tooltip page for the gallery (11.3), offline, as tools/build_gallery.mjs builds the original
// cases: the unchanged harness (examples/crpg-gallery/src/harness.tsx) with one group, TOOLTIP_TESTS, whose one case
// renders the page (examples/crpg-gallery/src/tooltip/NestedTooltipDemo.tsx: the captured page without its smoke
// tuning panel) over the captured, unmodified NestedTooltip component. Two bundles differ only in the smoke:
//   TOOLTIP_TESTS.js      the crpg-smoke UI shader effect (src/tooltip/SmokeBg.tsx), for the native scenes;
//   TOOLTIP_REFERENCE.js  the original canvas smoke (the captured SmokeBg.tsx), for Chrome.
// The page's utilities are gallery.css's (its Tailwind candidates are every token of the captured sources). No
// original file is changed.
//
//   node tools/build_tooltip_page.mjs
import {readFileSync, writeFileSync} from 'node:fs';
import {resolve, relative} from 'node:path';
import {createRequire} from 'node:module';
import {repo, runtime, sha, verifyRuntime, restoreRuntimeLinks} from './gallery_runtime.mjs';

verifyRuntime();
restoreRuntimeLinks();
const require = createRequire(resolve(runtime, 'package.json'));
const esbuild = require('esbuild');
const components = resolve(repo, 'tests/fixtures/crpg-gallery/UI/src/components');
const page = resolve(repo, 'examples/crpg-gallery/src/tooltip');
const entry = resolve(repo, 'examples/crpg-gallery/src/harness.tsx');
const out = resolve(repo, 'examples/crpg-gallery/native');
const label = 'CRPG: Nested Tooltip + Smoke';
const inputs = {};
const outputs = {};
const name = (p) => relative(repo, p).replaceAll('\\', '/');
const remember = (p) => inputs[name(p)] = sha(readFileSync(p));
const emit = (file, bytes) => {
  writeFileSync(resolve(out, file), bytes);
  outputs[name(resolve(out, file))] = sha(bytes);
};
for (const p of [entry, resolve(repo, 'tools/build_tooltip_page.mjs'), resolve(repo, 'tools/gallery_runtime.mjs'),
  resolve(runtime, 'manifest.json')]) {
  remember(p);
}
for (const [bundle, smoke] of [['TOOLTIP_TESTS', resolve(page, 'SmokeBg.tsx')],
  ['TOOLTIP_REFERENCE', resolve(components, 'SmokeBg.tsx')]]) {
  const result = await esbuild.build({entryPoints: [entry], absWorkingDir: repo, bundle: true, write: false,
    format: 'iife', platform: 'browser', target: 'es2020', jsx: 'automatic', nodePaths: [resolve(runtime, 'node_modules')],
    define: {'process.env.NODE_ENV': '"production"'}, metafile: true, legalComments: 'inline', minify: false,
    plugins: [{name: 'tooltip-page', setup(build) {
      // The harness's registry: the page as the one case of TOOLTIP_TESTS.
      build.onResolve({filter: /^ce-gallery-original$/}, () => ({path: 'tooltip-page', namespace: 'tooltip-page'}));
      build.onLoad({filter: /.*/, namespace: 'tooltip-page'}, () => ({
        contents: `import {createElement} from 'react';\nimport {NestedTooltipDemo} from './NestedTooltipDemo';\n` +
          `export const __ceGalleryGroups={TOOLTIP_TESTS:[{label:${JSON.stringify(label)},` +
          `render:()=>createElement(NestedTooltipDemo)}]};\nexport const __ceOriginalGallery=()=>null;\n`,
        loader: 'tsx', resolveDir: page}));
      // The captured component, unmodified, and the smoke this bundle draws, for every importer.
      build.onResolve({filter: /^\.\/NestedTooltip$/}, () => ({path: resolve(components, 'NestedTooltip.tsx')}));
      build.onResolve({filter: /^\.\/SmokeBg$/}, () => ({path: smoke}));
    }}]});
  for (const input of Object.keys(result.metafile.inputs)) {
    if (!input.startsWith('tooltip-page:')) {
      remember(resolve(repo, input));
    }
  }
  emit(bundle + '.js', result.outputFiles[0].contents);
}
writeFileSync(resolve(out, 'tooltip-metadata.json'), JSON.stringify({format: 'gallery-tooltip-bundle-v1',
  node: process.version, esbuild: esbuild.version, group: 'TOOLTIP_TESTS', label,
  options: {target: 'es2020', format: 'iife', production: true, original_component: 'unchanged NestedTooltip.tsx',
    page: 'NestedTooltipDemo.tsx without the smoke tuning panel', smoke: {TOOLTIP_TESTS: 'crpg-smoke effect',
      TOOLTIP_REFERENCE: 'original canvas'}},
  inputs, outputs}, null, 2) + '\n');
console.log(`Built the nested tooltip page: ${Object.keys(outputs).length} bundles from ${Object.keys(inputs).length} inputs`);
