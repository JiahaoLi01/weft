// Keyboard and focus scenarios (tests/fixtures/keyboard/scenarios.json): the same page in Chrome
// (tools/browser_keyboard_reference.mjs) and in a weft View (tests/keyboard_tests.cpp). It builds its elements in #root
// and logs the key, mouse and focus events window sees in its capture phase, with the element that has the focus then.
(() => {
  const root = document.getElementById('root');
  const element = (tag, attributes, text, parent) => {
    const n = document.createElement(tag);
    for (const [k, v] of Object.entries(attributes)) n.setAttribute(k, v);
    if (text) n.appendChild(document.createTextNode(text));
    (parent || root).appendChild(n);
    return n;
  };
  const row0 = element('div', {id: 'row0'});
  element('button', {id: 'b1'}, 'One', row0);
  const row1 = element('div', {id: 'row1'});
  element('a', {id: 'a1', href: '#x'}, 'Link', row1);
  element('a', {id: 'a0'}, 'Bare', row1);
  element('span', {id: 'plain'}, 'Plain text', row1);
  element('div', {id: 't2', class: 'f', tabindex: '2'}, 'Two');
  element('div', {id: 't1', class: 'f', tabindex: '1'}, 'One');
  const d0 = element('div', {id: 'd0', class: 'f', tabindex: '0'});
  element('span', {id: 'd0s'}, 'Zero', d0);
  element('div', {id: 'dm', class: 'f', tabindex: '-1'}, 'Minus');
  const row2 = element('div', {id: 'row2'});
  element('button', {id: 'bd', disabled: ''}, 'Disabled', row2);
  element('button', {id: 'bn', style: 'display: none'}, 'None', row2);
  element('button', {id: 'bv', style: 'visibility: hidden'}, 'Hidden', row2);
  element('button', {id: 'b2'}, 'Two', row2);
  const bp = element('button', {id: 'bp'}, 'Prevent', row2);
  const bk = element('button', {id: 'bk'}, 'Keypress', row2);
  const md = element('button', {id: 'md'}, 'NoMouse', row2);
  const sc = element('div', {id: 'sc'});
  element('div', {id: 'tall1', class: 'tall'}, 'Above', sc);
  element('button', {id: 'bs'}, 'Scrolled', sc);
  element('div', {id: 'tall2', class: 'tall'}, 'Below', sc);
  // A scroll container with nothing focusable in it: Chrome's keyboard can scroll it (Tab reaches it).
  const sc2 = element('div', {id: 'sc2'});
  element('div', {id: 'tall3', class: 'tall'}, 'Scroll me', sc2);
  element('div', {id: 'tail', class: 'f', tabindex: '0'}, 'Tail');

  // bp's keydown of Enter and Space, bk's keypresses and md's mousedown are prevented.
  bp.addEventListener('keydown', e => {
    if (e.key === 'Enter' || e.key === ' ') e.preventDefault();
  });
  bk.addEventListener('keypress', e => e.preventDefault());
  md.addEventListener('mousedown', e => e.preventDefault());

  const name = n => !n ? null : n === window ? 'window' : n === document ? 'document' :
    n === document.body ? 'body' : n.id || n.localName || n.nodeName;
  const visible = n => {
    try { return n && n.matches ? n.matches(':focus-visible') : null; } catch (_) { return 'error'; }
  };
  const log = [];
  const record = e => {
    const o = {type: e.type, target: name(e.target)};
    if (/^key/.test(e.type)) {
      Object.assign(o, {key: e.key, code: e.code, keyCode: e.keyCode, which: e.which, charCode: e.charCode,
        location: e.location, repeat: e.repeat, isComposing: e.isComposing});
    } else if (/click|mouse|pointer/.test(e.type)) {
      Object.assign(o, {cls: e.constructor && e.constructor.name, detail: e.detail, button: e.button,
        buttons: e.buttons, x: e.clientX, y: e.clientY});
      if ('pointerId' in e) Object.assign(o, {pointerId: e.pointerId, pointerType: e.pointerType});
    } else {
      Object.assign(o, {related: name(e.relatedTarget), visible: visible(e.target)});
    }
    Object.assign(o, {mods: (e.ctrlKey ? 'C' : '') + (e.shiftKey ? 'S' : '') + (e.altKey ? 'A' : '') +
      (e.metaKey ? 'M' : ''), bubbles: e.bubbles, cancelable: e.cancelable, trusted: e.isTrusted,
      active: name(document.activeElement)});
    log.push(o);
  };
  for (const type of ['keydown', 'keypress', 'keyup', 'click', 'mousedown', 'mouseup', 'pointerdown', 'pointerup',
    'focus', 'blur', 'focusin', 'focusout']) {
    window.addEventListener(type, record, true);
  }
  globalThis.__keyboard = {
    // The events since the last take.
    take() { return log.splice(0, log.length); },
    // The focus and the scroll container's position.
    state() {
      return {active: name(document.activeElement), visible: visible(document.activeElement), scroll: sc.scrollTop,
        scroll2: sc2.scrollTop};
    },
    // The centre of an element in the viewport, for clicks.
    centre(id) {
      const r = document.getElementById(id).getBoundingClientRect();
      return [r.left + r.width / 2, r.top + r.height / 2];
    },
  };
})();
