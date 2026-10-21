// Text field scenarios (tests/fixtures/text_field/scenarios.json): the same page in Chrome
// (tools/browser_text_field_reference.mjs) and in a weft View (tests/text_field_tests.cpp). It builds its fields in #root
// and logs what window sees in its capture phase of the keyboard, the fields' input, change and selection, composition,
// focus and the mouse, with each field's value and selection when an event comes.
(() => {
  const root = document.getElementById('root');
  const element = (tag, attributes, parent) => {
    const n = document.createElement(tag);
    for (const [k, v] of Object.entries(attributes)) n.setAttribute(k, v);
    (parent || root).appendChild(n);
    return n;
  };
  const row = (id) => element('div', {id, class: 'row'});
  element('input', {id: 'i1', placeholder: 'Name'}, row('r1'));
  element('input', {id: 'i2', value: 'hello world'}, row('r2'));
  element('input', {id: 'ip', type: 'password', value: 'secret'}, row('r3'));
  element('input', {id: 'ir', readonly: '', value: 'fixed'}, row('r4'));
  element('input', {id: 'id', disabled: '', value: 'off'}, row('r5'));
  element('input', {id: 'im', maxlength: '5'}, row('r6'));
  element('input', {id: 'iw', class: 'narrow', value: 'The quick brown fox jumps over the lazy dog'}, row('r7'));
  element('input', {id: 'iu', class: 'ua'}, row('r8'));
  element('input', {id: 'is', type: 'search', value: 'find'}, row('r9'));
  element('button', {id: 'b'}, row('r10')).textContent = 'Button';
  const prevent = element('input', {id: 'ib', value: 'guarded'}, row('r11'));
  // Edits of ib's are refused before they happen (beforeinput), but for its composition, which the page may not stop.
  prevent.addEventListener('beforeinput', e => {
    if (e.inputType !== 'insertCompositionText') e.preventDefault();
  });
  // A textarea three lines tall whose default value (its child text) is two lines, and one with the UA's own styles.
  element('textarea', {id: 'ta', class: 'area'}, element('div', {id: 'r12', class: 'row tall'})).textContent =
    'first line\nsecond';
  element('textarea', {id: 'tu'}, element('div', {id: 'r13', class: 'row tall'}));

  const fields = () => [...document.querySelectorAll('input, textarea')];
  const name = n => !n ? null : n === window ? 'window' : n === document ? 'document' :
    n === document.body ? 'body' : n.id || n.localName || n.nodeName;
  const field = n => n && (n.localName === 'input' || n.localName === 'textarea') ?
    {value: n.value, start: n.selectionStart, end: n.selectionEnd, direction: n.selectionDirection} : null;
  const log = [];
  const record = e => {
    const o = {type: e.type, target: name(e.target)};
    if (/^key/.test(e.type)) {
      Object.assign(o, {key: e.key, code: e.code, keyCode: e.keyCode, isComposing: e.isComposing});
    } else if (e.type === 'beforeinput' || e.type === 'input') {
      Object.assign(o, {cls: e.constructor && e.constructor.name, inputType: e.inputType, data: e.data,
        isComposing: e.isComposing, cancelable: e.cancelable});
    } else if (/^composition/.test(e.type)) {
      Object.assign(o, {cls: e.constructor && e.constructor.name, data: e.data});
    } else if (/click|mouse/.test(e.type)) {
      Object.assign(o, {detail: e.detail});
    }
    const f = field(e.target);
    if (f) o.field = f;
    log.push(o);
  };
  for (const type of ['keydown', 'keypress', 'keyup', 'beforeinput', 'input', 'change', 'select', 'compositionstart',
    'compositionupdate', 'compositionend', 'focus', 'blur', 'mousedown', 'mouseup', 'click', 'dblclick']) {
    window.addEventListener(type, record, true);
  }
  document.addEventListener('selectionchange', e => log.push({type: 'selectionchange', target: name(e.target)}));

  globalThis.__field = {
    take() {
      const out = log.slice();
      log.length = 0;
      return out;
    },
    state() {
      const out = {active: name(document.activeElement)};
      for (const f of fields()) {
        out[f.id] = Object.assign(field(f), {scrollLeft: Math.round(f.scrollLeft)});
      }
      return out;
    },
    // Where a point of an element is in the viewport: its border box's left and top plus (x, y).
    at(id, x, y) {
      const r = document.getElementById(id).getBoundingClientRect();
      return [r.left + x, r.top + y];
    },
    rect(id) {
      const r = document.getElementById(id).getBoundingClientRect();
      return [r.left, r.top, r.width, r.height];
    },
  };
})();
