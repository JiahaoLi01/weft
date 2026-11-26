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

  // 1. Subtle Edge Vignette over Live D3D11 3D Whitebox Arena
  h(root, 'div', 'vignette');
  h(root, 'div', 'scan-tint');

  // 2. Top Tactical Compass
  const compassWrap = h(root, 'div', 'compass-wrap');
  const track = h(compassWrap, 'div', 'compass-track');
  const marks = ['330', '345', 'N', '015', '030', 'NE', '060', '075', 'E'];
  for (const m of marks) {
    h(track, 'span', /^[A-Z]+$/.test(m) ? 'compass-cardinal' : '', m);
  }
  h(compassWrap, 'div', 'compass-caret', '024 NNE');

  // 3. Top Center Match Scoreboard
  const board = h(root, 'div', 'scoreboard');
  const allyStrip = h(board, 'div', 'team-strip');
  const allies = [
    { alive: true, ult: true },
    { alive: true, ult: false },
    { alive: true, ult: true },
    { alive: true, ult: false },
    { alive: false, ult: false },
  ];
  for (const a of allies) {
    const p = h(allyStrip, 'div', 'agent-pip ally' + (a.alive ? '' : ' dead'));
    h(p, 'div', 'pip-ult' + (a.ult ? ' ready' : ''));
    h(p, 'div', 'pip-bar ' + (a.alive ? 'ally' : 'dead'));
  }

  const core = h(board, 'div', 'score-core');
  const allyScore = h(core, 'div', 'score-box ally');
  h(allyScore, 'div', 'score-num ally', '09');
  h(allyScore, 'div', 'score-side', 'ATK');

  const timer = h(core, 'div', 'timer-box');
  h(timer, 'div', 'timer-val', '01:24');
  h(timer, 'div', 'timer-round', 'ROUND 17');

  const enemyScore = h(core, 'div', 'score-box enemy');
  h(enemyScore, 'div', 'score-num enemy', '07');
  h(enemyScore, 'div', 'score-side', 'DEF');

  const enemyStrip = h(board, 'div', 'team-strip');
  const enemies = [
    { alive: true, ult: true },
    { alive: true, ult: false },
    { alive: true, ult: false },
    { alive: false, ult: false },
    { alive: false, ult: false },
  ];
  for (const e of enemies) {
    const p = h(enemyStrip, 'div', 'agent-pip enemy' + (e.alive ? '' : ' dead'));
    h(p, 'div', 'pip-ult' + (e.ult ? ' ready' : ''));
    h(p, 'div', 'pip-bar ' + (e.alive ? 'enemy' : 'dead'));
  }

  // 4. Top-Left Tactical Radar Minimap (pure SVG architecture & blips)
  const radarShell = h(root, 'div', 'radar-shell');
  const radarFrame = h(radarShell, 'div', 'radar-frame');
  const radarInner = h(radarFrame, 'div', 'radar-inner');
  h(radarInner, 'div', 'radar-grid');
  h(radarInner, 'div', 'radar-grid-v');

  const radarSvg = s(radarInner, 'svg', { class: 'radar-svg', width: 212, height: 194, viewBox: '0 0 212 194' });
  // Radar range rings
  s(radarSvg, 'circle', { cx: 106, cy: 112, r: 42, fill: 'none', stroke: 'rgba(0,240,255,0.16)', 'stroke-width': 1 });
  s(radarSvg, 'circle', { cx: 106, cy: 112, r: 78, fill: 'none', stroke: 'rgba(0,240,255,0.12)', 'stroke-width': 1, 'stroke-dasharray': '4 4' });
  // Architectural skybridge & sector footprints
  s(radarSvg, 'polygon', {
    points: '76,18 136,18 144,182 68,182',
    fill: 'rgba(130,195,225,0.14)',
    stroke: 'rgba(165,225,250,0.45)',
    'stroke-width': 1.4,
  });
  s(radarSvg, 'rect', { x: 18, y: 46, width: 44, height: 68, fill: 'rgba(120,170,200,0.1)', stroke: 'rgba(165,225,250,0.32)', 'stroke-width': 1.2 });
  s(radarSvg, 'rect', { x: 152, y: 34, width: 42, height: 84, fill: 'rgba(120,170,200,0.1)', stroke: 'rgba(165,225,250,0.32)', 'stroke-width': 1.2 });
  // Cover barricades along corridor
  s(radarSvg, 'rect', { x: 82, y: 84, width: 10, height: 26, fill: 'rgba(0,240,255,0.28)', stroke: '#00f0ff', 'stroke-width': 1 });
  s(radarSvg, 'rect', { x: 118, y: 66, width: 12, height: 22, fill: 'rgba(0,240,255,0.28)', stroke: '#00f0ff', 'stroke-width': 1 });
  s(radarSvg, 'rect', { x: 118, y: 120, width: 10, height: 28, fill: 'rgba(180,220,240,0.25)' });
  // Objective zone A ring
  s(radarSvg, 'circle', { cx: 106, cy: 40, r: 16, fill: 'rgba(255,184,48,0.16)', stroke: '#ffb830', 'stroke-width': 1.4, 'stroke-dasharray': '3 2' });
  const zoneText = s(radarSvg, 'text', { x: 106, y: 44, 'text-anchor': 'middle', fill: '#ffb830', 'font-size': 11, 'font-weight': 700 });
  zoneText.textContent = 'A';
  // Player FOV vision cone
  s(radarSvg, 'polygon', { points: '106,112 72,44 140,44', fill: 'rgba(0,240,255,0.16)' });
  s(radarSvg, 'line', { x1: 106, y1: 112, x2: 72, y2: 44, stroke: 'rgba(0,240,255,0.4)', 'stroke-width': 1 });
  s(radarSvg, 'line', { x1: 106, y1: 112, x2: 140, y2: 44, stroke: 'rgba(0,240,255,0.4)', 'stroke-width': 1 });
  // Enemy spotted blips + ping ring
  s(radarSvg, 'circle', { cx: 122, cy: 62, r: 11, fill: 'none', stroke: 'rgba(255,59,82,0.55)', 'stroke-width': 1.2 });
  s(radarSvg, 'circle', { cx: 122, cy: 62, r: 4.5, fill: '#ff3b52', stroke: '#ffffff', 'stroke-width': 1.2 });
  s(radarSvg, 'circle', { cx: 166, cy: 46, r: 4, fill: '#ff3b52', stroke: '#ffffff', 'stroke-width': 1 });
  // Ally chevrons
  s(radarSvg, 'polygon', { points: '90,148 95,158 90,155 85,158', fill: '#00f0ff' });
  s(radarSvg, 'polygon', { points: '44,88 52,84 49,92', fill: '#00f0ff' });
  s(radarSvg, 'polygon', { points: '126,158 131,168 126,165 121,168', fill: '#00f0ff' });
  // Player self arrow
  s(radarSvg, 'polygon', { points: '106,102 113,117 106,113 99,117', fill: '#ffffff', stroke: '#00f0ff', 'stroke-width': 1.4 });

  h(radarInner, 'div', 'radar-tag-tl', 'RADAR // LIVE');
  h(radarInner, 'div', 'radar-tag-br', 'Z-04 · 1:250');

  const sectorBar = h(radarShell, 'div', 'sector-bar');
  const secLeft = h(sectorBar, 'div', 'sector-name', 'SECTOR 04');
  h(secLeft, 'span', 'sector-cn', '· 灰盒演训场');
  h(sectorBar, 'div', 'sector-badge', 'SITE A');

  // 5. Top-Right Telemetry & Kill Feed
  const topRight = h(root, 'div', 'top-right');
  const tele = h(topRight, 'div', 'telemetry-strip');
  h(tele, 'div', 'tele-dot');
  const t1 = h(tele, 'span', '', 'PING ');
  h(t1, 'span', 'tele-hi', '14ms');
  const t2 = h(tele, 'span', '', 'LOSS ');
  h(t2, 'span', 'tele-hi', '0%');
  const t3 = h(tele, 'span', '', 'TICK ');
  h(t3, 'span', 'tele-hi', '128Hz');

  const weaponMiniSvg = (parent, kind) => {
    const svg = s(parent, 'svg', { width: 38, height: 14, viewBox: '0 0 38 14' });
    if (kind === 'rifle') {
      s(svg, 'path', { d: 'M2 7 L10 5 L28 5 L36 6 L36 8 L26 8 L24 12 L21 12 L22 8 L13 8 L10 11 L7 11 L9 8 L2 9 Z', fill: '#e6f3fc' });
    } else if (kind === 'sniper') {
      s(svg, 'path', { d: 'M1 8 L9 6 L16 6 L16 3 L23 3 L23 6 L37 6.5 L37 8 L21 8 L18 12 L15 12 L17 8 L8 9 L2 11 Z', fill: '#e6f3fc' });
    } else {
      s(svg, 'path', { d: 'M4 11 L15 5 L34 3 L22 10 L11 11 L6 13 Z', fill: '#00f0ff' });
    }
  };

  const kills = [
    { killer: 'KESTREL', victim: 'VEX_99', weapon: 'rifle', hs: true, own: true },
    { killer: 'SORA.EXE', victim: 'NIGHTFALL', weapon: 'sniper', hs: true, own: false },
    { killer: 'RONIN', victim: 'ASHEN_04', weapon: 'blade', hs: false, own: false },
  ];
  for (const k of kills) {
    const kc = h(topRight, 'div', 'kill-card' + (k.own ? ' own' : ''));
    h(kc, 'span', 'kill-ally', k.killer);
    weaponMiniSvg(kc, k.weapon);
    if (k.hs) {
      const hs = h(kc, 'div', 'kill-hs');
      const hsvg = s(hs, 'svg', { width: 12, height: 12, viewBox: '0 0 12 12' });
      s(hsvg, 'circle', { cx: 6, cy: 6, r: 3.5, fill: 'none', stroke: '#ff5268', 'stroke-width': 1.4 });
      s(hsvg, 'line', { x1: 6, y1: 0.5, x2: 6, y2: 3.2, stroke: '#ff5268', 'stroke-width': 1.2 });
      s(hsvg, 'line', { x1: 6, y1: 8.8, x2: 6, y2: 11.5, stroke: '#ff5268', 'stroke-width': 1.2 });
      s(hsvg, 'line', { x1: 0.5, y1: 6, x2: 3.2, y2: 6, stroke: '#ff5268', 'stroke-width': 1.2 });
      s(hsvg, 'line', { x1: 8.8, y1: 6, x2: 11.5, y2: 6, stroke: '#ff5268', 'stroke-width': 1.2 });
    }
    h(kc, 'span', 'kill-enemy', k.victim);
  }

  // 6. Center Tactical Crosshair + Hit-Marker
  const xhair = h(root, 'div', 'crosshair');
  const xsvg = s(xhair, 'svg', { width: 48, height: 48, viewBox: '0 0 48 48' });
  // Outer cyan reticle ticks
  s(xsvg, 'line', { x1: 24, y1: 8, x2: 24, y2: 17, stroke: '#00f0ff', 'stroke-width': 1.8 });
  s(xsvg, 'line', { x1: 24, y1: 31, x2: 24, y2: 40, stroke: '#00f0ff', 'stroke-width': 1.8 });
  s(xsvg, 'line', { x1: 8, y1: 24, x2: 17, y2: 24, stroke: '#00f0ff', 'stroke-width': 1.8 });
  s(xsvg, 'line', { x1: 31, y1: 24, x2: 40, y2: 24, stroke: '#00f0ff', 'stroke-width': 1.8 });
  // Center dot
  s(xsvg, 'circle', { cx: 24, cy: 24, r: 1.5, fill: '#ffffff' });
  // Subtle tactical arc brackets
  s(xsvg, 'path', { d: 'M14 14 L18 18 M34 14 L30 18 M14 34 L18 30 M34 34 L30 30', stroke: 'rgba(255,59,82,0.88)', 'stroke-width': 1.6, 'stroke-linecap': 'round' });

  // 7. 3D World-Anchored Enemy Target Nameplate (pixel-locked to projected 3D enemy head at x=721.0, y=337.9, dist=12.3m)
  const anchor = h(root, 'div', 'anchor-target');
  const tcard = h(anchor, 'div', 'target-card');
  const thead = h(tcard, 'div', 'target-head');
  h(thead, 'span', 'target-name', 'VANGUARD-07');
  h(thead, 'span', 'target-dist', '12.3m');
  const tShieldBg = h(tcard, 'div', 'target-shield-bg');
  h(tShieldBg, 'div', 'target-shield-fill');
  const tHpBg = h(tcard, 'div', 'target-hp-bg');
  h(tHpBg, 'div', 'target-hp-ghost');
  h(tHpBg, 'div', 'target-hp-fill');
  h(anchor, 'div', 'target-stem');
  h(anchor, 'div', 'target-diamond');

  // 8. Bottom-Left Agent Vitality & Shield Core
  const vitalWrap = h(root, 'div', 'vital-wrap');
  const buffs = h(vitalWrap, 'div', 'buff-row');
  h(buffs, 'div', 'buff-pill', 'OVERCLOCK  6.2s');
  h(buffs, 'div', 'buff-pill amber', 'OPTIC SYNC');

  const vitalBorder = h(vitalWrap, 'div', 'vital-border');
  const vitalPanel = h(vitalBorder, 'div', 'vital-panel');

  const hex = h(vitalPanel, 'div', 'agent-hex');
  const hexSvg = s(hex, 'svg', { width: 34, height: 34, viewBox: '0 0 34 34' });
  s(hexSvg, 'polygon', { points: '17,3 30,13 24,29 17,23 10,29 4,13', fill: 'none', stroke: '#00f0ff', 'stroke-width': 2 });
  s(hexSvg, 'polygon', { points: '17,9 24,15 17,20 10,15', fill: '#eef8ff' });

  const vbody = h(vitalPanel, 'div', 'vital-body');
  const vtop = h(vbody, 'div', 'vital-top');
  h(vtop, 'div', 'vital-callsign', 'KESTREL // RECON');
  const vnums = h(vtop, 'div', 'vital-nums');

  const sp1 = h(vnums, 'div', 'stat-pair');
  h(sp1, 'span', 'shield-val', '50');
  h(sp1, 'span', 'stat-unit', 'SHD');

  const sp2 = h(vnums, 'div', 'stat-pair');
  h(sp2, 'span', 'hp-val', '100');
  h(sp2, 'span', 'stat-unit', 'HP');

  const sSegs = h(vbody, 'div', 'shield-segs');
  for (let i = 0; i < 4; i++) h(sSegs, 'div', 'shield-seg' + (i < 3 ? ' filled' : ''));

  const hpTrack = h(vbody, 'div', 'hp-bar-track');
  const hpFill = h(hpTrack, 'div', 'hp-bar-fill');
  h(hpFill, 'div', 'hp-bar-hatch');

  // 9. Bottom-Center Ability Loadout Dock
  const dock = h(root, 'div', 'ability-dock');
  const abilities = [
    { key: 'C', name: 'PULSE', charges: 2, max: 2, state: 'ready', icon: 'pulse' },
    { key: 'Q', name: 'ARC', charges: 0, max: 1, state: 'cooldown', cd: '4.2', icon: 'arc' },
    { key: 'E', name: 'DASH', charges: 1, max: 1, state: 'active', icon: 'dash' },
    { key: 'X', name: 'ORBITAL', charges: 7, max: 7, state: 'ult', icon: 'ult' },
  ];

  const drawAbilityIcon = (parent, kind) => {
    const size = kind === 'ult' ? 34 : 28;
    const svg = s(parent, 'svg', { width: size, height: size, viewBox: '0 0 32 32' });
    if (kind === 'pulse') {
      s(svg, 'circle', { cx: 16, cy: 16, r: 4, fill: '#00f0ff' });
      s(svg, 'circle', { cx: 16, cy: 16, r: 9, fill: 'none', stroke: '#eef8ff', 'stroke-width': 2 });
      s(svg, 'path', { d: 'M6 10 A12 12 0 0 1 26 10 M26 22 A12 12 0 0 1 6 22', fill: 'none', stroke: '#00f0ff', 'stroke-width': 2, 'stroke-linecap': 'round' });
    } else if (kind === 'arc') {
      s(svg, 'circle', { cx: 16, cy: 17, r: 8, fill: 'none', stroke: 'rgba(210,235,250,0.22)', 'stroke-width': 2 });
      s(svg, 'path', { d: 'M13 7 L19 7 M16 7 L16 9 M17 12 L13 18 L17 18 L15 23', fill: 'none', stroke: 'rgba(210,235,250,0.24)', 'stroke-width': 2, 'stroke-linejoin': 'round' });
    } else if (kind === 'dash') {
      s(svg, 'path', { d: 'M6 22 L16 10 L14 16 L26 10 L16 22 L18 16 Z', fill: '#00f0ff' });
      s(svg, 'line', { x1: 4, y1: 14, x2: 11, y2: 14, stroke: '#ffffff', 'stroke-width': 2, 'stroke-linecap': 'round' });
    } else {
      s(svg, 'circle', { cx: 16, cy: 16, r: 10, fill: 'none', stroke: '#ffb830', 'stroke-width': 2 });
      s(svg, 'polygon', { points: '16,4 19,13 28,16 19,19 16,28 13,19 4,16 13,13', fill: '#ffffff' });
      s(svg, 'circle', { cx: 16, cy: 16, r: 3, fill: '#ff3b52' });
    }
  };

  for (const ab of abilities) {
    const slot = h(dock, 'div', 'ability-slot' + (ab.state === 'active' ? ' active' : '') + (ab.state === 'ult' ? ' ult' : ''));
    const pips = h(slot, 'div', 'ability-pips');
    for (let i = 0; i < ab.max; i++) {
      if (ab.state === 'ult') {
        h(pips, 'div', 'ab-pip ult-pip');
      } else {
        h(pips, 'div', 'ab-pip' + (i < ab.charges ? ' on' : ''));
      }
    }
    const border = h(slot, 'div', 'ability-border');
    const box = h(border, 'div', 'ability-box');
    if (ab.state === 'ult') h(box, 'div', 'ability-flare');
    drawAbilityIcon(box, ab.icon);
    if (ab.state === 'cooldown') {
      h(box, 'div', 'ability-cd-sweep');
      h(box, 'div', 'ability-cd-num', ab.cd);
    }
    h(slot, 'div', 'ability-key', ab.key);
    h(slot, 'div', 'ability-name', ab.name);
  }

  // 10. Bottom-Right Weapon & Ammo Module
  const weaponWrap = h(root, 'div', 'weapon-wrap');
  const tabs = h(weaponWrap, 'div', 'slot-tabs');
  h(tabs, 'div', 'w-tab active', '1  VANDAL');
  h(tabs, 'div', 'w-tab', '2  SIDEARM');
  h(tabs, 'div', 'w-tab', '3  BLADE');

  const wBorder = h(weaponWrap, 'div', 'weapon-border');
  const wPanel = h(wBorder, 'div', 'weapon-panel');
  const wHead = h(wPanel, 'div', 'weapon-head');
  h(wHead, 'div', 'weapon-title', 'VANDAL MK-IV');
  h(wHead, 'div', 'fire-mode', 'AUTO');

  const wMid = h(wPanel, 'div', 'weapon-mid');
  const rifleSvg = s(wMid, 'svg', { width: 132, height: 42, viewBox: '0 0 132 42' });
  // Detailed futuristic rifle vector silhouette
  s(rifleSvg, 'path', {
    d: 'M6 17 L24 13 L42 13 L46 9 L66 9 L69 13 L102 13 L105 15 L126 15 L126 19 L104 19 L100 22 L84 22 L81 36 L72 36 L75 22 L53 22 L47 34 L39 34 L43 22 L28 22 L20 29 L6 29 Z',
    fill: '#dceefa',
  });
  s(rifleSvg, 'rect', { x: 49, y: 14, width: 16, height: 3, fill: '#00f0ff' });
  s(rifleSvg, 'rect', { x: 74, y: 15, width: 22, height: 2, fill: '#0b1624' });
  s(rifleSvg, 'line', { x1: 112, y1: 17, x2: 124, y2: 17, stroke: '#ff3b52', 'stroke-width': 2 });

  const ammo = h(wMid, 'div', 'ammo-readout');
  h(ammo, 'span', 'ammo-cur', '24');
  h(ammo, 'span', 'ammo-sep', '/');
  h(ammo, 'span', 'ammo-max', '96');

  const bullets = h(wPanel, 'div', 'bullet-strip');
  for (let i = 0; i < 30; i++) {
    const cls = i < 24 ? (i < 6 ? 'bullet-pip low' : 'bullet-pip lit') : 'bullet-pip';
    h(bullets, 'div', cls);
  }
})();
