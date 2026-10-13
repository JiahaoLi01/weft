// The gallery's UI particle page (docs/refactoring-plan.md, step 11.4c): written for the native scene by hand, without
// a framework, on the scene platform's weft.emitParticles and the -weft-particles property. The host registers the
// particles it names (probes/gallery_particles.hpp): a campfire's flames, embers and smoke and a rune's aura stream
// from CSS, motes drift over the page at a density the page sets, and a chest throws coins to the gold counter with
// sparks, every 2.5 s by itself and on a click. __ceParticlePage lets probes set the density and open the chest.
(() => {
  const groups = {PARTICLE_TESTS: [{label: 'UI Particles: Campfire, Loot, Rune, Motes'}]};
  const autoLootMs = 2500, coinsPerLoot = 24, coinFlightMs = 1450;
  let active = null, errors = [], timers = [], nodes = null, gold = 0, density = 300, clicks = 0;
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
  // The motes' rate: each layer holds at most 12000 per second (its particles live up to 5 s; an emission holds 65536).
  const layerRate = 12000;
  function setDensity(rate) {
    density = Math.max(0, Math.round(rate));
    if (!nodes) return;
    nodes.motes.forEach((layer, i) => {
      const share = Math.min(layerRate, Math.max(0, density - i * layerRate));
      layer.setAttribute('style', '--particles-rate:' + share);
    });
    nodes.density.textContent = 'Motes ' + density + ' / s';
  }
  function loot() {
    if (!nodes) return;
    weft.emitParticles(nodes.chest, 'sparks', {count: 40, duration: .12});
    weft.emitParticles(nodes.chest, 'coins', {count: coinsPerLoot, duration: .35, target: nodes.coin});
    timers.push(setTimeout(guard(() => {
      gold += coinsPerLoot;
      if (nodes) nodes.count.textContent = String(gold);
    }), coinFlightMs));
  }
  function autoLoot() {
    timers.push(setTimeout(guard(() => { loot(); autoLoot(); }), autoLootMs));
  }
  function card(row, title, hint) {
    const c = add(row, 'section', 'pp-card');
    add(c, 'div', 'pp-card-title', title);
    const stage = add(c, 'div', 'pp-stage');
    add(c, 'div', 'pp-hint', hint);
    return stage;
  }
  function mount(group, index) {
    if (!groups[group]?.[index]) throw new Error('CE_GALLERY_CASE: unknown registration');
    unmount();
    errors = [];
    gold = 0;
    clicks = 0;
    active = {group, index, label: groups[group][index].label};
    const root = document.getElementById('root');
    const page = add(root, 'div', 'pp-page');
    const motes = [0, 1, 2, 3].map(() => add(page, 'div', 'pp-motes'));
    const bar = add(page, 'header', 'pp-bar');
    add(bar, 'div', 'pp-title', 'UI PARTICLES');
    const purse = add(bar, 'div', 'pp-gold');
    const coin = add(purse, 'div', 'pp-coin');
    const count = add(purse, 'div', 'pp-count', '0');
    const row = add(page, 'main', 'pp-row');
    const camp = card(row, 'Campfire', 'CSS: -weft-particles: flames / embers / smoke');
    add(camp, 'div', 'pp-smoke');
    add(camp, 'div', 'pp-fire');
    add(camp, 'div', 'pp-embers');
    add(camp, 'div', 'pp-logs');
    const loot_stage = card(row, 'Loot', 'Click: weft.emitParticles(chest, "coins", {target: gold})');
    const chest = add(loot_stage, 'div', 'pp-chest');
    add(chest, 'div', 'pp-lid');
    add(chest, 'div', 'pp-lock');
    const rune_stage = card(row, 'Rune', 'Hover: --particles-rate 60 → 240');
    const rune = add(rune_stage, 'div', 'pp-rune');
    add(rune, 'div', 'pp-rune-core');
    const footer = add(page, 'footer', 'pp-bar');
    const less = add(footer, 'div', 'pp-button', '−');
    const shown = add(footer, 'div', 'pp-density', '');
    const more = add(footer, 'div', 'pp-button', '+');
    nodes = {motes, chest, coin, count, density: shown};
    setDensity(density);
    chest.addEventListener('click', guard(() => { ++clicks; loot(); }));
    less.addEventListener('click', guard(() => setDensity(density / 2)));
    more.addEventListener('click', guard(() => setDensity(Math.max(300, density * 2))));
    autoLoot();
  }
  function unmount() {
    for (const t of timers) clearTimeout(t);
    timers = [];
    const root = document.getElementById('root');
    while (root.firstChild) root.removeChild(root.firstChild);
    nodes = null;
    active = null;
  }
  globalThis.__ceParticlePage = Object.freeze({setDensity, loot, density: () => density, gold: () => gold, clicks: () => clicks});
  globalThis.__ceGallery = Object.freeze({
    groups: () => Object.fromEntries(Object.entries(groups).map(([name, cases]) => [name, cases.map(c => c.label)])),
    mount,
    unmount,
    original() { mount('PARTICLE_TESTS', 0); },
    state: () => ({active, errors: errors.slice(), bridge: globalThis.__ceGalleryBridge.slice()})
  });
})();
