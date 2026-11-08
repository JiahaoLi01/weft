// Text fields to copy, cut, paste, undo and redo in (tools/browser_field_editing_reference.mjs records what Chrome does
// with steps.json, tests/field_editing_tests.cpp what a View does): a text input, a textarea, an input of maxlength 5,
// a read-only one, a password, and three whose listeners change what happens (one gives copy its own data, one
// prevents paste, one prevents undo). window.__log has every clipboard and input event at them, in order.
(() => {
  const root = document.getElementById('root');
  const log = (window.__log = []);
  const field = (tag, id, attributes = {}) => {
    const e = document.createElement(tag);
    e.id = id;
    for (const [k, v] of Object.entries(attributes)) e.setAttribute(k, v);
    root.appendChild(e);
    return e;
  };
  field('input', 'a');
  field('textarea', 'b');
  field('input', 'c', {maxlength: '5'});
  field('input', 'd', {readonly: '', value: 'read only'});
  field('input', 'e', {type: 'password', value: 'secret'});
  const f = field('input', 'f', {value: 'own data'});
  const g = field('input', 'g');
  const h = field('input', 'h');
  const text = (dataTransfer) => (dataTransfer ? dataTransfer.getData('text/plain') : null);
  for (const e of root.querySelectorAll('input, textarea')) {
    for (const type of ['copy', 'cut', 'paste']) {
      e.addEventListener(type, (event) => log.push([type, e.id, text(event.clipboardData), event.bubbles,
        event.cancelable]));
    }
    for (const type of ['beforeinput', 'input']) {
      e.addEventListener(type, (event) => log.push([type, e.id, event.inputType, event.data,
        text(event.dataTransfer)]));
    }
  }
  f.addEventListener('copy', (event) => {
    event.clipboardData.setData('text/plain', 'given by copy');
    event.preventDefault();
  });
  g.addEventListener('paste', (event) => event.preventDefault());
  h.addEventListener('beforeinput', (event) => {
    if (event.inputType === 'historyUndo') event.preventDefault();
  });
  // The focused field's id, and the fields' values and selections.
  window.__state = (label) => log.push(['state', label, (document.activeElement || {}).id || null,
    ...[...root.querySelectorAll('input, textarea')].map((e) => [e.id, e.value, e.selectionStart, e.selectionEnd])]);
})();
