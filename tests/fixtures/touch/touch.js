// Touch scenarios (tests/fixtures/touch/scenarios.json): the same page in Chrome (tools/browser_touch_reference.mjs) and
// in a weft View (tests/touch_tests.cpp). It builds its elements in #root and logs the pointer, touch, mouse, click and
// scroll events window sees in its capture phase (its listeners are not passive). Pointer ids and touch identifiers are
// numbered in the order they first appear (Chrome's own numbers depend on what came before).
(() => {
  const root = document.getElementById('root');
  const element = (tag, attributes, parent) => {
    const n = document.createElement(tag);
    for (const [k, v] of Object.entries(attributes)) n.setAttribute(k, v);
    (parent || root).appendChild(n);
    return n;
  };
  element('div', {id: 'tap', class: 'box'});
  const nostart = element('div', {id: 'nostart', class: 'box'});
  const noend = element('div', {id: 'noend', class: 'box'});
  const grab = element('div', {id: 'grab', class: 'box'});
  element('div', {id: 'pad', class: 'box'});
  const list = element('div', {id: 'list'});
  for (let i = 0; i < 20; i++) element('div', {id: 'row' + i, class: 'row'}, list);
  const xlist = element('div', {id: 'xlist'});
  element('div', {id: 'wide', class: 'wide'}, xlist);

  // nostart's touchstart and noend's touchend are prevented (listeners on the elements are not passive).
  nostart.addEventListener('touchstart', e => e.preventDefault(), {passive: false});
  noend.addEventListener('touchend', e => e.preventDefault(), {passive: false});
  // grab's touchmoves are prevented: the page drags, the browser does not pan.
  grab.addEventListener('touchmove', e => e.preventDefault(), {passive: false});

  const name = n => !n ? null : n === window ? 'window' : n === document ? 'document' :
    n === document.body ? 'body' : n === document.documentElement ? 'html' : n.id || n.localName || n.nodeName;
  // Small numbers for pointer ids and touch identifiers, in the order they appear.
  const numbers = new Map();
  const number = v => {
    if (v === undefined || v === null) return null;
    if (!numbers.has(v)) numbers.set(v, numbers.size + 1);
    return numbers.get(v);
  };
  const touchNumbers = new Map();
  const touchNumber = v => {
    if (!touchNumbers.has(v)) touchNumbers.set(v, touchNumbers.size + 1);
    return touchNumbers.get(v);
  };
  const round = v => Math.round(v * 100) / 100;
  const log = [];
  let origin = performance.now();
  const record = e => {
    const o = {type: e.type, target: name(e.target), cancelable: e.cancelable};
    if (/^pointer|^gotpointer|^lostpointer/.test(e.type)) {
      Object.assign(o, {id: e.pointerId === 1 ? 'mouse' : number(e.pointerId), pointerType: e.pointerType,
        isPrimary: e.isPrimary, x: round(e.clientX), y: round(e.clientY), button: e.button, buttons: e.buttons,
        width: round(e.width), height: round(e.height), pressure: round(e.pressure)});
    } else if (/^touch/.test(e.type)) {
      const list = l => Array.from(l, t => ({id: touchNumber(t.identifier), target: name(t.target), x: round(t.clientX),
        y: round(t.clientY)}));
      Object.assign(o, {touches: list(e.touches), targetTouches: e.targetTouches.length,
        changed: list(e.changedTouches)});
    } else if (/click|mouse|contextmenu/.test(e.type)) {
      Object.assign(o, {x: round(e.clientX), y: round(e.clientY), button: e.button, buttons: e.buttons,
        detail: e.detail, pointerType: e.pointerType === undefined ? null : e.pointerType});
    } else if (e.type === 'scroll') {
      Object.assign(o, {top: round(e.target.scrollTop), left: round(e.target.scrollLeft),
        t: Math.round(performance.now() - origin)});
    }
    log.push(o);
  };
  const types = ['pointerover', 'pointerenter', 'pointerdown', 'pointermove', 'pointerup', 'pointercancel',
    'pointerout', 'pointerleave', 'gotpointercapture', 'lostpointercapture', 'touchstart', 'touchmove', 'touchend',
    'touchcancel', 'mouseover', 'mouseenter', 'mousedown', 'mousemove', 'mouseup', 'mouseout', 'mouseleave', 'click',
    'dblclick', 'contextmenu'];
  // Not passive: the touch events are dispatched as a page that may cancel them sees them (with only passive listeners,
  // Chrome sends them uncancelable where nothing could cancel them).
  for (const t of types) window.addEventListener(t, record, {capture: true, passive: false});
  // Chrome dispatches enter and leave events only where a listener can see them (one on an element, or capturing on
  // an ancestor element; window's does not count): capturing ones on #root make it dispatch them from #root down, the
  // elements a weft scene has.
  for (const t of ['pointerenter', 'pointerleave', 'mouseenter', 'mouseleave']) {
    root.addEventListener(t, () => {}, {capture: true});
  }
  // scroll does not bubble: listen on the scrollers.
  list.addEventListener('scroll', record, {passive: true});
  xlist.addEventListener('scroll', record, {passive: true});

  const matching = selector => Array.from(document.querySelectorAll(selector)).map(name)
    .filter(n => n !== 'html' && n !== 'body' && n !== 'root');
  globalThis.__touch = {
    // The events since the last take; scroll times count from mark().
    take() {
      const out = log.slice();
      log.length = 0;
      return out;
    },
    mark() {
      origin = performance.now();
    },
    state() {
      return {list: round(list.scrollTop), xlist: round(xlist.scrollLeft), hover: matching(':hover'),
        active: matching(':active')};
    },
    // An element's box: x, y, width, height in client px.
    box(id) {
      const r = document.getElementById(id).getBoundingClientRect();
      return [r.left, r.top, r.width, r.height];
    },
  };
})();
