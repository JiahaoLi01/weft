// Opens Chrome's own DevTools front end on a Weft page and looks at what it made of it (docs/devtools.md).
//
//   node tools/devtools_frontend_check.mjs <host:port> <output directory> [panel] [selector]
//
// Starts headless Chrome, points its bundled front end at the first page the Weft dev build lists, waits for the panel,
// and writes <panel>.png and <panel>.json (the front end's own errors and what its panel shows as text). The Weft host
// must be running with WEFT_DEVTOOLS_LISTEN.
import { spawn } from 'node:child_process';
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const [address, out, panel = 'elements', selector = ''] = process.argv.slice(2);
if (!address || !out) {
  console.error('usage: devtools_frontend_check.mjs <host:port> <output directory> [panel] [selector]');
  process.exit(2);
}
mkdirSync(out, { recursive: true });

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const targets = await (await fetch(`http://${address}/json/list`)).json();
if (!targets.length) {
  console.error('the page lists no targets');
  process.exit(1);
}
const page = targets[0];
const socket = page.webSocketDebuggerUrl.replace('ws://', '');

const profile = mkdtempSync(join(tmpdir(), 'weft-devtools-'));
const chrome = spawn('/Applications/Google Chrome.app/Contents/MacOS/Google Chrome', [
  '--headless=new', '--no-first-run', '--disable-background-networking', '--remote-debugging-port=0',
  `--user-data-dir=${profile}`, '--window-size=1500,1000', 'about:blank',
], { stdio: 'ignore' });

let exitCode = 0;
try {
  const portFile = join(profile, 'DevToolsActivePort');
  for (let i = 0; i < 100 && !existsSync(portFile); ++i) {
    await sleep(100);
  }
  const port = readFileSync(portFile, 'utf8').split('\n')[0];
  const version = await (await fetch(`http://127.0.0.1:${port}/json/version`)).json();
  const browser = new WebSocket(version.webSocketDebuggerUrl);
  await new Promise((resolve) => browser.addEventListener('open', resolve));
  let nextId = 1;
  const waiting = new Map();
  const log = [];
  browser.addEventListener('message', (event) => {
    const message = JSON.parse(event.data);
    if (message.id && waiting.has(message.id)) {
      const { resolve, reject } = waiting.get(message.id);
      waiting.delete(message.id);
      message.error ? reject(new Error(JSON.stringify(message.error))) : resolve(message.result);
    } else if (message.method === 'Runtime.consoleAPICalled' && ['error', 'warning'].includes(message.params.type)) {
      log.push(`${message.params.type}: ${message.params.args.map((a) => a.value ?? a.description).join(' ')}`);
    } else if (message.method === 'Runtime.exceptionThrown') {
      log.push(`exception: ${message.params.exceptionDetails.exception?.description ?? message.params.exceptionDetails.text}`);
    }
  });
  const call = (method, params = {}, sessionId) => new Promise((resolve, reject) => {
    const id = nextId++;
    waiting.set(id, { resolve, reject });
    browser.send(JSON.stringify({ id, method, params, sessionId }));
  });

  const url = `devtools://devtools/bundled/devtools_app.html?ws=${socket}&panel=${panel}`;
  const { targetId } = await call('Target.createTarget', { url });
  const { sessionId } = await call('Target.attachToTarget', { targetId, flatten: true });
  await call('Runtime.enable', {}, sessionId);
  await call('Page.enable', {}, sessionId);
  await call('Emulation.setDeviceMetricsOverride', { width: 1500, height: 1000, deviceScaleFactor: 1, mobile: false }, sessionId);
  await sleep(6000);

  // The front end's light DOM goes through nested shadow roots.
  const deepText = `(() => { const walk = (n, out) => { if (n.nodeType === 3) { const t = n.textContent.trim(); if (t) out.push(t); }
    for (const c of n.childNodes || []) walk(c, out); if (n.shadowRoot) walk(n.shadowRoot, out); return out; };
    return walk(document.body, []).join('\\n'); })()`;
  let scriptResult;
  if (selector) {
    const outcome = await call('Runtime.evaluate', { expression: selector, awaitPromise: true, returnByValue: true }, sessionId);
    scriptResult = outcome.exceptionDetails ? outcome.exceptionDetails : outcome.result.value;
    await sleep(2500);
  }
  const text = await call('Runtime.evaluate', { expression: deepText, returnByValue: true }, sessionId);
  const shot = await call('Page.captureScreenshot', { format: 'png' }, sessionId);
  writeFileSync(join(out, `${panel}.png`), Buffer.from(shot.data, 'base64'));
  writeFileSync(join(out, `${panel}.json`), JSON.stringify({ log, scriptResult, text: text.result.value }, null, 1));
  console.log(`${panel}: ${text.result.value.split('\n').length} lines of text, ${log.length} front end errors`);
  for (const line of log.slice(0, 25)) {
    console.log(`  ${line.slice(0, 170)}`);
  }
  browser.close();
} catch (error) {
  console.error(error);
  exitCode = 1;
} finally {
  chrome.kill();
  await sleep(200);
  rmSync(profile, { recursive: true, force: true });
}
process.exit(exitCode);
