// What an img reports to script as its src changes and it loads (tools/browser_img_reference.mjs records Chrome's,
// tests/scene_img_tests.cpp compares a View's): complete, naturalWidth and naturalHeight, the width and height IDL
// attributes, the load and error events and their order, decode(), new Image() and reflection. Each entry of
// window.__imgLog is a step's name and what it saw.
(async () => {
  const log = [];
  const root = document.getElementById('root');
  const state = (name, i) => log.push([name, i.complete, i.naturalWidth, i.naturalHeight, i.width, i.height]);
  // The next load or error event of an img.
  const settled = (i) => new Promise((resolve) => {
    const done = (e) => {
      i.removeEventListener('load', done);
      i.removeEventListener('error', done);
      resolve(e.type);
    };
    i.addEventListener('load', done);
    i.addEventListener('error', done);
  });

  const a = new Image();
  state('new Image()', a);
  log.push(['new Image() is an img', a.localName, a instanceof HTMLImageElement, a.getAttribute('src'), a.src]);
  const loadingA = settled(a);
  a.src = 'quad-40x30.png';
  state('src set', a);
  log.push(['src reflects', a.getAttribute('src'), a.src.endsWith('/quad-40x30.png') || a.src === 'quad-40x30.png']);
  log.push(['first', await loadingA]);
  state('loaded, not in the document', a);
  root.appendChild(a);
  state('in the document', a);
  a.style.width = '80px';
  state('styled 80px', a);
  a.style.padding = '5px';
  state('padded', a);
  a.style.display = 'none';
  state('display none', a);
  a.remove();

  const b = new Image(30, 20);
  log.push(['new Image(30, 20)', b.getAttribute('width'), b.getAttribute('height'), b.width, b.height]);
  b.width = 44;
  log.push(['width set', b.getAttribute('width'), b.width, b.height]);
  b.height = 0;
  log.push(['height set 0', b.getAttribute('height'), b.height]);

  const c = document.createElement('img');
  const failing = settled(c);
  c.src = 'missing.png';
  state('missing src set', c);
  log.push(['missing', await failing]);
  state('missing settled', c);
  c.setAttribute('width', '25');
  state('missing with a width', c);

  const d = document.createElement('img');
  d.src = 'tall-20x50.png';
  await d.decode();
  state('decoded', d);
  log.push(['currentSrc', d.currentSrc.endsWith('tall-20x50.png')]);
  const e = document.createElement('img');
  e.src = 'missing-too.png';
  log.push(['decode of a missing image', await e.decode().then(() => 'resolved', (x) => x.name)]);
  log.push(['decode without src', await document.createElement('img').decode().then(() => 'resolved',
    (x) => x.name)]);

  const f = document.createElement('img');
  const order = [];
  f.onload = (event) => order.push('onload ' + event.bubbles + ' ' + event.cancelable + ' ' + (event.target === f) +
    ' ' + (this === undefined));
  f.addEventListener('load', () => order.push('listener ' + f.complete + ' ' + f.naturalWidth));
  root.addEventListener('load', () => order.push('root capture'), true);
  root.addEventListener('load', () => order.push('root bubble'));
  root.appendChild(f);
  const loadingF = settled(f);
  f.src = 'quad-16x16.png';
  Promise.resolve().then(() => order.push('microtask'));
  setTimeout(() => order.push('timeout 0'), 0);
  await loadingF;
  log.push(['order', order.join(', ')]);
  log.push(['onload handler', typeof f.onload, f.onerror]);

  const changing = settled(f);
  f.src = 'wide-120x40.png';
  state('src changed', f);
  log.push(['changed', await changing]);
  state('changed and loaded', f);
  f.src = 'wide-120x40.png';
  state('same src again', f);
  f.removeAttribute('src');
  state('src removed', f);
  const emptied = settled(f);
  f.src = '';
  state('src empty', f);
  log.push(['empty src', await Promise.race([emptied, new Promise((r) => setTimeout(() => r('no event'), 100))])]);

  const g = document.createElement('img');
  g.alt = 'a picture';
  log.push(['alt', g.getAttribute('alt'), g.alt, document.createElement('img').alt]);
  g.setAttribute('width', 'abc');
  log.push(['width attribute not a number', g.width]);
  g.setAttribute('width', '33.7');
  log.push(['width attribute 33.7', g.width]);
  log.push(['complete without src', g.complete, g.naturalWidth]);
  window.__imgLog = log;
})().catch((x) => {
  window.__imgLog = [['exception', String(x), x && x.stack]];
});
