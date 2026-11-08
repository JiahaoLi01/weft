// Chrome oracle for text field editing (src/view/view.cpp's field keys, src/script/script_dom.cpp's edits): copy, cut,
// paste, undo and redo in the fields of tests/fixtures/field_editing/editing.js, by the steps of steps.json, in headless
// Chrome (whose clipboard is its own, in memory). Each step:
//   ["focus", id]            the field takes the focus;
//   ["select", start, end]   setSelectionRange;
//   ["type", text]           a key typed for each character;
//   ["key", name]            Backspace, Delete, ArrowLeft, End;
//   ["command", name]        copy, cut, paste, undo, redo, selectAll: the editing command a shortcut runs;
//   ["script", source]       script runs in the page;
//   ["state", label]         the page logs every field's value and selection.
// The page logs every clipboard and input event at the fields (window.__log). Writes
// tests/fixtures/field_editing/chrome-results.json, which tests/field_editing_tests.cpp compares a View's log with.
//
//   node tools/browser_field_editing_reference.mjs [output.json] [chrome executable]
import {spawn} from 'node:child_process';
import {existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

const repo = resolve(fileURLToPath(new URL('..', import.meta.url)));
const directory = resolve(repo, 'tests/fixtures/field_editing');
const output = resolve(process.argv[2] ?? resolve(directory, 'chrome-results.json'));
const defaults = ['C:/Program Files/Google/Chrome/Application/chrome.exe',
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'];
const executable = process.argv[3] ?? defaults.find(existsSync);
if (!executable || !existsSync(executable)) throw new Error('Provide a local Chromium executable as the second argument');

const steps = JSON.parse(readFileSync(resolve(directory, 'steps.json'), 'utf8'));
const page = '<!doctype html><html><head><meta charset=utf-8><style>input,textarea{display:block;width:300px;' +
  'font:16px sans-serif;outline:none}</style></head><body><div id=root></div><script>' +
  readFileSync(resolve(directory, 'editing.js'), 'utf8') + '</script></body></html>';

// The key events of a key, as a keyboard sends them: the command Chrome's editing behaviour runs for it.
const keys = {
  Backspace: {code: 'Backspace', keyCode: 8, command: 'deleteBackward'},
  Delete: {code: 'Delete', keyCode: 46, command: 'deleteForward'},
  ArrowLeft: {code: 'ArrowLeft', keyCode: 37, command: 'moveLeft'},
  End: {code: 'End', keyCode: 35, command: 'moveToEndOfLine'},
};
// The shortcut of each command (Control, as on Windows; the command is what matters).
const shortcuts = {
  copy: {key: 'c', code: 'KeyC', keyCode: 67, modifiers: 2},
  cut: {key: 'x', code: 'KeyX', keyCode: 88, modifiers: 2},
  paste: {key: 'v', code: 'KeyV', keyCode: 86, modifiers: 2},
  undo: {key: 'z', code: 'KeyZ', keyCode: 90, modifiers: 2},
  redo: {key: 'z', code: 'KeyZ', keyCode: 90, modifiers: 10},
  selectAll: {key: 'a', code: 'KeyA', keyCode: 65, modifiers: 2},
};

const profile = mkdtempSync(join(tmpdir(), 'weft-field-editing-'));
const child = spawn(executable, ['--headless=new', '--no-first-run', '--disable-background-networking',
  '--remote-debugging-port=0', '--force-device-scale-factor=1', '--window-size=800,600', '--user-data-dir=' + profile,
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
  await call('Emulation.setFocusEmulationEnabled', {enabled: true});
  const tree = await call('Page.getFrameTree');
  await call('Page.setDocumentContent', {frameId: tree.frameTree.frame.id, html: page});
  const press = async (event, command) => {
    await call('Input.dispatchKeyEvent', {type: 'rawKeyDown', key: event.key, code: event.code,
      windowsVirtualKeyCode: event.keyCode, modifiers: event.modifiers ?? 0, commands: [command]});
    await call('Input.dispatchKeyEvent', {type: 'keyUp', key: event.key, code: event.code,
      windowsVirtualKeyCode: event.keyCode, modifiers: event.modifiers ?? 0});
  };
  for (const [kind, ...args] of steps) {
    if (kind === 'focus') {
      await evaluate(`document.getElementById(${JSON.stringify(args[0])}).focus()`);
    } else if (kind === 'select') {
      await evaluate(`document.activeElement.setSelectionRange(${args[0]}, ${args[1]})`);
    } else if (kind === 'type') {
      for (const c of args[0]) {
        const code = c === ' ' ? 'Space' : /[a-z]/i.test(c) ? 'Key' + c.toUpperCase() : /[0-9]/.test(c) ? 'Digit' + c : '';
        await call('Input.dispatchKeyEvent', {type: 'keyDown', key: c, code, text: c, unmodifiedText: c});
        await call('Input.dispatchKeyEvent', {type: 'keyUp', key: c, code});
      }
    } else if (kind === 'key') {
      const k = keys[args[0]];
      await press({key: args[0], ...k}, k.command);
    } else if (kind === 'command') {
      await press(shortcuts[args[0]], args[0]);
    } else if (kind === 'script') {
      await evaluate(args[0]);
    } else if (kind === 'state') {
      await evaluate(`__state(${JSON.stringify(args[0])})`);
    } else {
      throw new Error('step ' + kind);
    }
  }
  const log = await evaluate('JSON.stringify(window.__log)');
  writeFileSync(output, JSON.stringify({browser: version.Browser, source: 'headless Chrome, its own clipboard',
    log: JSON.parse(log)}, null, 1) + '\n');
  console.log(`Recorded ${JSON.parse(log).length} log entries from ${version.Browser} to ${output}`);
} finally {
  socket?.close();
  const exited = new Promise((r) => child.once('exit', r));
  child.kill();
  await exited;
  rmSync(profile, {recursive: true, force: true, maxRetries: 5, retryDelay: 200});
}
