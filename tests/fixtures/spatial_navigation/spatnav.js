// Spatial navigation scenarios (tests/fixtures/spatial_navigation/scenarios.json): the same page in Chrome with
// --enable-spatial-navigation (tools/browser_spatial_navigation_reference.mjs) and in a weft View of a page with
// WEFT_PAGE_SPATIAL_NAVIGATION (tests/spatial_navigation_tests.cpp). Every element that may take the focus sits at a
// place of its own (spatnav.css), so that both lay them out alike; the page logs the key, focus, click and mouse-over
// events window sees in its capture phase, with the element that has the focus then.
(() => {
  const root = document.getElementById('root');
  const element = (tag, attributes, text, parent) => {
    const n = document.createElement(tag);
    for (const [k, v] of Object.entries(attributes)) n.setAttribute(k, v);
    if (text) n.appendChild(document.createTextNode(text));
    (parent || root).appendChild(n);
    return n;
  };
  // A grid of buttons, three by three, its middle row moved to the right.
  for (let r = 0; r < 3; ++r) {
    for (let c = 0; c < 3; ++c) element('button', {id: `g${r}${c}`, class: `cell r${r} c${c}`}, `${r}${c}`);
  }
  // A row of links of different widths.
  element('a', {id: 'l0', href: '#a', class: 'link'}, 'Short');
  element('a', {id: 'l1', href: '#b', class: 'link'}, 'A longer link');
  element('a', {id: 'l2', href: '#c', class: 'link'}, 'Mid link');
  // A panel that takes the focus with a button inside, and one that does not with a button inside.
  const panel = element('div', {id: 'panel', tabindex: '0', class: 'panel'});
  element('button', {id: 'pb', class: 'inner'}, 'Inside', panel);
  const box = element('div', {id: 'box', class: 'panel'});
  element('button', {id: 'bb', class: 'inner'}, 'Boxed', box);
  // A scroll container whose items do not all show.
  const sc = element('div', {id: 'sc'});
  for (let k = 0; k < 6; ++k) element('button', {id: `s${k}`, class: 'item'}, `Item ${k}`, sc);
  // A text field and a textarea of two lines, and two buttons, one over the other.
  element('input', {id: 'in', value: 'text'});
  element('textarea', {id: 'ta'}, 'one\ntwo');
  element('button', {id: 'under'}, 'Under');
  element('button', {id: 'over'}, 'Over');
  // What takes no focus: a disabled button and a hidden one; and a button far down on the right.
  element('button', {id: 'off', disabled: ''}, 'Disabled');
  element('button', {id: 'hid'}, 'Hidden');
  element('button', {id: 'far'}, 'Far');
  // A button whose keydown listener moves the focus itself on ArrowRight.
  const jump = element('button', {id: 'jump'}, 'Jump');
  jump.addEventListener('keydown', e => {
    if (e.key === 'ArrowRight') document.getElementById('g22').focus();
  });
  // What spatial navigation lets take the focus besides (Element::SupportsSpatialNavigationFocus): a div with a click
  // listener, one with a mouseenter listener, and a card that sets cursor: pointer, whose label inherits it and whose
  // badge sets it again; a div with only a mouseleave listener takes none.
  element('div', {id: 'leave', class: 'extra'}, 'Leave').addEventListener('mouseleave', () => {});
  element('div', {id: 'clicky', class: 'extra'}, 'Click').addEventListener('click', () => {});
  element('div', {id: 'hover', class: 'extra'}, 'Over').addEventListener('mouseenter', () => {});
  const card = element('div', {id: 'card', class: 'extra'});
  element('span', {id: 'label'}, 'Card', card);
  element('span', {id: 'badge'}, '!', card);

  const name = n => !n ? null : n === window ? 'window' : n === document ? 'document' :
    n === document.body ? 'body' : n.id || n.localName || n.nodeName;
  const log = [];
  const record = e => {
    const o = {type: e.type, target: name(e.target)};
    if (/^key/.test(e.type)) {
      Object.assign(o, {key: e.key, code: e.code, keyCode: e.keyCode});
    } else if (/^mouse|^pointer/.test(e.type)) {
      Object.assign(o, {related: name(e.relatedTarget)});
    } else if (/focus|blur/.test(e.type)) {
      Object.assign(o, {related: name(e.relatedTarget)});
    }
    o.active = name(document.activeElement);
    log.push(o);
  };
  for (const type of ['keydown', 'keypress', 'keyup', 'click', 'focus', 'blur', 'mouseover', 'mouseout']) {
    window.addEventListener(type, record, true);
  }
  globalThis.__spatnav = {
    // The events since the last take.
    take() { return log.splice(0, log.length); },
    // The focus, whether it shows as :focus-visible, the element under the pointer (the innermost :hover), what is
    // :active, the scroll container's position and the carets of the text field and the textarea.
    state() {
      const hovered = Array.from(document.querySelectorAll(':hover')).pop() ?? null;
      const active = Array.from(document.querySelectorAll(':active')).pop() ?? null;
      const field = document.getElementById('in');
      const area = document.getElementById('ta');
      return {active: name(document.activeElement),
        visible: document.activeElement && document.activeElement !== document.body ?
          document.activeElement.matches(':focus-visible') : null,
        hover: name(hovered), pressed: name(active), scroll: sc.scrollTop, caret: field.selectionStart,
        area: area.selectionStart};
    },
  };
})();
