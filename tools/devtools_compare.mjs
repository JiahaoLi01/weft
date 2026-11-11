// Lays the page a running dev build shows out in Chrome too, and lists the elements whose boxes differ
// (docs/devtools.md, "和 Chrome 对比").
//
//   node tools/devtools_compare.mjs <host:port> [--target <id>] [--folder <page folder>] [--tolerance <px>]
//                                   [--out <directory>] [--chrome <executable>] [--chrome-defaults] [--limit <n>]
//
// The document compared is the one the View has now: its elements, attributes and text as the page's scripts left
// them, the values of its text fields, its scroll positions and its hover, active and focus states, with the sheet it
// has now. Chrome gets that document built node by node (no markup is parsed, no script of the page runs) in a
// viewport of the View's size and scale, and each element's getBoundingClientRect() is set against the box Weft gave
// it. What moves in either (script animations, CSS animations and transitions under way in Chrome) is left out.
//
// As the project's Chrome references do (tools/browser_*_reference.mjs), Chrome is given what the scene's user agent
// sheet has beyond Chrome's own: * { box-sizing: border-box; border-style: solid; border-width: 0 }, in a cascade layer
// below the page's rules (--chrome-defaults leaves it out, to see the page as Chrome alone lays it out). And the
// scene's root is in Chrome what it is in a View: a box of the viewport's size that its children's margins do not
// collapse through and that absolutely positioned boxes are placed in.
//
// A box that differs is listed when some of the difference is its own: not what moving with its parent, being pushed
// by the sibling before it or stretching with its parent explains. The others are counted. Siblings of one kind that
// differ the same way are listed once.
//
// The page's url() and src files are read from the folder of the page's base when that is a folder of this machine,
// else from --folder. --out keeps chrome.html (the same document, to open in Chrome and inspect beside the game's)
// and report.json. Exits 1 when a box differs.
import {spawn} from 'node:child_process';
import {existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {dirname, join, resolve} from 'node:path';
import {pathToFileURL} from 'node:url';

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
const folderOption = option('--folder');
const tolerance = Number(option('--tolerance') ?? 0.5);
const out = option('--out');
const limit = Number(option('--limit') ?? 40);
const chromeDefaults = flag('--chrome-defaults');
const defaults = ['C:/Program Files/Google/Chrome/Application/chrome.exe',
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'];
const executable = option('--chrome') ?? defaults.find(existsSync);
const [address] = args;
if (!address || !Number.isFinite(tolerance)) {
  console.error('usage: devtools_compare.mjs <host:port> [--target <id>] [--folder <page folder>] [--tolerance <px>] ' +
    '[--out <directory>] [--chrome <executable>] [--chrome-defaults] [--limit <n>]');
  process.exit(2);
}
if (!executable || !existsSync(executable)) {
  console.error('no Chrome found: --chrome <executable>');
  process.exit(2);
}

const sleep = (ms) => new Promise((done) => setTimeout(done, ms));

// A DevTools protocol connection: Weft's, or Chrome's (whose calls of a page carry its session).
const connect = async (url) => {
  const socket = new WebSocket(url);
  await new Promise((done, fail) => {
    socket.addEventListener('open', done);
    socket.addEventListener('error', () => fail(new Error(`cannot connect to ${url}`)));
  });
  let next = 1;
  const waiting = new Map();
  socket.addEventListener('message', (event) => {
    const message = JSON.parse(event.data);
    if (message.id && waiting.has(message.id)) {
      const {done, fail} = waiting.get(message.id);
      waiting.delete(message.id);
      message.error ? fail(new Error(`${message.error.message}`)) : done(message.result);
    }
  });
  return {
    call: (method, params = {}, sessionId) => new Promise((done, fail) => {
      const id = next++;
      waiting.set(id, {done, fail});
      socket.send(JSON.stringify({id, method, params, sessionId}));
    }),
    close: () => socket.close(),
  };
};

// ---- The page as Weft has it.
const targets = await (await fetch(`http://${address}/json/list`)).json();
const target = wanted ? targets.find((t) => t.id === wanted) : targets[0];
if (!target) {
  console.error(wanted ? `no target ${wanted}` : 'no page is loaded');
  process.exit(2);
}
const weft = await connect(`ws://${address}/devtools/page/${target.id}`);
const snapshot = await weft.call('Weft.getSnapshot');
weft.close();

// Where the page's files are on this machine.
const baseFolder = folderOption ? resolve(folderOption) :
  snapshot.base && existsSync(dirname(snapshot.base)) ? dirname(snapshot.base) : undefined;

// The elements in tree order, as both sides list them.
const elements = [];
const index = (node, parent, depth) => {
  const entry = {node, parent, depth, children: []};
  elements.push(entry);
  let position = 0;
  for (const child of node.children ?? []) {
    if (child.text === undefined) {
      position += 1;
      const inner = index(child, entry, depth + 1);
      inner.position = position;
      entry.children.push(inner);
    }
  }
  return entry;
};
index(snapshot.tree, undefined, 0);

// ---- The same document in Chrome.
const uaMirror = '@layer weft-root,weft-ua;@layer weft-root{body>*{display:flow-root;position:relative}}' +
  (chromeDefaults ? '' : '@layer weft-ua{*{box-sizing:border-box;border-style:solid;border-width:0}}');
const escapeStyle = (text) => text.replace(/<\/style/gi, '<\\/style');
// Builds the document from the snapshot's tree; returns the elements in tree order.
const builder = `(tree) => {
  const xhtml = 'http://www.w3.org/1999/xhtml';
  const list = [];
  const build = (n) => {
    if (n.text !== undefined) {
      return document.createTextNode(n.text);
    }
    const e = document.createElementNS(n.ns ?? xhtml, n.tag === 'script' ? 'weft-script' : n.tag);
    for (const [name, value] of Object.entries(n.attributes ?? {})) {
      try {
        e.setAttribute(name, value);
      } catch {
        // A name markup does not take.
      }
    }
    list.push(e);
    e.weftNode = n;
    for (const child of n.children ?? []) {
      e.appendChild(build(child));
    }
    return e;
  };
  document.body.appendChild(build(tree));
  for (const e of list) {
    if (e.weftNode.value !== undefined && 'value' in e) {
      e.value = e.weftNode.value;
    }
  }
  for (const e of list) {
    if (e.weftNode.scroll) {
      e.scrollLeft = e.weftNode.scroll[0];
      e.scrollTop = e.weftNode.scroll[1];
    }
  }
  window.weftElements = list;
  return list.length;
}`;
const shell = (embedded) => '<!doctype html><html><head><meta charset="utf-8">' +
  (baseFolder ? `<base href="${pathToFileURL(baseFolder).href}/">` : '') +
  `<style>${uaMirror}html,body{margin:0;width:${snapshot.width}px;height:${snapshot.height}px;overflow:hidden}` +
  // The scene's root is the viewport's size, whatever the page gives it.
  `body>*{box-sizing:border-box!important;width:${snapshot.width}px!important;height:${snapshot.height}px!important}` +
  `</style><style>${escapeStyle(snapshot.stylesheet)}\n${escapeStyle(snapshot.keyframes ?? '')}</style></head><body>` +
  (embedded ? `<script>(${builder})(${JSON.stringify(snapshot.tree).replace(/</g, '\\u003c')})</script>` : '') +
  '</body></html>';

const work = mkdtempSync(join(tmpdir(), 'weft-compare-'));
const profile = join(work, 'profile');
const page = join(work, 'page.html');
writeFileSync(page, shell(false));
if (out) {
  mkdirSync(out, {recursive: true});
  writeFileSync(join(out, 'chrome.html'), shell(true));
}
const chrome = spawn(executable, ['--headless=new', '--no-first-run', '--disable-background-networking',
  '--allow-file-access-from-files', '--remote-debugging-port=0', `--user-data-dir=${profile}`, 'about:blank'],
{stdio: 'ignore'});
let chromeRects;
let chromeMoving;
let chromeVersion;
try {
  const portFile = join(profile, 'DevToolsActivePort');
  for (let i = 0; i < 200 && !existsSync(portFile); ++i) {
    await sleep(50);
  }
  const port = readFileSync(portFile, 'utf8').split('\n')[0];
  const version = await (await fetch(`http://127.0.0.1:${port}/json/version`)).json();
  chromeVersion = version.Browser;
  const browser = await connect(version.webSocketDebuggerUrl);
  const {targetId} = await browser.call('Target.createTarget', {url: 'about:blank'});
  const {sessionId} = await browser.call('Target.attachToTarget', {targetId, flatten: true});
  const call = (method, params) => browser.call(method, params, sessionId);
  await call('Page.enable');
  await call('Emulation.setDeviceMetricsOverride',
    {width: Math.round(snapshot.width), height: Math.round(snapshot.height), deviceScaleFactor: snapshot.scale, mobile: false});
  await call('Page.navigate', {url: pathToFileURL(page).href});
  for (let i = 0; i < 100; ++i) {
    const ready = await call('Runtime.evaluate', {expression: 'document.readyState', returnByValue: true});
    if (ready.result.value === 'complete') {
      break;
    }
    await sleep(50);
  }
  const built = await call('Runtime.evaluate',
    {expression: `(${builder})(${JSON.stringify(snapshot.tree)})`, returnByValue: true});
  if (built.exceptionDetails || built.result.value !== elements.length) {
    throw new Error(`Chrome built ${built.result?.value} elements of ${elements.length}: ` +
      `${built.exceptionDetails?.exception?.description ?? ''}`);
  }
  // The states the page's elements are in.
  const stated = elements.map((e, i) => [e.node.state, i]).filter(([state]) => state);
  if (stated.length) {
    await call('DOM.enable');
    await call('CSS.enable');
    await call('DOM.getDocument', {depth: 0});
    for (const [state, i] of stated) {
      const {result} = await call('Runtime.evaluate', {expression: `window.weftElements[${i}]`});
      const {nodeId} = await call('DOM.requestNode', {objectId: result.objectId});
      await call('CSS.forcePseudoState', {nodeId, forcedPseudoClasses: state.split(' ')});
    }
  }
  const measured = await call('Runtime.evaluate', {
    awaitPromise: true,
    returnByValue: true,
    expression: `(async () => {
      await document.fonts.ready;
      await Promise.all([...document.images].map((image) => image.decode().catch(() => undefined)));
      await new Promise((done) => requestAnimationFrame(() => requestAnimationFrame(done)));
      const list = window.weftElements;
      const moving = new Set();
      for (const animation of document.getAnimations()) {
        const at = list.indexOf(animation.effect && animation.effect.target);
        if (at >= 0 && animation.playState !== 'finished') {
          moving.add(at);
        }
      }
      return {
        rects: list.map((e) => {
          if (!e.getClientRects().length) {
            return null;
          }
          const r = e.getBoundingClientRect();
          return [r.x, r.y, r.width, r.height];
        }),
        moving: [...moving],
      };
    })()`,
  });
  if (measured.exceptionDetails) {
    throw new Error(measured.exceptionDetails.exception?.description ?? 'Chrome could not measure the page');
  }
  chromeRects = measured.result.value.rects;
  chromeMoving = new Set(measured.result.value.moving);
  browser.close();
} finally {
  chrome.kill();
  await sleep(200);
  rmSync(work, {recursive: true, force: true});
}

// ---- The boxes side by side.
const name = (entry) => {
  const a = entry.node.attributes ?? {};
  let text = entry.node.tag + (a.id ? `#${a.id}` : '') +
    (a.class ? '.' + a.class.trim().split(/\s+/).join('.') : '');
  if (!a.id && entry.parent && entry.parent.children.filter((c) => c.node.tag === entry.node.tag).length > 1) {
    text += `:nth-child(${entry.position})`;
  }
  return text;
};
const path = (entry) => {
  const parts = [];
  for (let e = entry; e; e = e.parent) {
    parts.unshift(name(e));
  }
  return parts.length > 4 ? '… > ' + parts.slice(-4).join(' > ') : parts.join(' > ');
};
elements.forEach((entry, i) => {
  entry.weft = entry.node.rect ?? null;
  entry.chrome = chromeRects[i];
  entry.moving = Boolean(entry.node.animated) || chromeMoving.has(i) || Boolean(entry.parent?.moving);
});
const far = (a, b) => Math.abs(a - b) > tolerance;
const differs = (a, b) => a.some((v, k) => far(v, b[k]));
// A box in its parent's: where it starts from the parent's start, and its size.
const within = (rect, parent) => parent ? [rect[0] - parent[0], rect[1] - parent[1], rect[2], rect[3]] : rect;
// What of a box's difference is its own. A box is placed in its parent (where it starts from the parent's start) and
// has a size; of the four numbers, one that differs is explained when the box before it among its siblings ends as far
// off (the box was pushed), or when its parent's size is as far off (the box stretches with it). What is left is the
// box's own difference: by name, how far Weft is from Chrome.
const ownDifference = (entry) => {
  const both = entry.parent?.weft && entry.parent?.chrome;
  const a = within(entry.weft, both && entry.parent.weft);
  const b = within(entry.chrome, both && entry.parent.chrome);
  const labels = both ? ['x in its parent', 'y in its parent', 'w', 'h'] : ['x', 'y', 'w', 'h'];
  let pushedBy = [0, 0];
  let stretchedBy = [0, 0];
  if (both) {
    const at = entry.parent.children.indexOf(entry);
    const before = at > 0 ? entry.parent.children[at - 1] : undefined;
    if (before?.weft && before?.chrome) {
      const c = within(before.weft, entry.parent.weft);
      const d = within(before.chrome, entry.parent.chrome);
      pushedBy = [c[0] + c[2] - d[0] - d[2], c[1] + c[3] - d[1] - d[3]];
    }
    stretchedBy = [entry.parent.weft[2] - entry.parent.chrome[2], entry.parent.weft[3] - entry.parent.chrome[3]];
  }
  const out = [];
  for (let k = 0; k < 4; ++k) {
    const by = a[k] - b[k];
    const explained = k < 2 ? pushedBy[k] : stretchedBy[k - 2];
    if (far(a[k], b[k]) && !(far(explained, 0) && !far(by, explained))) {
      out.push([labels[k], by]);
    }
  }
  return out;
};
let compared = 0;
let moved = 0;
let moving = 0;
const own = [];
for (const entry of elements) {
  if (entry.moving) {
    moving += 1;
    continue;
  }
  compared += 1;
  if (!entry.weft || !entry.chrome) {
    if (Boolean(entry.weft) !== Boolean(entry.chrome)) {
      // A box only one of them makes; what is in a box neither makes is not laid out.
      if (!entry.parent || (entry.parent.weft && entry.parent.chrome)) {
        entry.difference = [];
        own.push(entry);
      }
    }
    continue;
  }
  if (!differs(entry.weft, entry.chrome)) {
    continue;
  }
  entry.difference = ownDifference(entry);
  if (entry.difference.length) {
    own.push(entry);
  } else {
    moved += 1;
  }
}

const number = (value) => String(Math.round(value * 100) / 100);
const box = (rect) => rect ? `x ${number(rect[0])}  y ${number(rect[1])}  w ${number(rect[2])}  h ${number(rect[3])}` :
  'no box';
const delta = (entry) => entry.difference.map(([label, by]) => `${label} ${by > 0 ? '+' : '−'}${number(Math.abs(by))}`)
  .join(', ');
// Siblings of one kind that differ the same way are one entry.
const groups = [];
for (const entry of own) {
  const kind = `${entry.node.tag}.${entry.node.attributes?.class ?? ''}|${Boolean(entry.weft)}${Boolean(entry.chrome)}|` +
    delta(entry);
  const last = groups.at(-1);
  if (last && last.kind === kind && last.entries[0].parent === entry.parent) {
    last.entries.push(entry);
  } else {
    groups.push({kind, entries: [entry]});
  }
}

console.log(`Weft ${number(snapshot.width)}×${number(snapshot.height)} @${number(snapshot.scale)}x and ${chromeVersion}` +
  `: ${snapshot.base || target.title}`);
console.log(`${compared} elements compared within ${tolerance} px: ${own.length} differ in their own box` +
  (moved ? `, ${moved} more only moved, were pushed or stretched with one that does` : '') +
  (moving ? `; ${moving} that are animating were left out` : ''));
if (!chromeDefaults) {
  console.log('Chrome was given the scene\'s box-sizing: border-box and zero-width solid borders ' +
    '(--chrome-defaults: without them).');
}
if (snapshot.safeArea.some((inset) => inset !== 0)) {
  console.log(`The View has a safe area (${snapshot.safeArea.join(', ')}) that Chrome does not: what reads env() differs.`);
}
if (!baseFolder) {
  console.log('The page\'s files are not on this machine (--folder <page folder>): Chrome has no images or fonts of it.');
}
for (const {entries} of groups.slice(0, limit)) {
  const entry = entries[0];
  console.log('');
  console.log(`${'  '.repeat(Math.min(entry.depth, 6))}${path(entry)}`);
  const indent = '  '.repeat(Math.min(entry.depth, 6)) + '    ';
  console.log(`${indent}Weft    ${box(entry.weft)}`);
  console.log(`${indent}Chrome  ${box(entry.chrome)}${delta(entry) ? `    (Weft: ${delta(entry)})` : ''}`);
  for (const dropped of entry.node.unsupported ?? []) {
    console.log(`${indent}Weft does not take: ${dropped}`);
  }
  if (entries.length > 1) {
    console.log(`${indent}and ${entries.length - 1} of its siblings the same way`);
  }
}
if (groups.length > limit) {
  console.log(`\n… and ${groups.length - limit} more (--limit <n>).`);
}
if (out) {
  writeFileSync(join(out, 'report.json'), JSON.stringify({
    weft: {width: snapshot.width, height: snapshot.height, scale: snapshot.scale, base: snapshot.base},
    chrome: chromeVersion,
    tolerance,
    compared,
    moved,
    moving,
    differences: own.map((entry) => ({path: path(entry), weft: entry.weft, chrome: entry.chrome,
      own: Object.fromEntries(entry.difference), unsupported: entry.node.unsupported ?? []})),
  }, null, 1) + '\n');
  console.log(`\n${join(out, 'chrome.html')} is the same document for Chrome; ${join(out, 'report.json')} has every difference.`);
}
process.exit(own.length ? 1 : 0);
