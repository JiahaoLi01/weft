// Shows a page folder on this machine in a running dev build, and keeps it current as its files change
// (docs/devtools.md, "热重载"). The game is not built, signed or installed again: the page and its files go over the
// developer tools' connection, to a game on this machine or on a device.
//
//   node tools/devtools_watch.mjs <host:port> <page folder> [--target <id>] [--keyframes <file>] [--any-page]
//                                 [--once]
//
// The page is the folder's page.json ({"scripts": [...], "stylesheet": "...", "keyframes": "...", "root": "...",
// "crpgBridge": false, "forgivingCss": false, "webPlatform": false, "spatialNavigation": false}, paths relative to the
// folder), as the sample hosts load one. Without a page.json it is the page the View has loaded: each of its scripts
// by its name in the folder, and its sheet by the name of the page's base (those the folder does not have stay the
// View's); its keyframes are --keyframes, or <sheet>-keyframes.css or keyframes.css beside the sheet.
//
// A changed sheet replaces the page's styles, the scripts' state as it was. A changed script loads the page again; a
// page that does not load leaves the one showing, and the reason is printed. A changed image or font is read again.
// The folder's page stands for one page of the host's while this runs: the page the game shows when this starts.
// The host loading that page again shows the folder's. A game goes from one page to another: while it shows another
// page, what changes in the folder is kept for the folder's page and does not touch the page showing; it is there when
// the game shows the folder's page again. If the game shows a page with another sheet's file name when this starts
// (it is on another screen), this waits for it to show the folder's; --any-page takes the page it shows.
import {existsSync, readFileSync, readdirSync, statSync} from 'node:fs';
import {basename, dirname, extname, join, relative, resolve, sep} from 'node:path';

const args = process.argv.slice(2);
const option = (name) => {
  const at = args.indexOf(name);
  return at < 0 ? undefined : args.splice(at, 2)[1];
};
const flag = (name) => {
  const at = args.indexOf(name);
  if (at >= 0) {
    args.splice(at, 1);
  }
  return at >= 0;
};
const wanted = option('--target');
const keyframesOption = option('--keyframes');
const once = flag('--once');
const anyPage = flag('--any-page');
const [address, folderArgument] = args;
if (!address || !folderArgument) {
  console.error('usage: devtools_watch.mjs <host:port> <page folder> [--target <id>] [--keyframes <file>] ' +
    '[--any-page] [--once]');
  process.exit(2);
}
const folder = resolve(folderArgument);
if (!existsSync(folder) || !statSync(folder).isDirectory()) {
  console.error(`${folder} is not a folder`);
  process.exit(2);
}

// The files a page names besides its scripts and sheets.
const resourceTypes = new Set(['.png', '.jpg', '.jpeg', '.webp', '.gif', '.svg', '.ttf', '.otf', '.ttc', '.woff',
  '.woff2']);
const skipped = new Set(['node_modules', '.git']);
// Resources go in messages of about this many bytes, and no more of them than this in all.
const chunkBytes = 4 << 20;
const totalBytes = 256 << 20;
const pollMs = 200;
const settleMs = 80;

const sleep = (ms) => new Promise((done) => setTimeout(done, ms));
const log = (text) => console.log(`[weft watch] ${text}`);
const modified = (file) => {
  try {
    return statSync(file).mtimeMs;
  } catch {
    return 0;
  }
};

const listResources = () => {
  const found = [];
  const visit = (directory) => {
    for (const entry of readdirSync(directory, {withFileTypes: true})) {
      if (entry.isDirectory()) {
        if (!skipped.has(entry.name)) {
          visit(join(directory, entry.name));
        }
      } else if (resourceTypes.has(extname(entry.name).toLowerCase())) {
        found.push(join(directory, entry.name));
      }
    }
  };
  visit(folder);
  return found.sort();
};

class Connection {
  constructor(socket) {
    this.socket = socket;
    this.next = 1;
    this.waiting = new Map();
    this.closed = new Promise((done) => socket.addEventListener('close', done));
    // What the page says while a push is under way waits for the push's answer; and what its sheet drops is said
    // once, not again at each push that drops the same.
    this.held = undefined;
    this.dropped = new Set();
    this.droppedBefore = new Set();
    // Called with what the game shows each time it loads a page (Weft.pageChanged).
    this.onPage = undefined;
    socket.addEventListener('message', (event) => {
      const message = JSON.parse(event.data);
      if (message.id && this.waiting.has(message.id)) {
        const {done, fail} = this.waiting.get(message.id);
        this.waiting.delete(message.id);
        message.error ? fail(new Error(message.error.message)) : done(message.result);
      } else if (message.method === 'Runtime.consoleAPICalled') {
        const text = message.params.args.map((a) => a.value ?? a.description).join(' ');
        this.say(`${message.params.type === 'log' ? '' : message.params.type + ': '}${text}`);
      } else if (message.method === 'Runtime.exceptionThrown') {
        this.say(message.params.exceptionDetails.text.trim());
      } else if (message.method === 'Log.entryAdded') {
        const entry = message.params.entry;
        const where = entry.lineNumber === undefined ? '' : ` (${basename(entry.url ?? '')}:${entry.lineNumber + 1})`;
        const line = `${entry.level}: ${entry.text}${where}`;
        this.dropped.add(line);
        if (!this.droppedBefore.has(line)) {
          this.say(line);
        }
      } else if (message.method === 'Weft.pageChanged') {
        this.onPage?.(message.params);
      }
    });
    socket.addEventListener('close', () => {
      for (const {fail} of this.waiting.values()) {
        fail(new Error('the connection closed'));
      }
      this.waiting.clear();
    });
  }

  say(line) {
    if (this.held) {
      this.held.push(line);
    } else {
      console.log(`[page] ${line}`);
    }
  }

  call(method, params = {}) {
    return new Promise((done, fail) => {
      const id = this.next++;
      this.waiting.set(id, {done, fail});
      this.socket.send(JSON.stringify({id, method, params}));
    });
  }

  // A call that replaces the page or its styles: what the page says meanwhile is printed after `report`, which gets
  // the call's result and prints it, without the line that only repeats `said`.
  async push(method, params, report) {
    this.held = [];
    this.droppedBefore = this.dropped;
    this.dropped = new Set();
    let result;
    try {
      result = await this.call(method, params);
    } finally {
      const held = this.held;
      this.held = undefined;
      const said = result === undefined ? undefined : report(result);
      if (said !== undefined || result === undefined) {
        // Nothing was replaced: what the sheet in place drops has been said.
        this.dropped = this.droppedBefore;
      }
      for (const line of held) {
        if (line !== said) {
          console.log(`[page] ${line}`);
        }
      }
    }
    return result;
  }
}

const connect = async () => {
  const targets = await (await fetch(`http://${address}/json/list`)).json();
  if (!targets.length) {
    throw new Error('no page is loaded');
  }
  const target = wanted ? targets.find((t) => t.id === wanted) : targets[0];
  if (!target) {
    throw new Error(`no target ${wanted}; there are ${targets.map((t) => t.id).join(', ')}`);
  }
  if (!wanted && targets.length > 1) {
    log(`${targets.length} pages are loaded; taking ${target.id} (${target.title}). --target picks another of ` +
      targets.map((t) => t.id).join(', '));
  }
  const socket = new WebSocket(`ws://${address}/devtools/page/${target.id}`);
  await new Promise((done, fail) => {
    socket.addEventListener('open', done);
    socket.addEventListener('error', () => fail(new Error('the connection was refused')));
  });
  return {connection: new Connection(socket), target};
};

// The page's files: what is read from the folder, and what stays the View's.
const describe = async (connection) => {
  const manifestFile = join(folder, 'page.json');
  const loaded = await connection.call('Weft.getPage');
  const page = {scripts: [], files: [manifestFile]};
  if (existsSync(manifestFile)) {
    const manifest = JSON.parse(readFileSync(manifestFile, 'utf8'));
    page.scriptFiles = (manifest.scripts ?? []).map((name) => ({name, file: join(folder, name)}));
    page.sheetFile = join(folder, manifest.stylesheet);
    page.keyframesFile = manifest.keyframes ? join(folder, manifest.keyframes) : undefined;
    page.options = {
      root: manifest.root ?? '#root',
      crpgBridge: Boolean(manifest.crpgBridge),
      forgivingCss: Boolean(manifest.forgivingCss),
      webPlatform: Boolean(manifest.webPlatform),
      spatialNavigation: Boolean(manifest.spatialNavigation),
    };
    page.keyframesDefault = '';
  } else {
    // The page the View has, from the files of it the folder has.
    page.scriptFiles = loaded.scripts.map((script) => {
      const file = join(folder, script.name);
      if (script.name && existsSync(file)) {
        return {name: script.name, file};
      }
      log(`the folder has no ${script.name || '(unnamed script)'}: the View's stays`);
      return {name: script.name, source: script.source};
    });
    const sheet = join(folder, basename(loaded.base));
    if (loaded.base && existsSync(sheet)) {
      page.sheetFile = sheet;
    } else {
      log(`the folder has no ${basename(loaded.base) || 'style sheet'}: the View's stays`);
      page.sheetSource = loaded.stylesheet;
    }
    const stem = page.sheetFile ? page.sheetFile.slice(0, -extname(page.sheetFile).length) : undefined;
    const candidates = [keyframesOption && resolve(keyframesOption), stem && `${stem}-keyframes.css`,
      join(page.sheetFile ? dirname(page.sheetFile) : folder, 'keyframes.css')];
    page.keyframesFile = candidates.find((file) => file && existsSync(file));
    page.keyframesDefault = loaded.keyframes;
    page.options = {};
  }
  if (keyframesOption) {
    page.keyframesFile = resolve(keyframesOption);
  }
  // A url() of the page resolves against its base: the sheet's place.
  page.base = page.sheetFile ? dirname(page.sheetFile) : folder;
  return page;
};

const readPage = (page) => ({
  ...page.options,
  scripts: page.scriptFiles.map((s) => ({name: s.name, source: s.file ? readFileSync(s.file, 'utf8') : s.source})),
  stylesheet: page.sheetFile ? readFileSync(page.sheetFile, 'utf8') : page.sheetSource,
  keyframes: page.keyframesFile ? readFileSync(page.keyframesFile, 'utf8') : page.keyframesDefault,
});

const pushResources = async (connection, page, files, apply) => {
  let sent = 0;
  let batch = [];
  let batchBytes = 0;
  let count = 0;
  let deferred = false;
  const flush = async (last) => {
    if (batch.length) {
      const answer = await connection.call('Weft.setResources', {resources: batch, apply: apply && last});
      deferred = Boolean(answer.deferred);
      batch = [];
      batchBytes = 0;
    }
  };
  for (const file of files) {
    let bytes;
    try {
      bytes = readFileSync(file);
    } catch {
      continue;
    }
    if (sent + bytes.length > totalBytes) {
      log(`more than ${totalBytes >> 20} MB of files: ${relative(folder, file)} and what follows it are left out`);
      break;
    }
    sent += bytes.length;
    count += 1;
    batch.push({path: relative(page.base, file).split(sep).join('/'), data: bytes.toString('base64')});
    batchBytes += bytes.length;
    if (batchBytes >= chunkBytes) {
      await flush(false);
    }
  }
  await flush(true);
  return {count, bytes: sent, deferred};
};

const size = (bytes) => bytes < 1024 ? `${bytes} B` : bytes < 1 << 20 ? `${(bytes / 1024).toFixed(1)} KB` :
  `${(bytes / (1 << 20)).toFixed(1)} MB`;

// What is said of a change the game keeps for the folder's page because it shows another page now.
const kept = 'kept for this folder\'s page, which the game does not show now';

const pushPage = async (connection, page, why) => {
  const started = Date.now();
  const result = await connection.push('Weft.setPage', {page: readPage(page)}, (answer) => {
    if (answer.loaded) {
      log(`${why}: the page is loaded (${Date.now() - started} ms)`);
      return undefined;
    }
    if (answer.deferred) {
      log(`${why}: ${kept}`);
      return undefined;
    }
    log(`${why}: the page does NOT load, the one showing stays: ${answer.error.trim()}`);
    return answer.error.trim();
  });
  return result.loaded || Boolean(result.deferred);
};

const session = async () => {
  const {connection, target} = await connect();
  let open = true;
  connection.closed.then(() => {
    open = false;
  });
  // What the page says from now on: not what it said before this started.
  await connection.call('Runtime.discardConsoleEntries');
  await connection.call('Runtime.enable');
  await connection.call('Log.enable');
  // What the game shows, now and each time it loads a page.
  let showing;
  try {
    showing = await connection.call('Weft.enable');
  } catch (error) {
    if (/wasn't found/.test(error.message)) {
      // An engine from before the tools knew which page a game shows.
      console.error('[weft watch] the game\'s dev build is older than this tool: build the game again');
      process.exit(1);
    }
    throw error;
  }
  let wake;
  let page = await describe(connection);
  // The folder's page stands for the page the game shows when it is pushed first: the page whose sheet has the
  // folder's sheet's file name, where both are known.
  const own = (base) => anyPage || !page.sheetFile || !base || basename(base) === basename(page.sheetFile);
  let pushedOnce = false;
  connection.onPage = (state) => {
    if (pushedOnce && state.suppliedShown !== showing.suppliedShown) {
      log(state.suppliedShown ? 'the game shows this folder\'s page again' :
        `the game shows another page (${basename(state.base) || 'unnamed'}): what changes here is kept for this ` +
        'folder\'s page');
    }
    showing = state;
    wake?.();
  };
  if (!own(showing.base)) {
    const now = `the game shows ${basename(showing.base)}, not this folder's ${basename(page.sheetFile)}`;
    if (once) {
      throw new Error(`${now} (--any-page: take the page it shows)`);
    }
    log(`${now}: waiting for it to show that page (--any-page: take the page it shows now)`);
    while (open && !own(showing.base)) {
      await new Promise((done) => {
        wake = done;
        connection.closed.then(done);
      });
    }
    if (!open) {
      return undefined;
    }
  }
  let resources = listResources();
  const pushed = await pushResources(connection, page, resources, false);
  const shown = relative(process.cwd(), folder);
  log(`${shown.startsWith('..') ? folder : shown || '.'} -> ${address} (${target.id}): ${page.scriptFiles.length} scripts, ` +
    `${page.sheetFile ? basename(page.sheetFile) : 'the View\'s sheet'}, ${pushed.count} files (${size(pushed.bytes)})`);
  const loaded = await pushPage(connection, page, 'the folder\'s page');
  pushedOnce = true;
  showing = {...showing, supplied: true, suppliedShown: true};
  if (once) {
    connection.socket.close();
    return loaded ? 0 : 1;
  }

  const tracked = () => [...page.files, ...page.scriptFiles.map((s) => s.file), page.sheetFile, page.keyframesFile,
    ...resources].filter(Boolean);
  let times = new Map(tracked().map((file) => [file, modified(file)]));
  let scans = 0;
  while (open) {
    await sleep(pollMs);
    // New and removed images and fonts are found every few polls.
    if (++scans % 5 === 0) {
      const now = listResources();
      for (const file of now) {
        if (!times.has(file)) {
          times.set(file, 0);
        }
      }
      resources = now;
    }
    const changed = tracked().filter((file) => modified(file) !== times.get(file));
    if (!changed.length) {
      continue;
    }
    // An editor may write a file in steps: wait until the files are still.
    let stamp = changed.map(modified).join();
    for (;;) {
      await sleep(settleMs);
      const again = changed.map(modified).join();
      if (again === stamp) {
        break;
      }
      stamp = again;
    }
    for (const file of changed) {
      times.set(file, modified(file));
    }
    const names = changed.map((file) => relative(folder, file)).join(', ');
    try {
      const scripts = new Set(page.scriptFiles.map((s) => s.file));
      const changedResources = changed.filter((file) => resources.includes(file) && existsSync(file));
      if (changed.includes(page.files[0])) {
        // The page's description: everything anew.
        page = await describe(connection);
        await pushResources(connection, page, resources, false);
        await pushPage(connection, page, names);
        times = new Map(tracked().map((file) => [file, modified(file)]));
      } else if (changed.some((file) => scripts.has(file))) {
        await pushResources(connection, page, changedResources, false);
        await pushPage(connection, page, names);
      } else if (changed.includes(page.sheetFile) || changed.includes(page.keyframesFile)) {
        await pushResources(connection, page, changedResources, false);
        const started = Date.now();
        const next = readPage(page);
        await connection.push('Weft.setStyleSheet', {text: next.stylesheet, keyframes: next.keyframes}, (answer) => {
          log(answer.applied ?
            `${names}: the styles are replaced, the page's state as it was (${Date.now() - started} ms)` :
            answer.deferred ? `${names}: ${kept}` :
              `${names}: the styles are NOT replaced, those in place stay: ${answer.error}`);
          return answer.error;
        });
      } else if (changedResources.length) {
        const sent = await pushResources(connection, page, changedResources, true);
        log(sent.deferred ? `${names}: ${kept}` : `${names}: ${sent.count} files read again (${size(sent.bytes)})`);
      }
    } catch (error) {
      if (open) {
        log(`${names}: ${error.message}`);
      }
    }
  }
  return undefined;
};

for (;;) {
  let code;
  try {
    code = await session();
  } catch (error) {
    if (once) {
      console.error(`[weft watch] ${error.message}`);
      process.exit(1);
    }
    code = undefined;
    if (!/fetch failed|refused|closed|no page/.test(String(error.message))) {
      log(error.message);
    }
  }
  if (code !== undefined) {
    process.exit(code);
  }
  log(`waiting for a page at ${address}`);
  for (;;) {
    await sleep(1000);
    try {
      if ((await (await fetch(`http://${address}/json/list`)).json()).length) {
        break;
      }
    } catch {
      // Not listening yet.
    }
  }
}
