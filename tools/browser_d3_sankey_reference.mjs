// Chrome oracle for d3-sankey (third_party/d3-sankey, over third_party/d3): runs tests/fixtures/d3_sankey_probe.js after
// the two unmodified UMDs in headless Chrome and records its report (the layout's numbers and the svg it drew) in
// tests/fixtures/d3-sankey-reference.json, with the hashes of the three scripts it ran, for weft_d3_sankey_tests to
// compare weft's run with. The svg also becomes the "sankey" case of tests/fixtures/scene-paint-reference/cases.json
// (placed at the case canvas's origin, at 2x), whose Chrome capture tools/browser_scene_paint_reference.mjs then records.
// Offline: the page is a local file with the scripts inline.
//
//   node tools/browser_d3_sankey_reference.mjs [chrome.exe]
import {spawnSync} from 'node:child_process';
import {readFileSync, writeFileSync, mkdtempSync, existsSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {resolve, join} from 'node:path';
import {pathToFileURL} from 'node:url';
import {repo, sha} from './gallery_runtime.mjs';

const executable = process.argv[2] ?? 'C:/Program Files/Google/Chrome/Application/chrome.exe';
if (!existsSync(executable)) throw new Error('Provide a local Chromium executable as the first argument');
const sources = {
  'third_party/d3/d3.js': readFileSync(resolve(repo, 'third_party/d3/d3.js'), 'utf8'),
  'third_party/d3-sankey/d3-sankey.js': readFileSync(resolve(repo, 'third_party/d3-sankey/d3-sankey.js'), 'utf8'),
  'tests/fixtures/d3_sankey_probe.js': readFileSync(resolve(repo, 'tests/fixtures/d3_sankey_probe.js'), 'utf8'),
};
// Inline scripts must not close their element early.
const inline = (text) => `<script>${text.replace(/<\/script/gi, '<\\/script')}</script>`;
const page = '<!doctype html><html><head><meta charset=utf-8></head><body><pre id=report></pre>' +
  Object.values(sources).map(inline).join('') +
  '<script>document.getElementById("report").textContent = JSON.stringify({browser: navigator.userAgent, ' +
  'report: globalThis.weftSankeyReport});</script></body></html>';
const folder = mkdtempSync(join(tmpdir(), 'weft-d3-sankey-'));
const file = join(folder, 'page.html');
writeFileSync(file, page);
const run = spawnSync(executable, ['--headless=new', '--no-first-run', '--disable-background-networking',
  '--user-data-dir=' + join(folder, 'profile'), '--dump-dom', pathToFileURL(file).href],
  {encoding: 'utf8', maxBuffer: 64 * 1024 * 1024, windowsHide: true, timeout: 60000});
if (run.status !== 0) throw new Error('Chromium failed: ' + run.stderr);
const found = run.stdout.match(/<pre id="report">([\s\S]*?)<\/pre>/);
if (!found || !found[1]) throw new Error('no report in the page: ' + run.stdout.slice(0, 400));
const unescape = (s) => s.replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&quot;/g, '"').replace(/&amp;/g, '&');
const {browser, report} = JSON.parse(unescape(found[1]));
const chrome = (browser.match(/Chrome\/[\d.]+/) ?? [browser])[0];

writeFileSync(resolve(repo, 'tests/fixtures/d3-sankey-reference.json'), JSON.stringify({
  format: 'weft-d3-sankey-reference-v1', browser: chrome,
  scripts: Object.fromEntries(Object.entries(sources).map(([path, text]) => [path, sha(text)])), report}, null, 1) + '\n');

// The paint case: the svg at the canvas's origin, as one line among the cases (replacing the last recording's).
const casesPath = resolve(repo, 'tests/fixtures/scene-paint-reference/cases.json');
const lines = readFileSync(casesPath, 'utf8').split('\n');
const svg = Object.assign({}, report.svg, {style: 'position:absolute;left:0;top:0'});
const line = '    ' + JSON.stringify({name: 'sankey', scale: 2, nodes: [svg]}).replace(/,"/g, ', "').replace(/":/g, '": ');
const at = lines.findIndex((l) => l.startsWith('    {"name": "sankey"'));
if (at >= 0) {
  lines[at] = line + (lines[at].trimEnd().endsWith(',') ? ',' : '');
} else {
  // The last case's line closes with "]}" (or a case of nested lines closes with "    ]}"); the new one follows it.
  const end = lines.findIndex((l) => l === '  ]');
  if (end < 0) throw new Error('no end of the cases');
  lines[end - 1] = lines[end - 1] + ',';
  lines.splice(end, 0, line);
}
writeFileSync(casesPath, lines.join('\n'));
console.log(`Recorded the d3-sankey fixture from ${chrome}: ${report.nodes.length} nodes, ${report.links.length} links`);
