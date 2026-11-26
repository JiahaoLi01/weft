(() => {
  const SVG_NS = 'http://www.w3.org/2000/svg';
  const root = document.getElementById('root');
  const h = (p, t, c, txt, a = {}) => {
    const el = document.createElement(t);
    if (c) el.className = c;
    if (txt !== undefined) el.textContent = txt;
    for (const [k, v] of Object.entries(a)) el.setAttribute(k, String(v));
    p.appendChild(el);
    return el;
  };
  const s = (p, t, a = {}) => {
    const el = document.createElementNS(SVG_NS, t);
    for (const [k, v] of Object.entries(a)) el.setAttribute(k, String(v));
    p.appendChild(el);
    return el;
  };

  const cmdMenu = h(root, 'div', 'cmd-menu');
  for (const c of [
    { key: 'X', lbl: 'ATTACK', cls: 'c1' },
    { key: 'Y', lbl: 'SKILL', cls: 'c2' },
    { key: 'B', lbl: 'GUARD', cls: 'c3' },
    { key: 'A', lbl: 'ITEM', cls: 'c4' },
    { key: 'LT', lbl: 'BURST', cls: 'c5' },
  ]) {
    const btn = h(cmdMenu, 'div', 'cmd-btn ' + c.cls);
    h(btn, 'div', 'cmd-key', c.key);
    h(btn, 'div', 'cmd-lbl', c.lbl);
  }
  const arr = s(cmdMenu, 'svg', { class: 'cmd-arrow', width: 26, height: 26, viewBox: '0 0 26 26' });
  s(arr, 'polygon', { points: '2,2 24,13 2,24 8,13', fill: '#ffe600', stroke: '#0a0a0e', 'stroke-width': 2.6 });

  const drawer = h(root, 'div', 'skill-drawer');
  h(drawer, 'div', 'skill-shadow');
  const sbox = h(drawer, 'div', 'skill-box');
  const shead = h(sbox, 'div', 'skill-head');
  h(shead, 'div', 'skill-head-title', 'PERSONA SKILLS');
  h(shead, 'div', 'skill-head-sp', 'SP 168 / 210');

  const drawIcon = (parent, kind) => {
    const svg = s(parent, 'svg', { width: 18, height: 18, viewBox: '0 0 18 18' });
    if (kind === 'fire') {
      s(svg, 'path', { d: 'M9 1 C12 5 15 7.5 14.5 11.5 C14 14.8 11.8 17 9 17 C6.2 17 3.8 14.8 3.5 11.5 C3.2 8.5 5.5 6.8 6.8 5.2 C6.8 7 7.6 7.5 8 7.5 C7.5 5 8.2 2.8 9 1 Z', fill: '#e60023', stroke: '#0a0a0e', 'stroke-width': 1.2 });
    } else if (kind === 'elec') {
      s(svg, 'polygon', { points: '10,1 3,10 8.5,10 7,17 15,8 9.5,8', fill: '#00e5ff', stroke: '#0a0a0e', 'stroke-width': 1 });
    } else {
      s(svg, 'polygon', { points: '9,1 16,6 13,16 5,16 2,6', fill: '#f472ff', stroke: '#0a0a0e', 'stroke-width': 1.1 });
    }
  };

  for (const sk of [
    { jp: '紅蓮裂空斬', en: 'CRIMSON REND · FIRE', sp: '18 SP', icon: 'fire', sel: true },
    { jp: '蒼雷の楔', en: 'VOLT LANCE · ELEC', sp: '14 SP', icon: 'elec', sel: false },
    { jp: '修羅の舞', en: 'WAR DANCE · ATK UP', sp: '22 SP', icon: 'buff', sel: false },
  ]) {
    const row = h(sbox, 'div', 'skill-item' + (sk.sel ? ' selected' : ''));
    const left = h(row, 'div', 'skill-left');
    drawIcon(left, sk.icon);
    const txt = h(left, 'div', '');
    h(txt, 'div', 'skill-name', sk.jp);
    h(txt, 'div', 'skill-sub', sk.en);
    h(row, 'div', 'skill-cost', sk.sp);
  }
  h(sbox, 'div', 'skill-foot', 'Heavy Fire DMG to 1 foe · Triggers WEAK');
})();
