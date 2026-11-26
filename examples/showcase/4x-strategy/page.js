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

  const addMiniIcon = (parent, kind, color) => {
    const svg = s(parent, 'svg', { width: 13, height: 13, viewBox: '0 0 16 16', fill: 'none' });
    if (kind === 'energy') {
      s(svg, 'polygon', { points: '9,1.5 3.5,9 7.8,9 7,14.5 12.5,7 8.2,7', fill: color });
    } else if (kind === 'minerals') {
      s(svg, 'polygon', { points: '8,2 14,6 11.5,14 4.5,14 2,6', fill: color });
    } else if (kind === 'alloys') {
      s(svg, 'polygon', { points: '8,2 14,5.5 14,10.5 8,14 2,10.5 2,5.5', stroke: color, 'stroke-width': 1.6, fill: 'rgba(251,146,60,0.25)' });
    } else if (kind === 'science') {
      s(svg, 'circle', { cx: 8, cy: 8, r: 2.3, fill: color });
      s(svg, 'ellipse', { cx: 8, cy: 8, rx: 6.2, ry: 2.6, stroke: color, 'stroke-width': 1.2 });
    } else if (kind === 'unity') {
      s(svg, 'polygon', { points: '8,1.5 10,6 14.5,6.5 11,9.8 12,14.5 8,12 4,14.5 5,9.8 1.5,6.5 6,6', fill: color });
    } else {
      s(svg, 'circle', { cx: 8, cy: 8, r: 5.5, stroke: color, 'stroke-width': 1.5 });
      s(svg, 'circle', { cx: 8, cy: 8, r: 2, fill: color });
    }
  };

  // ==================== 1. WORLD-SPACE SVG TELEMETRY LEADER ====================
  // Exact 3D projected coordinates from showcase_host --mode 4x:
  // Sun=(190.6, 413.9), Earth=(556.5, 380.2), Moon=(677.4, 398.2), Mars=(577.2, 278.4), Saturn=(402.6, 208.6)
  const overSvg = s(root, 'svg', { class: 'overlay-svg', width: 1280, height: 720, viewBox: '0 0 1280 720' });

  // Subtle cyan corner bracket ticks around Earth's 3D selection ring
  s(overSvg, 'path', { d: 'M 464 316 L 464 302 L 478 302', fill: 'none', stroke: 'rgba(56, 189, 248, 0.72)', 'stroke-width': 1.5 });
  s(overSvg, 'path', { d: 'M 636 302 L 650 302 L 650 316', fill: 'none', stroke: 'rgba(56, 189, 248, 0.72)', 'stroke-width': 1.5 });
  s(overSvg, 'path', { d: 'M 464 442 L 464 456 L 478 456', fill: 'none', stroke: 'rgba(56, 189, 248, 0.72)', 'stroke-width': 1.5 });
  s(overSvg, 'path', { d: 'M 636 456 L 650 456 L 650 442', fill: 'none', stroke: 'rgba(56, 189, 248, 0.72)', 'stroke-width': 1.5 });

  // Glowing cyan leader line connecting 3D Earth -> Earth Visualization Panel (left edge x=724)
  s(overSvg, 'path', { d: 'M 646 336 L 686 296 L 724 296', fill: 'none', stroke: 'rgba(56, 189, 248, 0.25)', 'stroke-width': 5 });
  s(overSvg, 'path', { d: 'M 646 336 L 686 296 L 724 296', fill: 'none', stroke: '#38bdf8', 'stroke-width': 1.5 });
  s(overSvg, 'circle', { cx: 646, cy: 336, r: 3.5, fill: '#38bdf8', stroke: '#061222', 'stroke-width': 1.5 });
  s(overSvg, 'circle', { cx: 724, cy: 296, r: 3.5, fill: '#38bdf8', stroke: '#061222', 'stroke-width': 1.5 });

  // Subtle leader stems for Saturn, Mars, Sun nameplates
  s(overSvg, 'line', { x1: 403, y1: 162, x2: 403, y2: 146, stroke: 'rgba(234, 179, 8, 0.55)', 'stroke-width': 1.2 });
  s(overSvg, 'line', { x1: 577, y1: 254, x2: 577, y2: 238, stroke: 'rgba(251, 146, 60, 0.60)', 'stroke-width': 1.2 });
  s(overSvg, 'line', { x1: 191, y1: 522, x2: 191, y2: 538, stroke: 'rgba(251, 191, 36, 0.55)', 'stroke-width': 1.2 });

  // ==================== 2. TOP STELLARIS EMPIRE BAR ====================
  const topbar = h(root, 'div', 'topbar');

  const empId = h(topbar, 'div', 'empire-id');
  const emb = h(empId, 'div', 'empire-emblem');
  const embSvg = s(emb, 'svg', { width: 16, height: 16, viewBox: '0 0 20 20', fill: 'none' });
  s(embSvg, 'circle', { cx: 10, cy: 10, r: 7, stroke: '#38bdf8', 'stroke-width': 1.5 });
  s(embSvg, 'ellipse', { cx: 10, cy: 10, rx: 3.2, ry: 7, stroke: '#7dd3fc', 'stroke-width': 1.1 });
  s(embSvg, 'line', { x1: 3, y1: 10, x2: 17, y2: 10, stroke: '#7dd3fc', 'stroke-width': 1.1 });
  h(empId, 'span', 'empire-name', 'UNITED NATIONS OF EARTH');
  h(empId, 'span', 'sys-pill', 'SOL SYSTEM');

  const resStrip = h(topbar, 'div', 'res-strip');
  for (const r of [
    { icon: 'energy', col: '#fbbf24', val: '24.6K', delta: '+482', dCls: 'res-delta res-delta-gold' },
    { icon: 'minerals', col: '#38bdf8', val: '18.2K', delta: '+318', dCls: 'res-delta res-delta-cyan' },
    { icon: 'alloys', col: '#fb923c', val: '6,840', delta: '+214', dCls: 'res-delta' },
    { icon: 'science', col: '#38bdf8', val: 'TECH', delta: '+1,680', dCls: 'res-delta res-delta-cyan' },
    { icon: 'unity', col: '#a78bfa', val: '12.4K', delta: '+196', dCls: 'res-delta res-delta-violet' },
  ]) {
    const it = h(resStrip, 'div', 'res-item');
    addMiniIcon(it, r.icon, r.col);
    h(it, 'span', 'res-val', r.val);
    h(it, 'span', r.dCls, r.delta);
  }

  const clock = h(topbar, 'div', 'clock-box');
  h(clock, 'span', 'clock-date', '2284.07.14');
  h(clock, 'span', 'clock-spd', '▶▶ FAST');

  // ==================== 3. WORLD-PROJECTED 3D PLANET NAMEPLATES ====================
  // Sun nameplate (centered under Sun at x=191, y=538)
  const sunTag = h(root, 'div', 'tag-pin');
  sunTag.style.left = '136px';
  sunTag.style.top = '538px';
  h(sunTag, 'span', 'tag-dot dot-sun');
  h(sunTag, 'span', '', 'SOL');
  h(sunTag, 'span', 'tag-dim', 'G2V STAR');

  // Saturn nameplate (centered above Saturn at x=403, y=122)
  const satTag = h(root, 'div', 'tag-pin');
  satTag.style.left = '338px';
  satTag.style.top = '122px';
  h(satTag, 'span', 'tag-dot dot-saturn');
  h(satTag, 'span', '', 'SATURN');
  h(satTag, 'span', 'tag-sub', '+6 ENERGY');

  // Mars nameplate (centered above Mars at x=577, y=214)
  const marsTag = h(root, 'div', 'tag-pin');
  marsTag.style.left = '518px';
  marsTag.style.top = '214px';
  h(marsTag, 'span', 'tag-dot dot-mars');
  h(marsTag, 'span', '', 'MARS');
  h(marsTag, 'span', 'tag-dim', 'COLONY 74%');

  // Earth Hero Capital Nameplate (centered below Earth cyan selection ring at x=556, y=468)
  const earthTag = h(root, 'div', 'tag-earth');
  earthTag.style.left = '446px';
  earthTag.style.top = '468px';
  const eTop = h(earthTag, 'div', 'tag-earth-top');
  h(eTop, 'span', 'tag-dot dot-earth');
  h(eTop, 'span', 'tag-earth-name', 'EARTH · SOL III');
  h(eTop, 'span', 'tag-earth-cap', 'CAPITAL');
  h(earthTag, 'div', 'tag-earth-sub', '482 POPS · 88% STAB · ORBITAL RING LV.4');

  // ==================== 4. BOTTOM-LEFT SOL SYSTEM ORBITAL DOCK ====================
  const dock = h(root, 'div', 'sol-dock');
  const dLeft = h(dock, 'div', 'sol-dock-left');
  h(dLeft, 'div', 'sol-dock-title', 'SOL SYSTEM TELEMETRY · 3D VIEWPORT');
  h(dLeft, 'div', 'sol-dock-sub', '8 BODIES · HYPERLANE SECURE · 4X MSAA');

  const dStats = h(dock, 'div', 'sol-dock-stats');
  for (const k of [
    { lbl: 'ARMADA', val: '48.2K' },
    { lbl: 'TRADE', val: '94%' },
  ]) {
    const b = h(dStats, 'div', 'dock-kpi');
    h(b, 'span', 'dock-kpi-lbl', k.lbl);
    h(b, 'span', 'dock-kpi-val', k.val);
  }

  // ==================== 5. HERO RIGHT PANEL: EARTH COMPLEX VISUALIZATION SUITE ====================
  const panel = h(root, 'div', 'earth-panel');
  h(panel, 'div', 'ep-accent-top');

  // Row A: Earth Planetary Header
  const epHead = h(panel, 'div', 'ep-head');
  const epLeft = h(epHead, 'div');
  const epTR = h(epLeft, 'div', 'ep-title-row');
  h(epTR, 'span', 'ep-title', 'EARTH');
  h(epTR, 'span', 'ep-class-pill', 'CONTINENTAL · 100% HAB');
  h(epLeft, 'div', 'ep-sub', 'SOL III · SECTOR CAPITAL · SIZE 24 · ARCOLOGY');

  const epKpis = h(epHead, 'div', 'ep-kpis');
  for (const k of [
    { val: '88%', lbl: 'STABILITY', cls: 'c-emerald' },
    { val: '+42', lbl: 'AMENITIES', cls: 'c-cyan' },
    { val: '418', lbl: 'TRADE', cls: 'c-amber' },
  ]) {
    const box = h(epKpis, 'div', 'ep-kpi');
    h(box, 'span', 'ep-kpi-val ' + k.cls, k.val);
    h(box, 'span', 'ep-kpi-lbl', k.lbl);
  }

  // Row B: Century Planetary Output Multi-Series Area & Line Chart
  const chartCard = h(panel, 'div', 'viz-card');
  const chartHead = h(chartCard, 'div', 'viz-head');
  h(chartHead, 'span', 'viz-title', 'CENTURY OUTPUT (2200–2284)');
  const cLeg = h(chartHead, 'div', 'viz-legend');
  const series = [
    { name: 'RESEARCH', col: '#38bdf8', fill: 'rgba(56, 189, 248, 0.16)', vals: [180, 260, 390, 560, 740, 980, 1290, 1490, 1680] },
    { name: 'ENERGY', col: '#fbbf24', fill: 'rgba(251, 191, 36, 0.11)', vals: [240, 320, 410, 520, 610, 720, 840, 960, 1080] },
    { name: 'ALLOYS', col: '#fb923c', fill: 'rgba(251, 146, 60, 0.09)', vals: [110, 160, 230, 340, 460, 560, 690, 780, 860] },
    { name: 'TRADE', col: '#34d399', fill: 'rgba(52, 211, 153, 0.08)', vals: [95, 130, 185, 250, 310, 390, 465, 540, 620] },
  ];
  for (const sr of series) {
    const li = h(cLeg, 'div', 'leg-item');
    const sw = h(li, 'span', 'leg-swatch');
    sw.style.backgroundColor = sr.col;
    h(li, 'span', '', sr.name);
  }

  const cW = 486, cH = 138;
  const padL = 34, padR = 16, padT = 10, padB = 20;
  const plotW = cW - padL - padR, plotH = cH - padT - padB;
  const maxVal = 1800;
  const cSvg = s(chartCard, 'svg', { width: cW, height: cH, viewBox: `0 0 ${cW} ${cH}` });

  // Horizontal gridlines + Y labels
  for (const tick of [0, 600, 1200, 1800]) {
    const y = padT + plotH * (1 - tick / maxVal);
    s(cSvg, 'line', { x1: padL, y1: y.toFixed(1), x2: cW - padR, y2: y.toFixed(1), stroke: 'rgba(148, 163, 184, 0.14)', 'stroke-width': 1 });
    const lb = s(cSvg, 'text', { x: padL - 6, y: (y + 3).toFixed(1), 'text-anchor': 'end', fill: '#64748b', 'font-size': 8.5, 'font-family': 'JetBrains Mono, monospace' });
    lb.textContent = tick === 0 ? '0' : (tick >= 1000 ? (tick / 1000).toFixed(1) + 'K' : String(tick));
  }

  // Year axis ticks + milestone vertical markers
  const years = ['2200', '2212', '2224', '2236', '2248', '2260', '2272', '2278', '2284'];
  years.forEach((yr, i) => {
    const x = padL + (i / (years.length - 1)) * plotW;
    if (i % 2 === 0) {
      const lb = s(cSvg, 'text', { x: x.toFixed(1), y: cH - 5, 'text-anchor': 'middle', fill: '#64748b', 'font-size': 8.5, 'font-family': 'JetBrains Mono, monospace' });
      lb.textContent = yr;
    }
  });
  // Milestone vertical marker at 2260 (index 5): Orbital Ring Completed
  const mx = padL + (5 / 8) * plotW;
  s(cSvg, 'line', { x1: mx.toFixed(1), y1: padT, x2: mx.toFixed(1), y2: padT + plotH, stroke: 'rgba(56, 189, 248, 0.35)', 'stroke-width': 1, 'stroke-dasharray': '3 3' });
  const mLbl = s(cSvg, 'text', { x: (mx + 4).toFixed(1), y: padT + 9, fill: '#7dd3fc', 'font-size': 8, 'font-family': 'JetBrains Mono, monospace' });
  mLbl.textContent = '2260 ORBITAL RING';

  // Draw area polygons + crisp data polylines + endpoint dots
  for (const sr of series) {
    const coords = sr.vals.map((v, i) => {
      const x = padL + (i / (sr.vals.length - 1)) * plotW;
      const y = padT + plotH * (1 - v / maxVal);
      return [x, y];
    });
    const baseY = padT + plotH;
    const areaPts = `${padL},${baseY} ` + coords.map(p => `${p[0].toFixed(1)},${p[1].toFixed(1)}`).join(' ') + ` ${cW - padR},${baseY}`;
    s(cSvg, 'polygon', { points: areaPts, fill: sr.fill });
    const linePts = coords.map(p => `${p[0].toFixed(1)},${p[1].toFixed(1)}`).join(' ');
    s(cSvg, 'polyline', { points: linePts, fill: 'none', stroke: sr.col, 'stroke-width': 1.9, 'stroke-linejoin': 'round' });
    const last = coords[coords.length - 1];
    s(cSvg, 'circle', { cx: last[0].toFixed(1), cy: last[1].toFixed(1), r: 3, fill: sr.col, stroke: '#07101c', 'stroke-width': 1.2 });
  }

  // Clean upper-left live 2284 snapshot pill inside chart empty space
  s(cSvg, 'rect', { x: padL + 8, y: padT + 4, width: 158, height: 42, rx: 4, fill: 'rgba(6, 13, 24, 0.86)', stroke: 'rgba(56, 189, 248, 0.35)', 'stroke-width': 1 });
  const st1 = s(cSvg, 'text', { x: padL + 16, y: padT + 17, fill: '#94a3b8', 'font-size': 8, 'font-weight': 700, 'font-family': 'JetBrains Mono, monospace' });
  st1.textContent = '2284 PEAK OUTPUT';
  const st2 = s(cSvg, 'text', { x: padL + 16, y: padT + 32, fill: '#38bdf8', 'font-size': 10.5, 'font-weight': 700, 'font-family': 'JetBrains Mono, monospace' });
  st2.textContent = '+1,680 SCI';
  const st3 = s(cSvg, 'text', { x: padL + 92, y: padT + 32, fill: '#fbbf24', 'font-size': 10.5, 'font-weight': 700, 'font-family': 'JetBrains Mono, monospace' });
  st3.textContent = '+1.08K EN';

  // Row C: Split — Earth Production Sankey Flow + Population Strata Donut
  const rowC = h(panel, 'div', 'split-row');

  // C1: Sankey Flow Diagram
  const skCard = h(rowC, 'div', 'viz-card card-sankey');
  const skHead = h(skCard, 'div', 'viz-head');
  h(skHead, 'span', 'viz-title', 'RESOURCE ALLOCATION FLOW');
  h(skHead, 'span', 'leg-item', 'SANKEY');

  const skSvg = s(skCard, 'svg', { width: 272, height: 140, viewBox: '0 0 272 140' });
  const srcNodes = [
    { y: 6, h: 34, col: '#38bdf8', lbl: 'Research Labs', val: '640' },
    { y: 46, h: 30, col: '#fbbf24', lbl: 'Solar Grid', val: '482' },
    { y: 82, h: 26, col: '#fb923c', lbl: 'Foundries', val: '318' },
    { y: 114, h: 20, col: '#34d399', lbl: 'Trade Hubs', val: '240' },
  ];
  const dstNodes = [
    { y: 6, h: 40, col: '#38bdf8', lbl: 'Tech Surge', val: '710' },
    { y: 52, h: 30, col: '#fb923c', lbl: 'Sol Armada', val: '420' },
    { y: 88, h: 24, col: '#a78bfa', lbl: 'Megastruct', val: '310' },
    { y: 118, h: 16, col: '#34d399', lbl: 'Net Vault', val: '240' },
  ];
  const flows = [
    { sy: 6, sh: 26, dy: 6, dh: 26, col: 'rgba(56, 189, 248, 0.34)' },
    { sy: 32, sh: 8, dy: 88, dh: 8, col: 'rgba(56, 189, 248, 0.24)' },
    { sy: 46, sh: 12, dy: 34, dh: 12, col: 'rgba(251, 191, 36, 0.28)' },
    { sy: 58, sh: 12, dy: 52, dh: 12, col: 'rgba(251, 191, 36, 0.32)' },
    { sy: 70, sh: 6, dy: 118, dh: 6, col: 'rgba(251, 191, 36, 0.22)' },
    { sy: 82, sh: 16, dy: 64, dh: 16, col: 'rgba(251, 146, 60, 0.34)' },
    { sy: 98, sh: 10, dy: 98, dh: 10, col: 'rgba(251, 146, 60, 0.26)' },
    { sy: 114, sh: 10, dy: 124, dh: 10, col: 'rgba(52, 211, 153, 0.34)' },
    { sy: 124, sh: 10, dy: 108, dh: 6, col: 'rgba(52, 211, 153, 0.22)' },
  ];
  const xL = 78, xR = 194, mx1 = 122, mx2 = 150;
  for (const f of flows) {
    const d = `M ${xL} ${f.sy} C ${mx1} ${f.sy}, ${mx2} ${f.dy}, ${xR} ${f.dy} L ${xR} ${f.dy + f.dh} C ${mx2} ${f.dy + f.dh}, ${mx1} ${f.sy + f.sh}, ${xL} ${f.sy + f.sh} Z`;
    s(skSvg, 'path', { d, fill: f.col });
  }
  for (const n of srcNodes) {
    s(skSvg, 'rect', { x: xL - 7, y: n.y, width: 7, height: n.h, rx: 1.5, fill: n.col });
    const t = s(skSvg, 'text', { x: xL - 11, y: n.y + n.h * 0.5 + 3, 'text-anchor': 'end', fill: '#cbd5e1', 'font-size': 8.5, 'font-family': 'Segoe UI, sans-serif' });
    t.textContent = n.lbl;
  }
  for (const n of dstNodes) {
    s(skSvg, 'rect', { x: xR, y: n.y, width: 7, height: n.h, rx: 1.5, fill: n.col });
    const t = s(skSvg, 'text', { x: xR + 11, y: n.y + n.h * 0.5 + 3, 'text-anchor': 'start', fill: '#cbd5e1', 'font-size': 8.5, 'font-family': 'Segoe UI, sans-serif' });
    t.textContent = n.lbl;
  }

  // C2: Population Strata Donut Chart (exact SVG arc paths!)
  const dnCard = h(rowC, 'div', 'viz-card card-donut');
  const dnHead = h(dnCard, 'div', 'viz-head');
  h(dnHead, 'span', 'viz-title', 'POP STRATA');
  h(dnHead, 'span', 'leg-item', '482');

  const dnBody = h(dnCard, 'div', 'donut-body');
  const dnSvg = s(dnBody, 'svg', { width: 92, height: 92, viewBox: '0 0 92 92' });
  const dcx = 46, dcy = 46, dR = 34;
  const strata = [
    { lbl: 'Spec.', pct: 46, col: '#38bdf8' },
    { lbl: 'Work.', pct: 31, col: '#fbbf24' },
    { lbl: 'Synth', pct: 12, col: '#a78bfa' },
    { lbl: 'Ruler', pct: 11, col: '#34d399' },
  ];
  let acc = -90;
  for (const st of strata) {
    const span = (st.pct / 100) * 360 - 4;
    const a1 = acc * Math.PI / 180;
    const a2 = (acc + span) * Math.PI / 180;
    const x1 = dcx + Math.cos(a1) * dR, y1 = dcy + Math.sin(a1) * dR;
    const x2 = dcx + Math.cos(a2) * dR, y2 = dcy + Math.sin(a2) * dR;
    const large = span > 180 ? 1 : 0;
    s(dnSvg, 'path', {
      d: `M ${x1.toFixed(2)} ${y1.toFixed(2)} A ${dR} ${dR} 0 ${large} 1 ${x2.toFixed(2)} ${y2.toFixed(2)}`,
      fill: 'none', stroke: st.col, 'stroke-width': 9, 'stroke-linecap': 'butt'
    });
    acc += (st.pct / 100) * 360;
  }
  const dc1 = s(dnSvg, 'text', { x: dcx, y: dcy + 1, 'text-anchor': 'middle', fill: '#f8fafc', 'font-size': 14, 'font-weight': 700, 'font-family': 'JetBrains Mono, monospace' });
  dc1.textContent = '482';
  const dc2 = s(dnSvg, 'text', { x: dcx, y: dcy + 12, 'text-anchor': 'middle', fill: '#64748b', 'font-size': 7.5, 'font-weight': 700, 'font-family': 'JetBrains Mono, monospace' });
  dc2.textContent = 'POPS';

  const dnList = h(dnBody, 'div', 'donut-list');
  for (const st of strata) {
    const r = h(dnList, 'div', 'donut-row');
    const l = h(r, 'div', 'donut-left');
    const sw = h(l, 'span', 'leg-swatch');
    sw.style.backgroundColor = st.col;
    h(l, 'span', '', st.lbl);
    h(r, 'span', 'donut-pct', st.pct + '%');
  }

  // Row D: Split — Stellaris Planetary Districts Matrix + Live Sparklines
  const rowD = h(panel, 'div', 'split-row');

  // D1: Planetary Districts Matrix
  const distCard = h(rowD, 'div', 'viz-card card-districts');
  const distHead = h(distCard, 'div', 'viz-head');
  h(distHead, 'span', 'viz-title', 'PLANETARY DISTRICTS');
  h(distHead, 'span', 'leg-item', '21 / 24');
  const distList = h(distCard, 'div', 'dist-list');
  for (const d of [
    { name: 'CITY', cur: 8, max: 9, cls: 'pip pip-city' },
    { name: 'INDUSTRIAL', cur: 6, max: 7, cls: 'pip pip-ind' },
    { name: 'GENERATOR', cur: 4, max: 5, cls: 'pip pip-gen' },
    { name: 'RESEARCH', cur: 3, max: 3, cls: 'pip pip-res' },
  ]) {
    const row = h(distList, 'div', 'dist-row');
    h(row, 'span', 'dist-name', d.name);
    const pips = h(row, 'div', 'dist-pips');
    for (let i = 0; i < d.max; i++) {
      h(pips, 'span', i < d.cur ? d.cls : 'pip-empty');
    }
    h(row, 'span', 'dist-cnt', `${d.cur}/${d.max}`);
  }

  // D2: Live Earth Telemetry Sparklines
  const spCard = h(rowD, 'div', 'viz-card card-sparks');
  const spHead = h(spCard, 'div', 'viz-head');
  h(spHead, 'span', 'viz-title', 'TELEMETRY TRENDS');
  h(spHead, 'span', 'leg-item', '10Y');
  const spList = h(spCard, 'div', 'spark-list');
  const sparks = [
    { lbl: 'POP GROWTH', val: '+8.4/mo', cls: 'c-emerald', col: '#34d399', fill: 'rgba(52, 211, 153, 0.18)', pts: [38, 42, 46, 54, 58, 66, 74, 86] },
    { lbl: 'TRADE VALUE', val: '418', cls: 'c-amber', col: '#fbbf24', fill: 'rgba(251, 191, 36, 0.18)', pts: [45, 52, 48, 60, 68, 72, 79, 88] },
    { lbl: 'ORBITAL DEF', val: '18.6K', cls: 'c-cyan', col: '#38bdf8', fill: 'rgba(56, 189, 248, 0.18)', pts: [30, 32, 44, 52, 64, 70, 82, 92] },
  ];
  for (const sp of sparks) {
    const row = h(spList, 'div', 'spark-row');
    const meta = h(row, 'div', 'spark-meta');
    h(meta, 'span', 'spark-lbl', sp.lbl);
    h(meta, 'span', 'spark-val ' + sp.cls, sp.val);
    const sw = 112, sh = 24;
    const svg = s(row, 'svg', { width: sw, height: sh, viewBox: `0 0 ${sw} ${sh}` });
    const coords = sp.pts.map((v, i) => [(i / (sp.pts.length - 1)) * (sw - 4) + 2, sh - 2 - (v / 100) * (sh - 5)]);
    const aPts = `2,${sh - 2} ` + coords.map(p => `${p[0].toFixed(1)},${p[1].toFixed(1)}`).join(' ') + ` ${sw - 2},${sh - 2}`;
    s(svg, 'polygon', { points: aPts, fill: sp.fill });
    s(svg, 'polyline', { points: coords.map(p => `${p[0].toFixed(1)},${p[1].toFixed(1)}`).join(' '), fill: 'none', stroke: sp.col, 'stroke-width': 1.6 });
    const last = coords[coords.length - 1];
    s(svg, 'circle', { cx: last[0].toFixed(1), cy: last[1].toFixed(1), r: 2.2, fill: sp.col });
  }

  // Row E: Bottom Earth Economic Balance Bar
  const foot = h(panel, 'div', 'ep-footer');
  const fTop = h(foot, 'div', 'foot-top');
  h(fTop, 'span', 'c-cyan', 'EARTH NET OUTPUT +1,420 / MO');
  h(fTop, 'span', 'c-emerald', 'BIOSPHERE 99.4% · UPKEEP -386');
  const fBar = h(foot, 'div', 'foot-bar');
  for (const seg of [
    { w: '36%', col: '#38bdf8' },
    { w: '24%', col: '#fbbf24' },
    { w: '19%', col: '#fb923c' },
    { w: '13%', col: '#34d399' },
    { w: '8%', col: '#a78bfa' },
  ]) {
    const b = h(fBar, 'div', 'fb-seg');
    b.style.width = seg.w;
    b.style.backgroundColor = seg.col;
  }
})();
