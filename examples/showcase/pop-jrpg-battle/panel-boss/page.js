(() => {
  const root = document.getElementById('root');
  const h = (p, t, c, txt) => {
    const el = document.createElement(t);
    if (c) el.className = c;
    if (txt !== undefined) el.textContent = txt;
    p.appendChild(el);
    return el;
  };
  const hud = h(root, 'div', 'boss-hud');
  h(hud, 'div', 'boss-backing');
  const card = h(hud, 'div', 'boss-card');
  const head = h(card, 'div', 'boss-head');
  const nw = h(head, 'div', 'boss-name-wrap');
  h(nw, 'div', 'boss-name', 'PHANTOM ARCHON');
  h(nw, 'div', 'boss-jp', '虚空の剣王');
  h(head, 'div', 'boss-lv', 'LV.50');

  const hp = h(card, 'div', 'boss-hp-wrap');
  h(hp, 'div', 'boss-hp-ghost');
  h(hp, 'div', 'boss-hp-fill');
  h(hp, 'div', 'boss-hp-stripes');
  h(hp, 'div', 'boss-hp-txt', '4,420 / 9,600');

  const row = h(card, 'div', 'aff-row');
  for (const a of [
    { elem: 'FIRE', res: 'WEAK!', cls: 'weak' },
    { elem: 'ICE', res: 'NULL', cls: 'null' },
    { elem: 'ELEC', res: '—', cls: '' },
    { elem: 'WIND', res: '—', cls: '' },
    { elem: 'CURSE', res: 'DRAIN', cls: 'drain' },
  ]) {
    const chip = h(row, 'div', 'aff-chip ' + a.cls);
    h(chip, 'div', 'aff-elem', a.elem);
    h(chip, 'div', 'aff-res', a.res);
  }
})();
