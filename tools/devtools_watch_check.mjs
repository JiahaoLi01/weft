// Hot reload end to end (docs/devtools.md, "热重载"): a dev build's host, tools/devtools_watch.mjs on a page folder,
// and the folder's files edited as a developer would.
//
//   node tools/devtools_watch_check.mjs <weft_devtools_host executable> [port]
//
// The host is tests/devtools_host.c of a dev build: no window, no renderer, and it goes from one page to another when
// told, as a game goes from screen to screen. It has its own copies of two pages (what a game ships: the page of
// tests/fixtures/devtools, and another); the watcher has the developer's copy of the first. Checked:
//   - a sheet saved (the styles change, the scripts' state stays), a script saved (the page loads again), a script that
//     does not parse (the page showing stays, the reason is printed), the script mended, an image added;
//   - the game showing its other page: what is saved meanwhile is kept and does not touch that page, and is there when
//     the game shows the folder's page again; a page kept that does not load leaves the game its own;
//   - a watcher started while the game shows the other page waits for the folder's.
// Exits 1 at the first check that fails; prints what the watchers printed.
import {spawn} from 'node:child_process';
import {appendFileSync, cpSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

const repo = resolve(fileURLToPath(new URL('..', import.meta.url)));
const [host, portArgument] = process.argv.slice(2);
if (!host) {
  console.error('usage: devtools_watch_check.mjs <weft_devtools_host executable> [port]');
  process.exit(2);
}
const port = portArgument ?? '9331';
const scratch = mkdtempSync(join(tmpdir(), 'weft-watch-check-'));
const fixture = join(repo, 'tests/fixtures/devtools');
// What the game ships, and the developer's folder.
const shipped = join(scratch, 'bundle/page');
const other = join(scratch, 'bundle/other');
const folder = join(scratch, 'dev');
for (const directory of [shipped, other, folder]) {
  mkdirSync(directory, {recursive: true});
}
for (const name of ['page.css', 'page.js']) {
  cpSync(join(fixture, name), join(shipped, name));
  cpSync(join(fixture, name), join(folder, name));
}
cpSync(join(fixture, 'page.json'), join(folder, 'page.json'));
const otherSheet = '#root { position: relative; }\n#only { width: 40px; height: 10px; }\n';
writeFileSync(join(other, 'other.css'), otherSheet);
writeFileSync(join(other, 'other.js'), "window.which = 'other';\nvar only = document.createElement('div');\n" +
  "only.id = 'only';\ndocument.getElementById('root').appendChild(only);\n");
const control = join(scratch, 'control');
writeFileSync(control, '0');

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const check = (condition, what) => {
  if (!condition) {
    throw new Error(`FAILED: ${what}`);
  }
  console.log(`ok: ${what}`);
};

const game = spawn(host, [control, `${join(shipped, 'page.css')},${join(shipped, 'page.js')}`,
  `${join(other, 'other.css')},${join(other, 'other.js')}`],
{env: {...process.env, WEFT_DEVTOOLS_LISTEN: port}, stdio: 'ignore'});
// A watcher, and what it printed: `said` waits for a line it prints after the last one waited for.
const watch = () => {
  const out = {log: '', at: 0};
  out.process = spawn('node', [join(repo, 'tools/devtools_watch.mjs'), `127.0.0.1:${port}`, folder]);
  out.process.stdout.on('data', (c) => {
    out.log += c;
  });
  out.process.stderr.on('data', (c) => {
    out.log += c;
  });
  out.said = async (text, ms = 8000) => {
    for (let waited = 0; waited < ms; waited += 50) {
      const found = out.log.indexOf(text, out.at);
      if (found >= 0) {
        out.at = found + text.length;
        return true;
      }
      await sleep(50);
    }
    return false;
  };
  return out;
};
const watchers = [];
let failed = false;
try {
  for (let i = 0; i < 50; ++i) {
    try {
      if ((await (await fetch(`http://127.0.0.1:${port}/json/list`)).json()).length) {
        break;
      }
    } catch {
      // Not listening yet.
    }
    await sleep(100);
  }
  let watcher = watch();
  watchers.push(watcher);
  check(await watcher.said('the page is loaded'), 'the watcher pushes the folder\'s page');

  const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
  const socket = new WebSocket(targets[0].webSocketDebuggerUrl);
  await new Promise((r) => socket.addEventListener('open', r));
  let next = 1;
  const waiting = new Map();
  socket.addEventListener('message', (e) => {
    const m = JSON.parse(e.data);
    if (m.id && waiting.has(m.id)) {
      waiting.get(m.id)(m);
      waiting.delete(m.id);
    }
  });
  const call = (method, params = {}) => new Promise((r) => {
    const id = next++;
    waiting.set(id, r);
    socket.send(JSON.stringify({id, method, params}));
  });
  const ev = async (expression) => (await call('Runtime.evaluate', {expression})).result?.result?.value;
  const until = async (expression, value, ms = 8000) => {
    for (let waited = 0; waited < ms; waited += 60) {
      if (await ev(expression) === value) {
        return true;
      }
      await sleep(60);
    }
    return false;
  };
  const width = "document.getElementById('five').getBoundingClientRect().width";
  const has = (id) => `!!document.getElementById('${id}')`;
  const sheet = join(folder, 'page.css');
  const script = join(folder, 'page.js');
  const show = (page) => writeFileSync(control, String(page));
  // What the watcher prints of a change the game keeps for the folder's page.
  const keptFor = (file) => `${file}: kept for this folder's page`;

  // The sheet: the page's state stays.
  await ev('window.kept = 5');
  check(await ev(width) === 80, 'the page as the folder has it');
  writeFileSync(sheet, readFileSync(sheet, 'utf8').replace('width: 80px;', 'width: 123px;'));
  check(await until(width, 123), 'a saved sheet replaces the styles');
  check(await ev('window.kept') === 5, 'and the scripts\' state is as it was');

  // A script: the page again.
  appendFileSync(script, "\nadd(root, 'seven', 'box', '', 'seven');\n");
  check(await until(has('seven'), true), 'a saved script loads the page again');
  check(await ev('typeof window.kept') === 'undefined' && await ev(width) === 123, 'anew, with the folder\'s sheet');

  // A script that does not parse: the page showing stays.
  await ev('window.kept = 6');
  let good = readFileSync(script, 'utf8');
  writeFileSync(script, good + '\n}}} not a script\n');
  check(await watcher.said('does NOT load'), 'a script that does not parse is reported');
  check(await ev('window.kept') === 6 && await ev(has('seven')) === true, 'and the page showing stays as it was');
  good += "\nadd(root, 'eight', 'box', '', 'eight');\n";
  writeFileSync(script, good);
  check(await until(has('eight'), true), 'the script mended loads');

  // An image the folder gets, and a sheet that names it.
  const report = async () => (await call('Weft.getCompatibilityReport')).result.runtime.join('\n');
  appendFileSync(sheet, '\n.multi { background-image: url(dot.png); }\n');
  check(await watcher.said('the styles are replaced'), 'the sheet that names an image is taken');
  check((await report()).includes('dot.png): no such file'), 'a file neither the game nor the folder has is a diagnostic');
  await ev('window.kept = 8');
  writeFileSync(join(folder, 'dot.png'), Buffer.from(
    'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==', 'base64'));
  check(await watcher.said('files read again'), 'a new image is sent');
  await sleep(300);
  check(await ev('window.kept') === 8, 'and the page is not loaded again for it');

  // The game goes to its other page: what is saved in the folder meanwhile is kept, and that page is left alone.
  show(1);
  check(await until('window.which', 'other'), 'the game shows its other page');
  check(await watcher.said('the game shows another page (other.css)'), 'the watcher says so');
  await ev('window.kept = 21');
  writeFileSync(sheet, readFileSync(sheet, 'utf8').replace('width: 123px;', 'width: 55px;'));
  check(await watcher.said(keptFor('page.css')), 'a sheet saved meanwhile is kept');
  good += "\nadd(root, 'nine', 'box', '', 'nine');\n";
  writeFileSync(script, good);
  check(await watcher.said(keptFor('page.js')), 'and so is a script');
  check((await call('Weft.getPage')).result.stylesheet === otherSheet && await ev('window.kept') === 21,
    'the page showing has its own sheet and its state');
  show(0);
  check(await until(has('nine'), true) && await ev(width) === 55, 'back on the folder\'s page, what was kept is there');
  check(await watcher.said('the game shows this folder\'s page again'), 'and the watcher says so');

  // A script kept that does not parse: when the game comes back, its own page loads, and the reason is printed.
  show(1);
  check(await until('window.which', 'other'), 'the other page again');
  writeFileSync(script, good + '\n}}} not a script\n');
  check(await watcher.said(keptFor('page.js')), 'a script that does not parse is kept untried');
  show(0);
  check(await until(`${has('five')} && !${has('seven')}`, true), 'the game\'s own page loads in its place');
  check(await watcher.said('SyntaxError'), 'and the reason is printed');
  writeFileSync(script, good);
  check(await until(has('nine'), true), 'mended, the folder\'s page loads');

  // A watcher started while the game shows the other page waits for the folder's.
  watcher.process.kill();
  await sleep(400);
  show(1);
  check(await until('window.which', 'other'), 'the other page once more, without a watcher');
  watcher = watch();
  watchers.push(watcher);
  check(await watcher.said('waiting for it to show that page'), 'a watcher started now waits');
  await sleep(500);
  check(await ev('window.which') === 'other' && (await call('Weft.getPage')).result.supplied === false,
    'and leaves the page showing alone');
  show(0);
  check(await watcher.said('the page is loaded') && await until(has('nine'), true),
    'when the game shows the folder\'s page, the folder\'s is pushed');
  socket.close();
} catch (error) {
  console.error(error.message);
  failed = true;
} finally {
  for (const watcher of watchers) {
    watcher.process.kill();
  }
  writeFileSync(control, 'quit');
  await sleep(200);
  game.kill();
  rmSync(scratch, {recursive: true, force: true});
  watchers.forEach((watcher, i) => {
    console.log(`--- watcher ${i + 1} printed:`);
    console.log(watcher.log.trim());
  });
}
process.exit(failed ? 1 : 0);
