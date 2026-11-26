(() => {
  const SVG_NS = 'http://www.w3.org/2000/svg';
  const root = document.getElementById('root');

  const h = (parent, tag, cls, text, attrs = {}) => {
    const el = document.createElement(tag);
    if (cls) el.className = cls;
    if (text !== undefined) el.textContent = text;
    for (const [k, v] of Object.entries(attrs)) el.setAttribute(k, String(v));
    if (parent) parent.appendChild(el);
    return el;
  };

  const s = (parent, tag, attrs = {}) => {
    const el = document.createElementNS(SVG_NS, tag);
    for (const [k, v] of Object.entries(attrs)) el.setAttribute(k, String(v));
    if (parent) parent.appendChild(el);
    return el;
  };

  const appendRich = (parent, parts) => {
    for (const part of parts) {
      if (typeof part === 'string') {
        h(parent, 'span', '', part);
      } else {
        h(parent, 'span', part.cls, part.text);
      }
    }
  };

  const addIcon = (parent, kind, color, size = 22) => {
    const svg = s(parent, 'svg', { width: size, height: size, viewBox: '0 0 24 24', fill: 'none' });
    if (kind === 'sword') {
      s(svg, 'path', { d: 'M18.5 3.5L20.5 5.5L10.2 15.8L8.2 13.8L18.5 3.5Z', stroke: color, 'stroke-width': 1.7, fill: 'rgba(251,191,36,0.14)' });
      s(svg, 'path', { d: 'M6.5 12.5L11.5 17.5', stroke: color, 'stroke-width': 1.9, 'stroke-linecap': 'round' });
      s(svg, 'path', { d: 'M8.5 15.5L4.5 19.5', stroke: color, 'stroke-width': 1.9, 'stroke-linecap': 'round' });
    } else if (kind === 'helm') {
      s(svg, 'path', { d: 'M5 13C5 8 8 4.5 12 4.5C16 4.5 19 8 19 13V18.5L15 17V12.5H9V17L5 18.5V13Z', stroke: color, 'stroke-width': 1.6, fill: 'rgba(161,161,170,0.10)' });
      s(svg, 'line', { x1: 12, y1: 4.5, x2: 12, y2: 12.5, stroke: color, 'stroke-width': 1.4 });
    } else if (kind === 'amulet') {
      s(svg, 'path', { d: 'M7.5 4.5C7.5 8 9.5 10.5 12 10.5C14.5 10.5 16.5 8 16.5 4.5', stroke: color, 'stroke-width': 1.5 });
      s(svg, 'polygon', { points: '12,11 16,15.5 12,20 8,15.5', stroke: color, 'stroke-width': 1.6, fill: 'rgba(192,132,252,0.16)' });
    } else if (kind === 'armor') {
      s(svg, 'path', { d: 'M7 5L12 7.5L17 5L19.5 9L17 11V19.5H7V11L4.5 9L7 5Z', stroke: color, 'stroke-width': 1.6, fill: 'rgba(96,165,250,0.12)' });
    } else if (kind === 'ring') {
      s(svg, 'circle', { cx: 12, cy: 14, r: 5.5, stroke: color, 'stroke-width': 1.7 });
      s(svg, 'polygon', { points: '12,5 14.5,8.5 12,10.5 9.5,8.5', stroke: color, 'stroke-width': 1.4, fill: 'rgba(251,191,36,0.20)' });
    } else if (kind === 'shield') {
      s(svg, 'path', { d: 'M12 3.5L18.5 6.2V11.8C18.5 15.8 15.8 18.9 12 20.5C8.2 18.9 5.5 15.8 5.5 11.8V6.2L12 3.5Z', stroke: color, 'stroke-width': 1.6, fill: 'rgba(96,165,250,0.12)' });
      s(svg, 'line', { x1: 12, y1: 7, x2: 12, y2: 16.5, stroke: color, 'stroke-width': 1.3 });
    } else if (kind === 'cloak') {
      s(svg, 'path', { d: 'M8.5 5H15.5L19 19.5L12 17L5 19.5L8.5 5Z', stroke: color, 'stroke-width': 1.6, fill: 'rgba(192,132,252,0.12)' });
    } else if (kind === 'potion') {
      s(svg, 'path', { d: 'M10 4H14V7.5L17.5 13.5C18.2 14.8 17.8 18.5 15.5 19.5H8.5C6.2 18.5 5.8 14.8 6.5 13.5L10 7.5V4Z', stroke: color, 'stroke-width': 1.6, fill: 'rgba(248,113,113,0.18)' });
      s(svg, 'line', { x1: 8, y1: 14.5, x2: 16, y2: 14.5, stroke: color, 'stroke-width': 1.2 });
    } else if (kind === 'scroll') {
      s(svg, 'rect', { x: 6, y: 4.5, width: 12, height: 15, rx: 1.5, stroke: color, 'stroke-width': 1.5, fill: 'rgba(233,221,193,0.09)' });
      s(svg, 'line', { x1: 9, y1: 9, x2: 15, y2: 9, stroke: color, 'stroke-width': 1.3 });
      s(svg, 'line', { x1: 9, y1: 12.5, x2: 15, y2: 12.5, stroke: color, 'stroke-width': 1.3 });
      s(svg, 'line', { x1: 9, y1: 16, x2: 13, y2: 16, stroke: color, 'stroke-width': 1.3 });
    } else if (kind === 'book') {
      s(svg, 'path', { d: 'M5 5.5H11C12 5.5 12 6.5 12 7.5V19C12 18 11 17.5 10 17.5H5V5.5Z', stroke: color, 'stroke-width': 1.5, fill: 'rgba(167,139,250,0.12)' });
      s(svg, 'path', { d: 'M19 5.5H13C12 5.5 12 6.5 12 7.5V19C12 18 13 17.5 14 17.5H19V5.5Z', stroke: color, 'stroke-width': 1.5, fill: 'rgba(167,139,250,0.12)' });
    } else if (kind === 'gem') {
      s(svg, 'polygon', { points: '8,5 16,5 20,10.5 12,19.5 4,10.5', stroke: color, 'stroke-width': 1.6, fill: 'rgba(56,189,248,0.14)' });
      s(svg, 'polyline', { points: '4,10.5 20,10.5', stroke: color, 'stroke-width': 1.2 });
    } else if (kind === 'axe') {
      s(svg, 'line', { x1: 6, y1: 19, x2: 17, y2: 5, stroke: color, 'stroke-width': 1.7, 'stroke-linecap': 'round' });
      s(svg, 'path', { d: 'M12.5 6.5C15.5 5 18.5 6.5 19 10.5C16.5 11.5 14 10.5 12.5 8.5', stroke: color, 'stroke-width': 1.6, fill: 'rgba(251,146,60,0.16)' });
    } else if (kind === 'bow') {
      s(svg, 'path', { d: 'M6.5 18.5C5.5 12.5 8.5 6.5 17.5 5.5', stroke: color, 'stroke-width': 1.7, 'stroke-linecap': 'round' });
      s(svg, 'line', { x1: 6.5, y1: 18.5, x2: 17.5, y2: 5.5, stroke: color, 'stroke-width': 1.1 });
      s(svg, 'line', { x1: 9, y1: 15, x2: 18.5, y2: 9.5, stroke: color, 'stroke-width': 1.4 });
    } else if (kind === 'boots') {
      s(svg, 'path', { d: 'M8 5H13.5V13.5L18 15.5V19H7.5V5H8Z', stroke: color, 'stroke-width': 1.6, fill: 'rgba(161,161,170,0.10)' });
    } else if (kind === 'key') {
      s(svg, 'circle', { cx: 8.5, cy: 12, r: 3.5, stroke: color, 'stroke-width': 1.6 });
      s(svg, 'line', { x1: 12, y1: 12, x2: 20, y2: 12, stroke: color, 'stroke-width': 1.6 });
      s(svg, 'line', { x1: 16.5, y1: 12, x2: 16.5, y2: 15, stroke: color, 'stroke-width': 1.6 });
      s(svg, 'line', { x1: 19.5, y1: 12, x2: 19.5, y2: 15, stroke: color, 'stroke-width': 1.6 });
    } else if (kind === 'flame') {
      s(svg, 'path', { d: 'M12.5 3.5C14.5 7 18 9.2 18 13.8C18 17.4 15.3 20 12 20C8.7 20 6 17.4 6 13.8C6 11.4 7.4 9.5 8.8 8C8.8 10 9.8 11 10.8 11C10.5 8.2 11.2 5.4 12.5 3.5Z', stroke: color, 'stroke-width': 1.7, fill: 'rgba(248,113,113,0.18)' });
    } else if (kind === 'herb') {
      s(svg, 'path', { d: 'M6 19C9 15 13 10 18.5 5.5C17.5 11.5 14 16 8.5 17.5', stroke: color, 'stroke-width': 1.6, fill: 'rgba(52,211,153,0.16)' });
      s(svg, 'line', { x1: 6, y1: 19, x2: 14, y2: 11, stroke: color, 'stroke-width': 1.3 });
    } else {
      s(svg, 'polygon', { points: '12,3.5 19.5,12 12,20.5 4.5,12', stroke: color, 'stroke-width': 1.6, fill: 'rgba(251,191,36,0.14)' });
      s(svg, 'circle', { cx: 12, cy: 12, r: 2.5, stroke: color, 'stroke-width': 1.4 });
    }
    return svg;
  };

  // ==================== 1. TOP BAR ====================
  const topbar = h(root, 'div', 'topbar');
  const brand = h(topbar, 'div', 'brand');
  const mark = h(brand, 'div', 'brand-mark');
  addIcon(mark, 'relic', '#c7a667', 14);
  h(brand, 'span', 'brand-title', 'ASHEN OATH');
  h(brand, 'span', 'brand-sep');
  h(brand, 'span', 'brand-sub', 'ACT II · SANCTUM VAULT');

  const nav = h(topbar, 'div', 'nav-tabs');
  for (const label of ['CHARACTER', 'INVENTORY', 'ABILITIES', 'GRIMOIRE', 'JOURNAL']) {
    const tab = h(nav, 'div', label === 'INVENTORY' ? 'nav-tab active' : 'nav-tab', label);
    if (label === 'INVENTORY') h(tab, 'div', 'nav-tab-bar');
  }

  const topRight = h(topbar, 'div', 'top-right');
  const goldBox = h(topRight, 'div', 'top-metric');
  h(goldBox, 'span', 'top-metric-gold', '2,480');
  h(goldBox, 'span', 'top-metric-dim', 'GP');
  const partyBox = h(topRight, 'div', 'top-metric');
  h(partyBox, 'span', '', 'PARTY');
  h(partyBox, 'span', 'top-metric-dim', '4 / 4');

  // ==================== 2. MAIN WORKSPACE ====================
  const ws = h(root, 'div', 'workspace');

  // ---------- LEFT COLUMN: CHARACTER SUMMARY ----------
  const colL = h(ws, 'div', 'panel col-left');

  const charHead = h(colL, 'div', 'char-head');
  const charMeta = h(charHead, 'div');
  h(charMeta, 'div', 'char-name', 'Seraphina Vane');
  h(charMeta, 'div', 'char-meta', 'Lv. 14 Oathbreaker · Tiefling');
  const acBox = h(charHead, 'div', 'char-ac');
  h(acBox, 'span', 'char-ac-val', '22');
  h(acBox, 'span', 'char-ac-lbl', 'AC');

  // Flat Vital Bars
  const vitals = h(colL, 'div', 'vitals');
  for (const v of [
    { label: 'HEALTH', cur: '412 ', max: '/ 480', cls: 'vital-fill-hp', pct: '86%' },
    { label: 'AETHER', cur: '185 ', max: '/ 220', cls: 'vital-fill-mp', pct: '84%' },
  ]) {
    const row = h(vitals, 'div', 'vital-row');
    const hd = h(row, 'div', 'vital-head');
    h(hd, 'span', 'vital-label', v.label);
    const num = h(hd, 'span', 'vital-num', v.cur);
    h(num, 'span', 'vital-num-dim', v.max);
    const track = h(row, 'div', 'vital-track');
    const fill = h(track, 'div', v.cls);
    fill.style.width = v.pct;
  }

  // CRPGFramework Resource Dot-Grid Pills
  const resPills = h(colL, 'div', 'res-pills');
  for (const r of [
    { lbl: 'AP', cur: 2, max: 3 },
    { lbl: 'ACT', cur: 1, max: 1 },
    { lbl: 'SLOT', cur: 3, max: 4 },
  ]) {
    const pill = h(resPills, 'div', 'res-pill');
    h(pill, 'span', 'res-pill-lbl', r.lbl);
    const dots = h(pill, 'span', 'res-dots');
    for (let i = 0; i < r.max; i++) {
      h(dots, 'span', i < r.cur ? 'res-dot' : 'res-dot-empty');
    }
  }

  // Active Statuses
  const statusSec = h(colL, 'div');
  const stHead = h(statusSec, 'div', 'sec-head');
  h(stHead, 'span', 'sec-title', 'ACTIVE EFFECTS');
  h(stHead, 'span', 'sec-sub', '3');
  const stRow = h(statusSec, 'div', 'status-row');
  for (const st of [
    { bar: 'status-bar-red', name: 'Burn ×2', col: 'c-red', turn: '3 turns' },
    { bar: 'status-bar-emerald', name: 'Fortify', col: 'c-emerald', turn: '2 turns' },
    { bar: 'status-bar-amber', name: 'Oath', col: 'c-amber', turn: '∞' },
  ]) {
    const chip = h(stRow, 'div', 'status-chip');
    h(chip, 'div', 'status-bar ' + st.bar);
    const bd = h(chip, 'div', 'status-body');
    h(bd, 'span', 'status-name ' + st.col, st.name);
    h(bd, 'span', 'status-turn', st.turn);
  }

  // 6 Core Attributes
  const attrSec = h(colL, 'div');
  const atHead = h(attrSec, 'div', 'sec-head');
  h(atHead, 'span', 'sec-title', 'ATTRIBUTES');
  h(atHead, 'span', 'sec-sub', 'PROF +5');
  const attrGrid = h(attrSec, 'div', 'attr-grid');
  for (const a of [
    { k: 'STR', m: '+4', v: '18', hi: true },
    { k: 'DEX', m: '+2', v: '14' },
    { k: 'CON', m: '+3', v: '16' },
    { k: 'INT', m: '+1', v: '12' },
    { k: 'WIS', m: '+2', v: '15' },
    { k: 'CHA', m: '+4', v: '19', hi: true },
  ]) {
    const card = h(attrGrid, 'div', a.hi ? 'attr-card hi' : 'attr-card');
    const top = h(card, 'div', 'attr-top');
    h(top, 'span', 'attr-key', a.k);
    h(top, 'span', 'attr-mod', a.m);
    h(card, 'div', 'attr-val', a.v);
  }

  // Hexagonal Affinity Radar SVG
  const radarBox = h(colL, 'div', 'radar-box');
  const rSvg = s(radarBox, 'svg', { width: 224, height: 138, viewBox: '0 0 224 138' });
  const rcx = 112, rcy = 69, rR = 47;
  const rLabels = ['FIRE', 'PHYS', 'ARC', 'RAD', 'WARD', 'CRIT'];
  const rVals = [0.92, 0.78, 0.56, 0.84, 0.70, 0.88];
  const rPt = (idx, rad) => {
    const ang = (-90 + idx * 60) * Math.PI / 180;
    return [rcx + Math.cos(ang) * rad, rcy + Math.sin(ang) * rad];
  };
  for (const frac of [1.0, 0.66, 0.33]) {
    const pts = rLabels.map((_, i) => rPt(i, rR * frac).map(n => n.toFixed(1)).join(',')).join(' ');
    s(rSvg, 'polygon', { points: pts, fill: 'none', stroke: frac === 1.0 ? '#27272a' : '#1f1f24', 'stroke-width': 1 });
  }
  rLabels.forEach((_, i) => {
    const [x, y] = rPt(i, rR);
    s(rSvg, 'line', { x1: rcx, y1: rcy, x2: x.toFixed(1), y2: y.toFixed(1), stroke: '#27272a', 'stroke-width': 1 });
  });
  const dataPts = rVals.map((v, i) => rPt(i, rR * v).map(n => n.toFixed(1)).join(',')).join(' ');
  s(rSvg, 'polygon', { points: dataPts, fill: 'rgba(199, 166, 103, 0.22)', stroke: '#c7a667', 'stroke-width': 1.5 });
  rVals.forEach((v, i) => {
    const [x, y] = rPt(i, rR * v);
    s(rSvg, 'circle', { cx: x.toFixed(1), cy: y.toFixed(1), r: 2.5, fill: '#c7a667', stroke: '#09090b', 'stroke-width': 1 });
  });
  rLabels.forEach((lb, i) => {
    const [x, y] = rPt(i, rR + 13);
    const t = s(rSvg, 'text', { x: x.toFixed(1), y: (y + 3).toFixed(1), 'text-anchor': 'middle', fill: '#71717a', 'font-size': 8.5, 'font-weight': 700, 'font-family': 'Segoe UI, sans-serif' });
    t.textContent = lb;
  });

  // Resistances Row
  const resGrid = h(colL, 'div', 'res-grid');
  for (const r of [
    { k: 'FIRE', v: '60%', c: 'c-amber' },
    { k: 'RAD', v: '45%', c: 'c-emerald' },
    { k: 'COLD', v: '15%', c: 'c-sky' },
    { k: 'NEC', v: '-10%', c: 'c-red' },
  ]) {
    const it = h(resGrid, 'div', 'res-item');
    h(it, 'span', 'res-item-lbl', r.k);
    h(it, 'span', 'res-item-val ' + r.c, r.v);
  }

  // ---------- CENTER COLUMN: 3D RENDER TEXTURE + COMBAT ROLL LOG ----------
  const colC = h(ws, 'div', 'col-center');

  const stageWrap = h(colC, 'div', 'stage-wrap');
  h(stageWrap, 'div', 'stage-3d');
  h(stageWrap, 'div', 'stage-vignette');

  const stageTop = h(stageWrap, 'div', 'stage-top');
  const rtPill = h(stageTop, 'div', 'rt-pill');
  h(rtPill, 'span', 'rt-dot');
  const rtTxt = h(rtPill, 'span', '', 'RENDER TEXTURE · ');
  h(rtTxt, 'span', 'rt-pill-strong', 'stage3d');
  h(stageTop, 'div', 'stage-relic-tag', 'LEGENDARY +12');

  // Left Equipment Slots
  const eqL = h(stageWrap, 'div', 'equip-col equip-col-left');
  for (const eq of [
    { icon: 'helm', col: '#c084fc', cls: 'eq-slot eq-slot-epic', lbl: 'HEAD' },
    { icon: 'amulet', col: '#fbbf24', cls: 'eq-slot eq-slot-legend', lbl: 'NECK' },
    { icon: 'armor', col: '#60a5fa', cls: 'eq-slot eq-slot-rare', lbl: 'CHEST' },
    { icon: 'ring', col: '#c084fc', cls: 'eq-slot eq-slot-epic', lbl: 'RING' },
  ]) {
    const sl = h(eqL, 'div', eq.cls);
    addIcon(sl, eq.icon, eq.col, 21);
    h(sl, 'span', 'eq-slot-lbl', eq.lbl);
  }

  // Right Equipment Slots (MAIN +12 active legendary!)
  const eqR = h(stageWrap, 'div', 'equip-col equip-col-right');
  for (const eq of [
    { icon: 'sword', col: '#fbbf24', cls: 'eq-slot eq-slot-legend', lbl: 'MAIN', plus: '+12' },
    { icon: 'shield', col: '#60a5fa', cls: 'eq-slot eq-slot-rare', lbl: 'OFF' },
    { icon: 'cloak', col: '#c084fc', cls: 'eq-slot eq-slot-epic', lbl: 'CLOAK' },
    { icon: 'relic', col: '#a1a1aa', cls: 'eq-slot', lbl: 'RELIC' },
  ]) {
    const sl = h(eqR, 'div', eq.cls);
    if (eq.plus) h(sl, 'span', 'eq-plus', eq.plus);
    addIcon(sl, eq.icon, eq.col, eq.plus ? 22 : 21);
    h(sl, 'span', 'eq-slot-lbl', eq.lbl);
  }

  const stageBot = h(stageWrap, 'div', 'stage-bottom');
  const cap = h(stageBot, 'div');
  h(cap, 'div', 'stage-caption-title', 'Flamebrand Sword');
  h(cap, 'div', 'stage-caption-sub', 'Obsidian Greatsword · Live D3D11 RGBA16F Target');
  const camBar = h(stageBot, 'div', 'stage-cam-bar');
  h(camBar, 'span', 'cam-btn active', 'RELIC');
  h(camBar, 'span', 'cam-btn', 'CHAR');
  h(camBar, 'span', 'cam-btn', '360°');

  // Bottom Combat / Dice Log
  const logPanel = h(colC, 'div', 'panel log-panel');
  const logHead = h(logPanel, 'div', 'sec-head');
  logHead.style.marginBottom = '2px';
  h(logHead, 'span', 'sec-title', 'COMBAT LOG · TURN 04');
  h(logHead, 'span', 'sec-sub', 'D20 RULESET');
  const logList = h(logPanel, 'div', 'log-list');

  const logEntries = [
    {
      tag: 'd20(19)+6 = 25', cls: 'roll-tag roll-tag-crit',
      parts: [{ text: 'Flamebrand Sword', cls: 'kw kw-amber' }, ' crits Ash Warden for ', { text: '48 Fire Damage', cls: 'kw kw-orange' }, '.']
    },
    {
      tag: 'SAVE 11 vs 16', cls: 'roll-tag roll-tag-burn',
      parts: ['Ash Warden fails Dex save, gaining ', { text: 'Burn', cls: 'kw kw-red' }, ' (×2 stacks, 3 turns).']
    },
    {
      tag: 'PASSIVE +24%', cls: 'roll-tag roll-tag-ok',
      parts: [{ text: 'Elemental Mastery', cls: 'kw kw-violet' }, ' boosts Burn & ', { text: 'Elemental Reaction', cls: 'kw kw-sky' }, '.']
    },
  ];
  for (const entry of logEntries) {
    const row = h(logList, 'div', 'log-row');
    h(row, 'span', entry.cls, entry.tag);
    const txt = h(row, 'span');
    appendRich(txt, entry.parts);
  }

  // ---------- RIGHT COLUMN: INVENTORY BAG ----------
  const colR = h(ws, 'div', 'panel col-right');
  const bagHead = h(colR, 'div', 'bag-head');
  const bagTW = h(bagHead, 'div', 'bag-title-wrap');
  h(bagTW, 'span', 'bag-title', 'SATCHEL');
  h(bagTW, 'span', 'bag-count', '36 / 56');
  const bagFilters = h(bagHead, 'div', 'bag-filters');
  for (const f of ['ALL', 'GEAR', 'POTIONS', 'RELICS']) {
    h(bagFilters, 'span', f === 'ALL' ? 'bag-pill active' : 'bag-pill', f);
  }

  const bagGrid = h(colR, 'div', 'bag-grid');
  const bagItems = [
    // Row 1
    { icon: 'sword', col: '#fbbf24', rb: 'rb-legend', tag: '+12', tagCls: 'inv-tag-gold', sel: true },
    { icon: 'amulet', col: '#fbbf24', rb: 'rb-legend', tag: 'SET', tagCls: 'inv-tag-gold' },
    { icon: 'potion', col: '#f87171', rb: 'rb-rare', qty: '×6' },
    { icon: 'scroll', col: '#e9ddc1', rb: 'rb-rare', qty: '×3' },
    { icon: 'gem', col: '#38bdf8', rb: 'rb-epic', qty: '×2' },
    { icon: 'book', col: '#c084fc', rb: 'rb-epic', tag: 'IV', tagCls: 'inv-tag-purple' },
    { icon: 'key', col: '#c7a667', rb: 'rb-rare' },
    // Rows 2-4 (satchel slots behind floating tooltips)
    { icon: 'axe', col: '#fb923c', rb: 'rb-epic', tag: '+8', tagCls: 'inv-tag-purple' },
    { icon: 'shield', col: '#60a5fa', rb: 'rb-rare', tag: '+5', tagCls: 'inv-tag-blue' },
    { icon: 'potion', col: '#38bdf8', rb: 'rb-rare', qty: '×4' },
    { icon: 'herb', col: '#34d399', rb: 'rb-uncommon', qty: '×9' },
    { icon: 'ring', col: '#c084fc', rb: 'rb-epic' },
    { icon: 'cloak', col: '#a78bfa', rb: 'rb-rare' },
    { icon: 'boots', col: '#a1a1aa', rb: 'rb-common' },
    { icon: 'bow', col: '#60a5fa', rb: 'rb-rare', tag: '+6', tagCls: 'inv-tag-blue' },
    { icon: 'relic', col: '#fbbf24', rb: 'rb-legend' },
    { icon: 'scroll', col: '#fb923c', rb: 'rb-uncommon', qty: '×5' },
    { icon: 'gem', col: '#f87171', rb: 'rb-epic', qty: '×2' },
    null, null, null,
    { icon: 'armor', col: '#60a5fa', rb: 'rb-rare', tag: '+7', tagCls: 'inv-tag-blue' },
    { icon: 'helm', col: '#c084fc', rb: 'rb-epic', tag: '+9', tagCls: 'inv-tag-purple' },
    { icon: 'potion', col: '#34d399', rb: 'rb-uncommon', qty: '×3' },
    null, null, null, null,
    // Row 5 (100% visible below Tooltip 3!)
    { icon: 'flame', col: '#fb923c', rb: 'rb-legend', tag: 'RUNE', tagCls: 'inv-tag-gold', qty: '×2' },
    { icon: 'amulet', col: '#c084fc', rb: 'rb-epic', tag: '+9', tagCls: 'inv-tag-purple' },
    { icon: 'potion', col: '#f87171', rb: 'rb-rare', qty: '×5' },
    { icon: 'potion', col: '#38bdf8', rb: 'rb-rare', qty: '×4' },
    { icon: 'scroll', col: '#e9ddc1', rb: 'rb-uncommon', qty: '×7' },
    { icon: 'gem', col: '#a78bfa', rb: 'rb-epic', qty: '×3' },
    { icon: 'key', col: '#fbbf24', rb: 'rb-legend', tag: 'VAULT', tagCls: 'inv-tag-gold' },
    // Row 6 (100% visible!)
    { icon: 'sword', col: '#60a5fa', rb: 'rb-rare', tag: '+6', tagCls: 'inv-tag-blue' },
    { icon: 'shield', col: '#c084fc', rb: 'rb-epic', tag: '+10', tagCls: 'inv-tag-purple' },
    { icon: 'ring', col: '#34d399', rb: 'rb-uncommon' },
    { icon: 'book', col: '#fbbf24', rb: 'rb-legend', tag: 'V', tagCls: 'inv-tag-gold' },
    { icon: 'herb', col: '#34d399', rb: 'rb-uncommon', qty: '×12' },
    { icon: 'boots', col: '#60a5fa', rb: 'rb-rare', tag: '+4', tagCls: 'inv-tag-blue' },
    { icon: 'relic', col: '#38bdf8', rb: 'rb-rare' },
    // Row 7 (100% visible!)
    { icon: 'axe', col: '#a1a1aa', rb: 'rb-common' },
    { icon: 'cloak', col: '#60a5fa', rb: 'rb-rare' },
    { icon: 'gem', col: '#fbbf24', rb: 'rb-legend', qty: '×1' },
    { icon: 'scroll', col: '#c084fc', rb: 'rb-epic', qty: '×2' },
    { icon: 'potion', col: '#fb923c', rb: 'rb-rare', qty: '×3' },
    { icon: 'ring', col: '#60a5fa', rb: 'rb-rare' },
    null,
    // Row 8 (100% visible — populated + clean empty slots showing satchel headroom)
    { icon: 'bow', col: '#34d399', rb: 'rb-uncommon' },
    { icon: 'key', col: '#a1a1aa', rb: 'rb-common' },
    null, null, null, null, null,
  ];
  for (const item of bagItems) {
    if (!item) {
      h(bagGrid, 'div', 'inv-slot inv-slot-empty');
      continue;
    }
    const sl = h(bagGrid, 'div', item.sel ? 'inv-slot inv-slot-sel' : 'inv-slot');
    if (item.tag) h(sl, 'span', 'inv-tag ' + (item.tagCls || ''), item.tag);
    addIcon(sl, item.icon, item.col, 24);
    if (item.qty) h(sl, 'span', 'inv-qty', item.qty);
    h(sl, 'div', 'inv-rarity-bar ' + item.rb);
  }

  const bagFoot = h(colR, 'div', 'bag-foot');
  const wWrap = h(bagFoot, 'div', 'weight-wrap');
  h(wWrap, 'span', '', 'WEIGHT');
  const wTrack = h(wWrap, 'div', 'weight-track');
  h(wTrack, 'div', 'weight-fill');
  h(wWrap, 'span', '', '68 / 120 KG');
  const bActs = h(bagFoot, 'div', 'bag-actions');
  h(bActs, 'span', 'bag-btn', 'SORT: RARITY');
  h(bActs, 'span', 'bag-btn', 'COMPARE');

  // ==================== 3. CRPGFRAMEWORK 3-LEVEL NESTED TOOLTIP CASCADE ====================
  const ttLayer = h(root, 'div', 'tt-layer');

  // Clean hairline connector SVG
  const conn = s(ttLayer, 'svg', { class: 'conn-svg', width: 1280, height: 720, viewBox: '0 0 1280 720' });
  // Subtle guide from MAIN +12 equipment slot (right=742, y=134) -> EquipmentTooltip (left=754)
  s(conn, 'path', { d: 'M 742 134 L 754 134', fill: 'none', stroke: 'rgba(251, 191, 36, 0.72)', 'stroke-width': 1.4 });
  s(conn, 'circle', { cx: 742, cy: 134, r: 2.5, fill: '#fbbf24' });
  s(conn, 'circle', { cx: 754, cy: 134, r: 2.5, fill: '#fbbf24' });
  // Subtle guide between EquipmentTooltip (right=998) and BuffTooltip (left=1012)
  s(conn, 'path', { d: 'M 998 200 L 1005 200 L 1005 180 L 1012 180', fill: 'none', stroke: 'rgba(248, 113, 113, 0.62)', 'stroke-width': 1.2 });
  s(conn, 'circle', { cx: 998, cy: 200, r: 2.5, fill: '#f87171' });
  s(conn, 'circle', { cx: 1012, cy: 180, r: 2.5, fill: '#f87171' });
  // Subtle guide between BuffTooltip bottom (~318) and TermTooltip top (344)
  s(conn, 'path', { d: 'M 1072 316 L 1072 344', fill: 'none', stroke: 'rgba(167, 139, 250, 0.68)', 'stroke-width': 1.2 });
  s(conn, 'circle', { cx: 1072, cy: 316, r: 2.5, fill: '#a78bfa' });
  s(conn, 'circle', { cx: 1072, cy: 344, r: 2.5, fill: '#a78bfa' });

  // LEVEL 1: EquipmentTooltip (Pinned, border-amber-400 + SmokeBg top mist + stats)
  const tt1 = h(ttLayer, 'div', 'tt-equip');
  h(tt1, 'div', 'tt-smoke-amber');
  const tt1In = h(tt1, 'div', 'tt-inner');
  const pin1 = h(tt1In, 'div', 'tt-pin-ctrl');
  h(pin1, 'span', 'tt-pin-badge', 'PINNED');
  h(pin1, 'span', 'tt-close', '×');

  const eqHead = h(tt1In, 'div', 'tt-eq-head');
  const eqIco = h(eqHead, 'div', 'tt-eq-icon');
  addIcon(eqIco, 'sword', '#fbbf24', 18);
  const eqTitles = h(eqHead, 'div');
  h(eqTitles, 'div', 'tt-title-amber', 'Flamebrand Sword');
  h(eqTitles, 'div', 'tt-rarity', 'LEGENDARY · GREATSWORD');

  h(tt1In, 'div', 'tt-rule-amber');
  const eqDesc = h(tt1In, 'div', 'tt-desc');
  appendRich(eqDesc, [
    'Deals ', { text: 'Fire Damage', cls: 'kw kw-orange' },
    ' and has a 30% chance to inflict ', { text: 'Burn', cls: 'kw kw-active-red' },
    '. Part of the ', { text: 'Ember Amulet', cls: 'kw kw-amber' }, ' set.'
  ]);

  const eqStats = h(tt1In, 'div', 'tt-stats');
  for (const st of [
    { k: 'Attack', v: '28–42 (+12)', cls: 'tt-stat-v' },
    { k: 'Crit Chance', v: '+14%', cls: 'tt-stat-pos' },
    { k: 'Fire Bonus', v: '+24%', cls: 'tt-stat-pos' },
  ]) {
    const r = h(eqStats, 'div', 'tt-stat-row');
    h(r, 'span', 'tt-stat-k', st.k);
    h(r, 'span', st.cls, st.v);
  }

  // LEVEL 2: BuffTooltip (Pinned from Burn, 4px left accent bar + lifetime badge + tone modifiers)
  const tt2 = h(ttLayer, 'div', 'tt-buff');
  h(tt2, 'div', 'tt-buff-bar');
  const tt2Body = h(tt2, 'div', 'tt-buff-body');
  const pin2 = h(tt2Body, 'div', 'tt-pin-ctrl');
  h(pin2, 'span', 'tt-close', '×');

  const bHead = h(tt2Body, 'div', 'tt-buff-head');
  const bTW = h(bHead, 'div', 'tt-buff-title-wrap');
  addIcon(bTW, 'flame', '#f87171', 15);
  h(bTW, 'span', 'tt-title-red', 'Burn ×2');
  h(bHead, 'span', 'tt-life', '3 TURNS');

  h(tt2Body, 'div', 'tt-rule-red');
  const bDesc = h(tt2Body, 'div', 'tt-desc');
  appendRich(bDesc, [
    'Deals ', { text: 'Fire Damage', cls: 'kw kw-orange' },
    ' each turn. Scales with ', { text: 'Elemental Mastery', cls: 'kw kw-active-violet' },
    '; reduced by ', { text: 'Fortify', cls: 'kw kw-emerald' }, '.'
  ]);

  const bMods = h(tt2Body, 'div', 'tt-stats');
  for (const m of [
    { k: 'Fire Resistance', v: '-15%', cls: 'tt-stat-neg' },
    { k: 'Armor Class', v: '-3', cls: 'tt-stat-neg' },
    { k: 'Damage / Turn', v: '24', cls: 'tt-stat-v' },
    { k: 'Crit Vulnerability', v: '+8%', cls: 'tt-stat-pos' },
  ]) {
    const r = h(bMods, 'div', 'tt-stat-row');
    h(r, 'span', 'tt-stat-k', m.k);
    h(r, 'span', m.cls, m.v);
  }

  // LEVEL 3: TermTooltip (Active hover from Elemental Mastery + signature 20x20 SVG PinRing!)
  const tt3 = h(ttLayer, 'div', 'tt-term');
  const prWrap = h(tt3, 'div', 'tt-pinring-wrap');
  h(prWrap, 'span', 'tt-pinring-lbl', 'HOLD TO PIN');
  const prSvg = s(prWrap, 'svg', { width: 20, height: 20, viewBox: '0 0 20 20' });
  s(prSvg, 'circle', { cx: 10, cy: 10, r: 7, fill: 'none', stroke: '#3f3f46', 'stroke-width': 2 });
  // Crisp 72% clockwise progress arc starting at 12 o'clock (10,3) -> (3.13, 8.69)
  s(prSvg, 'path', {
    d: 'M 10 3 A 7 7 0 1 1 3.13 8.69',
    fill: 'none', stroke: '#e4e4e7', 'stroke-width': 2.2, 'stroke-linecap': 'round'
  });
  s(prSvg, 'circle', { cx: 10, cy: 10, r: 2.2, fill: '#a78bfa' });

  h(tt3, 'div', 'tt-title-violet', 'Elemental Mastery');
  h(tt3, 'div', 'tt-rule-zinc');
  const tDesc = h(tt3, 'div', 'tt-desc');
  appendRich(tDesc, [
    'Amplifies elemental status potency and triggers ',
    { text: 'Elemental Reaction', cls: 'kw kw-sky' },
    ' chains (+1.5% per point).'
  ]);
})();
