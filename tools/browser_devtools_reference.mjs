// Chrome oracle for the developer tools' CSS and DOM answers (docs/devtools.md): what Chrome's own CSS and DOM domains say
// of tests/fixtures/devtools/page.css and page.js: for each element, the author rules that match, lowest in the cascade
// first, each with the line it starts at and whether its declarations parse, and the element's box model. Writes
// tests/fixtures/devtools/chrome-results.json, which tests/devtools_protocol_tests.cpp compares a View's answers with.
//
//   node tools/browser_devtools_reference.mjs [output.json] [chrome executable]
import {spawn} from 'node:child_process';
import {existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {fileURLToPath, pathToFileURL} from 'node:url';

const repo = resolve(fileURLToPath(new URL('..', import.meta.url)));
const directory = resolve(repo, 'tests/fixtures/devtools');
const output = resolve(process.argv[2] ?? resolve(directory, 'chrome-results.json'));
const defaults = ['C:/Program Files/Google/Chrome/Application/chrome.exe',
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'];
const executable = process.argv[3] ?? defaults.find(existsSync);
if (!executable || !existsSync(executable)) {
  throw new Error('Provide a local Chromium executable as the second argument');
}

const profile = mkdtempSync(join(tmpdir(), 'weft-devtools-reference-'));
const html = join(profile, 'page.html');
writeFileSync(html, '<!doctype html><html><head><meta charset=utf-8><style>html,body{margin:0}</style>' +
  `<link rel=stylesheet href="${pathToFileURL(join(directory, 'page.css'))}"></head>` +
  `<body><div id=root></div><script src="${pathToFileURL(join(directory, 'page.js'))}"></script></body></html>`);
const child = spawn(executable, ['--headless=new', '--no-first-run', '--disable-background-networking',
  '--remote-debugging-port=0', '--force-device-scale-factor=1', '--window-size=800,600', `--user-data-dir=${profile}`,
  'about:blank'], {windowsHide: true, stdio: ['ignore', 'ignore', 'pipe']});
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
try {
  const port = await new Promise((resolvePort, reject) => {
    let text = '';
    const limit = setTimeout(() => reject(new Error('Chromium startup timeout')), 20000);
    child.stderr.on('data', (c) => {
      text += c;
      const m = text.match(/DevTools listening on ws:\/\/127\.0\.0\.1:(\d+)\//);
      if (m) {
        clearTimeout(limit);
        resolvePort(m[1]);
      }
    });
  });
  const version = await (await fetch(`http://127.0.0.1:${port}/json/version`)).json();
  const target = (await (await fetch(`http://127.0.0.1:${port}/json/list`)).json()).find((t) => t.type === 'page');
  const socket = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((r) => socket.addEventListener('open', r));
  let nextId = 1;
  const waiting = new Map();
  socket.addEventListener('message', (e) => {
    const m = JSON.parse(e.data);
    if (m.id && waiting.has(m.id)) {
      const {resolve: done, reject} = waiting.get(m.id);
      waiting.delete(m.id);
      m.error ? reject(new Error(JSON.stringify(m.error))) : done(m.result);
    }
  });
  const call = (method, params = {}) => new Promise((done, reject) => {
    const id = nextId++;
    waiting.set(id, {resolve: done, reject});
    socket.send(JSON.stringify({id, method, params}));
  });

  await call('Page.enable');
  await call('Page.navigate', {url: pathToFileURL(html).href});
  await sleep(1500);
  await call('DOM.enable');
  await call('CSS.enable');
  const {root} = await call('DOM.getDocument', {depth: -1});
  const {nodeIds} = await call('DOM.querySelectorAll', {nodeId: root.nodeId, selector: '[data-check]'});
  const elements = [];
  for (const nodeId of nodeIds) {
    const {attributes} = await call('DOM.getAttributes', {nodeId});
    const id = attributes[attributes.indexOf('id') + 1];
    const matched = await call('CSS.getMatchedStylesForNode', {nodeId});
    const {model} = await call('DOM.getBoxModel', {nodeId});
    elements.push({
      id,
      rules: matched.matchedCSSRules.filter((r) => r.rule.origin === 'regular').map((r) => ({
        line: r.rule.selectorList.selectors[r.matchingSelectors[0]].range.startLine,
        matching: r.matchingSelectors.length,
        declarations: r.rule.style.cssProperties.filter((p) => p.range).map((p) => ({
          name: p.name, value: p.value, important: Boolean(p.important), line: p.range.startLine,
          column: p.range.startColumn, end: p.range.endColumn, parsedOk: p.parsedOk !== false,
        })),
      })),
      border: model.border, content: model.content, margin: model.margin,
    });
  }
  writeFileSync(output, JSON.stringify({chrome: version.Browser, elements}, null, 1) + '\n');
  console.log(`${version.Browser}: ${elements.length} elements -> ${output}`);
  socket.close();
} finally {
  child.kill();
  await sleep(300);
  rmSync(profile, {recursive: true, force: true});
}
