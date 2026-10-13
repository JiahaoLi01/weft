// The gallery's translucent window page (docs/refactoring-plan.md, step 11.5c): written for the native scene by hand,
// without a framework. Each case stacks windows over a background that moves every frame: frosted glass (what lies
// behind blurred and saturated under a translucent tint, with a highlight edge), plain translucency (a translucent tint
// only) and blur only (what lies behind blurred, no tint). A window's title bar drags it and brings it to the front;
// − and + change how many there are. __ceGlassPage lets probes set the count and read the windows.
(() => {
  const kinds = ['frosted', 'plain', 'blur'];
  const groups = {GLASS_TESTS: [
    {label: 'Glass: Frosted - blur, saturate, tint, highlight'},
    {label: 'Glass: Plain Translucent - tint only'},
    {label: 'Glass: Blur Only - no tint'}]};
  const firstWindows = 4, maxWindows = 32;
  const windowWidth = 360, windowHeight = 232, margin = 48, toolbar = 84;
  const titles = ['Inventory', 'Party', 'Quest Log', 'Map', 'Skills', 'Journal', 'Crafting', 'Bestiary'];
  let active = null, errors = [], nodes = null, kind = 'frosted', drags = 0, drag = null;
  function record(error) {
    if (errors.length < 64) errors.push((String(error) + (error instanceof Error && error.stack ? '\n' + error.stack : '')).slice(0, 8192));
  }
  const guard = f => (...args) => { try { return f(...args); } catch (e) { record(e); } };
  const add = (parent, tag, cls, text) => {
    const e = document.createElement(tag);
    if (cls) e.setAttribute('class', cls);
    if (text !== undefined) e.textContent = text;
    parent.appendChild(e);
    return e;
  };
  // Window i's place: a scatter over the page, the same for the same size, every window inside it.
  function place(i) {
    const w = nodes.page.getBoundingClientRect();
    const spanX = Math.max(1, w.width - windowWidth - 2 * margin);
    const spanY = Math.max(1, w.height - windowHeight - margin - toolbar);
    return {left: margin + (i * 157) % spanX, top: margin + (i * 71) % spanY};
  }
  function move(win, left, top) {
    nodes.at.set(win, {left, top});
    win.style.left = left + 'px';
    win.style.top = top + 'px';
  }
  function makeWindow(i) {
    const win = add(nodes.stack, 'section', 'gw-window gw-' + kind);
    const bar = add(win, 'header', 'gw-titlebar');
    add(bar, 'div', 'gw-dot gw-dot-close');
    add(bar, 'div', 'gw-dot gw-dot-min');
    add(bar, 'div', 'gw-dot gw-dot-max');
    add(bar, 'div', 'gw-name', titles[i % titles.length] + ' ' + (i + 1));
    if (kind === 'frosted') add(win, 'div', 'gw-sheen');
    const body = add(win, 'div', 'gw-body');
    const rows = [['Gold', String(1240 + 37 * i)], ['Weight', (12 + i % 9) + ' / 40'], ['Level', String(3 + i % 17)]];
    for (const [k, v] of rows) {
      const row = add(body, 'div', 'gw-row');
      add(row, 'div', 'gw-key', k);
      add(row, 'div', 'gw-value', v);
    }
    const meter = add(body, 'div', 'gw-meter');
    const fill = add(meter, 'div', 'gw-fill');
    fill.style.width = (20 + (i * 23) % 75) + '%';
    const at = place(i);
    move(win, at.left, at.top);
    bar.addEventListener('mousedown', guard(e => {
      // To the front, then follow the pointer.
      nodes.stack.appendChild(win);
      const from = nodes.at.get(win);
      drag = {win, x: e.clientX, y: e.clientY, left: from.left, top: from.top};
    }));
    return win;
  }
  function follow(e) {
    if (!drag) return;
    const r = nodes.page.getBoundingClientRect();
    const left = Math.min(Math.max(drag.left + e.clientX - drag.x, -windowWidth / 2), r.width - windowWidth / 2);
    const top = Math.min(Math.max(drag.top + e.clientY - drag.y, 0), r.height - 40);
    move(drag.win, left, top);
  }
  function release() {
    if (!drag) return;
    drag = null;
    ++drags;
  }
  function setCount(n) {
    if (!nodes) return;
    n = Math.min(Math.max(Math.round(n), 1), maxWindows);
    while (nodes.windows.length < n) nodes.windows.push(makeWindow(nodes.windows.length));
    while (nodes.windows.length > n) {
      const win = nodes.windows.pop();
      if (drag && drag.win === win) drag = null;
      nodes.at.delete(win);
      win.parentNode.removeChild(win);
    }
    nodes.shown.textContent = 'Windows ' + n;
  }
  function mount(group, index) {
    if (!groups[group]?.[index]) throw new Error('CE_GALLERY_CASE: unknown registration');
    unmount();
    errors = [];
    drags = 0;
    kind = kinds[index];
    active = {group, index, label: groups[group][index].label};
    const root = document.getElementById('root');
    const page = add(root, 'div', 'gw-page');
    // The background: drifting lights, a turning ring, a band of tiles that scrolls and a title, all moving every frame
    // but the title, so every window has something to show.
    for (const orb of ['a', 'b', 'c', 'd']) add(page, 'div', 'gw-orb gw-orb-' + orb);
    const ring = add(page, 'div', 'gw-ring');
    add(ring, 'div', 'gw-ring-mark');
    add(page, 'div', 'gw-headline', 'AURORA');
    const band = add(page, 'div', 'gw-band');
    const tiles = add(band, 'div', 'gw-tiles');
    const names = ['HP', 'MP', 'ATK', 'DEF', 'SPD', 'LCK', 'INT', 'WIS'];
    for (let i = 0; i < 20; ++i) {
      const tile = add(tiles, 'div', 'gw-tile gw-tile-' + (i % 4));
      add(tile, 'div', 'gw-tile-name', names[i % names.length]);
      add(tile, 'div', 'gw-tile-value', String(10 + (i * 37) % 90));
    }
    const stack = add(page, 'div', 'gw-stack');
    const bar = add(page, 'footer', 'gw-toolbar');
    const less = add(bar, 'div', 'gw-button gw-less', '−');
    const shown = add(bar, 'div', 'gw-count', '');
    const more = add(bar, 'div', 'gw-button gw-more', '+');
    add(bar, 'div', 'gw-hint', 'Drag a title bar · − / + windows');
    nodes = {page, stack, shown, windows: [], at: new Map()};
    setCount(firstWindows);
    less.addEventListener('click', guard(() => setCount(nodes.windows.length - 1)));
    more.addEventListener('click', guard(() => setCount(nodes.windows.length + 1)));
    // The pointer moves over whatever is under it; the page's root sees every move and release.
    root.addEventListener('mousemove', guard(follow));
    root.addEventListener('mouseup', guard(release));
    root.addEventListener('mouseleave', guard(release));
  }
  function unmount() {
    const root = document.getElementById('root');
    while (root.firstChild) root.removeChild(root.firstChild);
    nodes = null;
    drag = null;
    active = null;
  }
  // The windows in the page's order (the last on top): their title and rect in page px.
  function windows() {
    if (!nodes) return [];
    const origin = nodes.page.getBoundingClientRect();
    return [...nodes.stack.children].map(w => {
      const r = w.getBoundingClientRect();
      return {title: w.querySelector('.gw-name').textContent, rect: [r.x - origin.x, r.y - origin.y, r.width, r.height]};
    });
  }
  globalThis.__ceGlassPage = Object.freeze({
    setCount, count: () => nodes ? nodes.windows.length : 0, drags: () => drags, kind: () => kind,
    state: () => JSON.stringify({kind, count: nodes ? nodes.windows.length : 0, drags, windows: windows()})
  });
  globalThis.__ceGallery = Object.freeze({
    groups: () => Object.fromEntries(Object.entries(groups).map(([name, cases]) => [name, cases.map(c => c.label)])),
    mount,
    unmount,
    original() { mount('GLASS_TESTS', 0); },
    state: () => ({active, errors: errors.slice(), bridge: globalThis.__ceGalleryBridge.slice()})
  });
})();
