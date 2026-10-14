// A game HUD's world-anchored nameplates (docs/refactoring-plan.md, step 13): written for the native scene by hand,
// without a framework. Each unit is a nameplate, its name over a health bar, that follows a point the host moves every
// frame. __ceHud lets the host set how many there are and push their positions and health as JSON through script
// evaluation, as an engine's browser bridge does (a script call with a JSON argument).
(() => {
  const groups = {HUD_TESTS: [{label: 'HUD: nameplates pushed as JSON'}]};
  const names = ['Goblin', 'Orc', 'Wolf', 'Bandit', 'Skeleton', 'Spider', 'Wraith', 'Troll'];
  const maxUnits = 4000;
  let active = null, errors = [], nodes = null, applied = 0;
  function record(error) {
    if (errors.length < 64) errors.push((String(error) + (error instanceof Error && error.stack ? '\n' + error.stack : '')).slice(0, 8192));
  }
  const guard = f => (...args) => { try { return f(...args); } catch (e) { record(e); } };
  const add = (parent, tag, cls, text) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    parent.appendChild(e);
    return e;
  };
  function setCount(n) {
    if (!nodes) throw new Error('CE_HUD: not mounted');
    n = Math.max(0, Math.min(maxUnits, Math.floor(n)));
    while (nodes.units.length < n) {
      const i = nodes.units.length;
      const unit = add(nodes.layer, 'div', 'hud-unit');
      add(unit, 'div', 'hud-name', names[i % names.length] + ' ' + (i + 1));
      const bar = add(unit, 'div', 'hud-bar');
      nodes.units.push({unit, fill: add(bar, 'div', 'hud-fill'), hp: -1});
    }
    while (nodes.units.length > n) {
      const u = nodes.units.pop();
      u.unit.parentNode.removeChild(u.unit);
    }
  }
  // The host's frame: [[x, y, hp], ...] for the units in order: where each nameplate's top-left goes in page px, and its
  // health from 0 to 1.
  function apply(json) {
    const values = JSON.parse(json);
    if (!nodes || values.length !== nodes.units.length) throw new Error('CE_HUD: one entry per unit');
    for (let i = 0; i < values.length; ++i) {
      const [x, y, hp] = values[i];
      const u = nodes.units[i];
      u.unit.style.transform = 'translate(' + x + 'px,' + y + 'px)';
      if (hp !== u.hp) {
        u.hp = hp;
        u.fill.style.width = hp * 100 + '%';
      }
    }
    ++applied;
  }
  function mount(group, index) {
    if (!groups[group]?.[index]) throw new Error('CE_GALLERY_CASE: unknown registration');
    unmount();
    errors = [];
    applied = 0;
    active = {group, index, label: groups[group][index].label};
    const root = document.getElementById('root');
    const page = add(root, 'div', 'hud-page');
    nodes = {page, layer: add(page, 'div', 'hud-layer'), units: []};
  }
  function unmount() {
    const root = document.getElementById('root');
    while (root.firstChild) root.removeChild(root.firstChild);
    nodes = null;
    active = null;
  }
  globalThis.__ceHud = Object.freeze({
    setCount: guard(setCount), apply: guard(apply), count: () => nodes ? nodes.units.length : 0, applied: () => applied
  });
  globalThis.__ceGallery = Object.freeze({
    groups: () => Object.fromEntries(Object.entries(groups).map(([name, cases]) => [name, cases.map(c => c.label)])),
    mount,
    unmount,
    original() { mount('HUD_TESTS', 0); },
    state: () => ({active, errors: errors.slice(), bridge: globalThis.__ceGalleryBridge.slice()})
  });
})();
