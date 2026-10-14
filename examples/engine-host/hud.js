// A game HUD an engine drives through weft.engine (docs/engine-communication.md): nameplates that follow the anchors
// the engine writes (data-anchor), their health from the view model "health" (unit id to a fraction), damage numbers
// from hud.damage events, and an end-turn button that calls the engine. Written without a framework, as plain DOM.
// The engine host sample and the View tests run it.
(() => {
  const engine = weft.engine;
  const add = (parent, tag, cls, text) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    parent.appendChild(e);
    return e;
  };
  const root = document.getElementById('root');
  const layer = add(root, 'div', 'hud-layer');
  // Unit id to its nameplate and health fill.
  const units = new Map();
  // Health as a scale of the fill: no layout.
  const health = engine.model('health');
  const show = (u, hp) => {
    if (hp !== undefined) u.fill.style.transform = 'scaleX(' + Math.max(0, Math.min(1, Number(hp))) + ')';
  };
  // A unit's nameplate stands on its anchor (the unit's id), scaled with it, hidden off screen. A delivery hands the
  // page the view model before the events, so a new nameplate starts from the model's health.
  engine.on('hud.spawn', ({id, name}) => {
    if (units.has(id)) return;
    const unit = add(layer, 'div', 'hud-unit');
    unit.setAttribute('data-anchor', String(id));
    unit.setAttribute('data-anchor-pivot', '50% 100%');
    add(unit, 'div', 'hud-name', String(name));
    const bar = add(unit, 'div', 'hud-bar');
    const u = {unit, fill: add(bar, 'div', 'hud-fill')};
    units.set(id, u);
    show(u, health.value?.[id]);
  });
  engine.on('hud.despawn', ({id}) => {
    const u = units.get(id);
    if (!u) return;
    u.unit.parentNode.removeChild(u.unit);
    units.delete(id);
  });
  // A damage number rises from the unit's anchor for a second.
  engine.on('hud.damage', ({id, amount}) => {
    if (!units.has(id)) return;
    const number = add(layer, 'div', 'hud-damage', String(amount));
    number.setAttribute('data-anchor', String(id));
    number.setAttribute('data-anchor-pivot', '50% 100%');
    number.setAttribute('data-anchor-scale', 'off');
    setTimeout(() => number.parentNode && number.parentNode.removeChild(number), 1000);
  });
  health.subscribe((value, changed) => {
    for (const [id, hp] of Object.entries(changed)) {
      const u = units.get(Number(id));
      if (u) show(u, hp);
    }
  });
  const panel = add(root, 'div', 'hud-panel');
  let title = 'HUD';
  try { title = engine.invoke('loc.get', {key: 'hud.title'}); } catch (_) {}
  add(panel, 'div', 'hud-title', title);
  let turn = 1;
  const turnLabel = add(panel, 'div', 'hud-turn', 'Turn 1');
  const end = add(panel, 'button', 'hud-end', 'End turn');
  end.addEventListener('click', () => {
    engine.call('hud.endTurn', {turn}).then(
      result => { turn = result.turn; turnLabel.textContent = 'Turn ' + turn; },
      error => { turnLabel.textContent = 'Error: ' + error.message; });
  });
  globalThis.__ceHudUnits = () => units.size;
  engine.send('hud.ready', {title});
})();
