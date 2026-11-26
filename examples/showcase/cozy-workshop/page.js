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

  // 1. Sunlit Cottage Background
  h(root, 'img', 'cottage-bg', undefined, { src: 'assets/bg.jpg' });
  h(root, 'div', 'sun-veil');

  // 2. Top Sunlit HUD Bar
  const topBar = h(root, 'div', 'top-bar');
  const leftPills = h(topBar, 'div', 'left-pills');

  const wsPill = h(leftPills, 'div', 'workshop-pill');
  const leafSvg = s(wsPill, 'svg', { width: 22, height: 22, viewBox: '0 0 24 24' });
  s(leafSvg, 'circle', { cx: 12, cy: 12, r: 11, fill: '#65ae57' });
  s(leafSvg, 'path', { d: 'M7 16 C8 10 13 7 18 7 C17 13 13 17 7 16 Z', fill: '#fdf9ec' });
  s(leafSvg, 'path', { d: 'M7 17 L13 11', stroke: '#4d8e41', 'stroke-width': 1.5, 'stroke-linecap': 'round' });
  h(wsPill, 'span', 'workshop-name', '风车工坊 · 烘焙台');

  const wPill = h(leftPills, 'div', 'weather-pill');
  const sunSvg = s(wPill, 'svg', { width: 17, height: 17, viewBox: '0 0 20 20' });
  s(sunSvg, 'circle', { cx: 10, cy: 10, r: 4.5, fill: '#f5a623' });
  for (let i = 0; i < 8; i++) {
    const a = (i * 45) * Math.PI / 180;
    s(sunSvg, 'line', {
      x1: (10 + Math.cos(a) * 6.5).toFixed(1),
      y1: (10 + Math.sin(a) * 6.5).toFixed(1),
      x2: (10 + Math.cos(a) * 8.8).toFixed(1),
      y2: (10 + Math.sin(a) * 8.8).toFixed(1),
      stroke: '#f5a623',
      'stroke-width': 1.8,
      'stroke-linecap': 'round',
    });
  }
  h(wPill, 'span', '', '晴朗 · 秋月 14日  15:20');

  const tabs = h(topBar, 'div', 'tab-pills');
  for (const t of ['甜点烘焙', '花草茶饮', '炼金药剂', '家具手作']) {
    h(tabs, 'div', 'cat-tab' + (t === '甜点烘焙' ? ' active' : ''), t);
  }

  const rightPills = h(topBar, 'div', 'right-pills');
  const coinB = h(rightPills, 'div', 'coin-badge');
  const csvg = s(coinB, 'svg', { width: 19, height: 19, viewBox: '0 0 20 20' });
  s(csvg, 'circle', { cx: 10, cy: 10, r: 8.5, fill: '#f7b938', stroke: '#c77d14', 'stroke-width': 1.6 });
  s(csvg, 'polygon', { points: '10,4.5 11.7,8.2 15.6,8.7 12.7,11.4 13.5,15.2 10,13.3 6.5,15.2 7.3,11.4 4.4,8.7 8.3,8.2', fill: '#fff8d6' });
  h(coinB, 'span', '', '2,480');

  const stamB = h(rightPills, 'div', 'stamina-badge');
  const stBar = h(stamB, 'div', 'stam-bar');
  h(stBar, 'div', 'stam-fill');
  h(stamB, 'span', '', '84/100');

  h(rightPills, 'div', 'close-btn', '×');

  // 3. Main Wooden Storybook Ledger
  const board = h(root, 'div', 'ledger-board');

  // --- LEFT PAGE ---
  const leftPage = h(board, 'div', 'page-sheet page-left');
  const pHdr = h(leftPage, 'div', 'page-hdr');
  const pTitleG = h(pHdr, 'div', 'page-title-grp');
  h(pTitleG, 'span', 'page-title', '手作配方图鉴');
  h(pTitleG, 'span', 'unlock-chip', '已解锁 18/24');
  const flts = h(pHdr, 'div', 'filter-pills');
  h(flts, 'span', 'flt-pill on', '可制作');
  h(flts, 'span', 'flt-pill', '全部');
  h(flts, 'span', 'flt-pill', '收藏');

  // Custom Colorful SVG Culinary Icon Drawer
  const drawDishSvg = (parent, kind) => {
    const svg = s(parent, 'svg', { width: 38, height: 38, viewBox: '0 0 38 38' });
    if (kind === 'tart') {
      s(svg, 'ellipse', { cx: 19, cy: 25, rx: 14, ry: 6, fill: '#e59d43', stroke: '#9b591a', 'stroke-width': 1.5 });
      s(svg, 'ellipse', { cx: 19, cy: 21, rx: 12, ry: 5.5, fill: '#f8cb66', stroke: '#b87122', 'stroke-width': 1.3 });
      s(svg, 'circle', { cx: 14, cy: 19, r: 3.2, fill: '#4b5ea8' });
      s(svg, 'circle', { cx: 24, cy: 19.5, r: 3.2, fill: '#e04f58' });
      s(svg, 'polygon', { points: '19,9 21.2,13.6 26.2,14.2 22.4,17.6 23.4,22.5 19,20 14.6,22.5 15.6,17.6 11.8,14.2 16.8,13.6', fill: '#ffe07d', stroke: '#b56e14', 'stroke-width': 1.2 });
    } else if (kind === 'tea') {
      s(svg, 'ellipse', { cx: 19, cy: 27, rx: 12, ry: 4, fill: '#d6e7f2', stroke: '#6d9cb8', 'stroke-width': 1.4 });
      s(svg, 'path', { d: 'M10 15 L28 15 L26 25 C25 27 13 27 12 25 Z', fill: '#8ecae6', stroke: '#4d82a0', 'stroke-width': 1.5 });
      s(svg, 'path', { d: 'M27 17 C32 17 32 23 26 23', fill: 'none', stroke: '#4d82a0', 'stroke-width': 2 });
      s(svg, 'path', { d: 'M16 11 C16 8 18 8 18 6 M21 11 C21 8 23 8 23 6', fill: 'none', stroke: '#a8d5e5', 'stroke-width': 1.6, 'stroke-linecap': 'round' });
    } else if (kind === 'jam') {
      s(svg, 'rect', { x: 11, y: 12, width: 16, height: 18, rx: 5, fill: '#e55c6c', stroke: '#9b2d3b', 'stroke-width': 1.5 });
      s(svg, 'rect', { x: 10, y: 8, width: 18, height: 5, rx: 2, fill: '#f9e6bc', stroke: '#b88546', 'stroke-width': 1.4 });
      s(svg, 'rect', { x: 14, y: 17, width: 10, height: 7, rx: 2, fill: '#fff8e8' });
    } else if (kind === 'soup') {
      s(svg, 'path', { d: 'M7 18 L31 18 C30 27 25 30 19 30 C13 30 8 27 7 18 Z', fill: '#fff3db', stroke: '#a87b4a', 'stroke-width': 1.5 });
      s(svg, 'ellipse', { cx: 19, cy: 18, rx: 11, ry: 3.8, fill: '#f29438', stroke: '#c46318', 'stroke-width': 1.3 });
      s(svg, 'circle', { cx: 19, cy: 18, r: 2.2, fill: '#fff8e2' });
    } else if (kind === 'roll') {
      s(svg, 'path', { d: 'M7 24 C8 14 14 11 19 11 C24 11 30 14 31 24 C27 27 11 27 7 24 Z', fill: '#edb058', stroke: '#9e5d1b', 'stroke-width': 1.5 });
      s(svg, 'path', { d: 'M14 13 C16 17 16 21 14 25 M24 13 C22 17 22 21 24 25', fill: 'none', stroke: '#b36b22', 'stroke-width': 1.4 });
    } else if (kind === 'mint') {
      s(svg, 'circle', { cx: 19, cy: 19, r: 9, fill: '#7de0b8', stroke: '#328f6c', 'stroke-width': 1.5 });
      s(svg, 'polygon', { points: '5,13 11,19 5,25', fill: '#b8f2dc', stroke: '#328f6c', 'stroke-width': 1.3 });
      s(svg, 'polygon', { points: '33,13 27,19 33,25', fill: '#b8f2dc', stroke: '#328f6c', 'stroke-width': 1.3 });
      s(svg, 'circle', { cx: 19, cy: 19, r: 4, fill: '#e9fff6' });
    } else if (kind === 'pancake') {
      s(svg, 'ellipse', { cx: 19, cy: 27, rx: 12, ry: 4, fill: '#df9945', stroke: '#915218', 'stroke-width': 1.4 });
      s(svg, 'ellipse', { cx: 19, cy: 22, rx: 11.5, ry: 4, fill: '#edb058', stroke: '#915218', 'stroke-width': 1.4 });
      s(svg, 'ellipse', { cx: 19, cy: 17, rx: 11, ry: 4, fill: '#f6c76d', stroke: '#915218', 'stroke-width': 1.4 });
      s(svg, 'rect', { x: 16, y: 12, width: 6, height: 4, rx: 1, fill: '#fff5b8', stroke: '#c78a22', 'stroke-width': 1 });
    } else if (kind === 'apple') {
      s(svg, 'circle', { cx: 19, cy: 20, r: 11, fill: '#e8a14c', stroke: '#9a581c', 'stroke-width': 1.5 });
      s(svg, 'circle', { cx: 19, cy: 20, r: 8, fill: '#d9534f' });
      s(svg, 'path', { d: 'M13 15 L25 25 M25 15 L13 25', stroke: '#f8d082', 'stroke-width': 2.2 });
    } else {
      s(svg, 'rect', { x: 14, y: 8, width: 10, height: 4, rx: 1.5, fill: '#c48f56' });
      s(svg, 'path', { d: 'M15 12 L23 12 L27 25 C27 28 24 30 19 30 C14 30 11 28 11 25 Z', fill: '#ffd166', stroke: '#b87e1a', 'stroke-width': 1.5 });
    }
  };

  const recGrid = h(leftPage, 'div', 'recipe-grid');
  const recipes = [
    { name: '蜂蜜星芒派', stars: '★★★', kind: 'tart', tag: '可做×3', sel: true },
    { name: '星露花茶', stars: '★★', kind: 'tea', tag: '×4' },
    { name: '林间莓果酱', stars: '★★', kind: 'jam', tag: '×2' },
    { name: '焦糖南瓜汤', stars: '★★', kind: 'soup', tag: '×5' },
    { name: '海盐奶油卷', stars: '★', kind: 'roll', tag: '×6' },
    { name: '月光薄荷糖', stars: '★★', kind: 'mint', tag: '×3' },
    { name: '榛果松饼塔', stars: '★★★', kind: 'pancake', tag: '×2' },
    { name: '苹果肉桂派', stars: '★★', kind: 'apple', tag: '×1' },
    { name: '晨露花蜜饮', stars: '★★★', kind: 'nectar', locked: true },
  ];

  for (const r of recipes) {
    const card = h(recGrid, 'div', 'rec-card' + (r.sel ? ' selected' : '') + (r.locked ? ' locked' : ''));
    if (r.tag) h(card, 'div', 'rec-tag', r.tag);
    const well = h(card, 'div', 'rec-icon-well');
    drawDishSvg(well, r.kind);
    h(card, 'div', 'rec-name', r.name);
    const meta = h(card, 'div', 'rec-meta');
    h(meta, 'span', 'rec-stars', r.stars);
    h(meta, 'span', '', r.locked ? '材料不足' : '熟练');
  }

  // Sticky Village Order Note
  const note = h(leftPage, 'div', 'order-note');
  h(note, 'div', 'note-pin');
  const noteL = h(note, 'div', '');
  h(noteL, 'div', 'order-title', '今日村镇委托 · 艾米莉的面包店');
  const nd = h(noteL, 'div', 'order-desc');
  h(nd, 'span', '', '交付 ');
  h(nd, 'span', 'order-hi', '2 份 蜂蜜星芒派');
  h(nd, 'span', '', '，赶在傍晚茶会前送达广场。');
  const noteR = h(note, 'div', 'order-reward');
  h(noteR, 'div', 'rew-pill', '+420 金币');
  h(noteR, 'div', 'rew-pill heart', '♥ 好感 +25');

  // --- RIGHT PAGE ---
  const rightPage = h(board, 'div', 'page-sheet page-right');
  h(rightPage, 'div', 'ribbon-mark');

  const heroRow = h(rightPage, 'div', 'hero-row');
  const dFrame = h(heroRow, 'div', 'dish-frame');
  h(dFrame, 'img', 'dish-img', undefined, { src: 'assets/tart.jpg' });
  h(dFrame, 'div', 'quality-ribbon', '★★★ 特级风味');

  const hInfo = h(heroRow, 'div', 'hero-info');
  const hTop = h(hInfo, 'div', '');
  const bRow = h(hTop, 'div', 'dish-badges');
  h(bRow, 'span', 'badge-sage', '甜点烘焙 · 稀有');
  h(bRow, 'span', 'badge-gold', '售价 210 金币');
  h(hTop, 'div', 'dish-title', '蜂蜜星芒派');

  h(hInfo, 'div', 'dish-desc', '刚出炉的酥脆派皮裹着林间野莓与槐花蜜，温热的星形酥饼散发着淡淡黄油与薄荷清香。');

  const eRow = h(hInfo, 'div', 'effect-row');
  h(eRow, 'div', 'eff-chip', '♥ 体力恢复 +45');
  h(eRow, 'div', 'eff-chip luck', '✦ 采集幸运 +15%');

  // Ingredients Strip
  const ingSec = h(rightPage, 'div', 'ing-section');
  const ingHdr = h(ingSec, 'div', 'ing-hdr');
  h(ingHdr, 'span', 'ing-title', '所需材料（每份 ×2 合计）');
  h(ingHdr, 'span', 'ing-ready', '✓ 材料齐备');

  const drawIngSvg = (parent, kind) => {
    const svg = s(parent, 'svg', { width: 42, height: 42, viewBox: '0 0 34 34' });
    if (kind === 'honey') {
      s(svg, 'rect', { x: 9, y: 10, width: 16, height: 17, rx: 5, fill: '#f5b030', stroke: '#9f5e10', 'stroke-width': 1.5 });
      s(svg, 'rect', { x: 8, y: 6, width: 18, height: 5, rx: 2, fill: '#fff4d4', stroke: '#b58442', 'stroke-width': 1.3 });
      s(svg, 'polygon', { points: '17,13 20,15 20,19 17,21 14,19 14,15', fill: '#ffe79e' });
    } else if (kind === 'berry') {
      s(svg, 'circle', { cx: 13, cy: 20, r: 6.5, fill: '#4963b8', stroke: '#283978', 'stroke-width': 1.4 });
      s(svg, 'circle', { cx: 22, cy: 21, r: 5.5, fill: '#dc4b59', stroke: '#8c222e', 'stroke-width': 1.4 });
      s(svg, 'path', { d: 'M14 13 C16 8 21 8 23 11 C19 12 16 14 14 13 Z', fill: '#5ca950' });
    } else if (kind === 'wheat') {
      s(svg, 'path', { d: 'M11 27 L23 8', stroke: '#b87f28', 'stroke-width': 2.2, 'stroke-linecap': 'round' });
      s(svg, 'ellipse', { cx: 16, cy: 14, rx: 3.5, ry: 2, transform: 'rotate(-35 16 14)', fill: '#f4c450', stroke: '#a66d18', 'stroke-width': 1.1 });
      s(svg, 'ellipse', { cx: 20, cy: 19, rx: 3.5, ry: 2, transform: 'rotate(-35 20 19)', fill: '#f4c450', stroke: '#a66d18', 'stroke-width': 1.1 });
      s(svg, 'ellipse', { cx: 21, cy: 11, rx: 3.5, ry: 2, transform: 'rotate(-35 21 11)', fill: '#f7d572', stroke: '#a66d18', 'stroke-width': 1.1 });
    } else {
      s(svg, 'path', { d: 'M10 24 C10 15 17 10 25 9 C24 18 18 24 10 24 Z', fill: '#64c26b', stroke: '#2f7835', 'stroke-width': 1.5 });
      s(svg, 'path', { d: 'M10 25 L20 14', stroke: '#2f7835', 'stroke-width': 1.4, 'stroke-linecap': 'round' });
    }
  };

  const ingGrid = h(ingSec, 'div', 'ing-grid');
  const ings = [
    { name: '野花蜂蜜', cnt: '6 / 4', kind: 'honey' },
    { name: '蓝莓鲜果', cnt: '9 / 6', kind: 'berry' },
    { name: '金穗面粉', cnt: '12 / 4', kind: 'wheat' },
    { name: '晨露薄荷', cnt: '4 / 2', kind: 'mint' },
  ];
  for (const ing of ings) {
    const slot = h(ingGrid, 'div', 'ing-slot');
    drawIngSvg(slot, ing.kind);
    h(slot, 'div', 'ing-name', ing.name);
    h(slot, 'div', 'ing-count', ing.cnt);
  }

  // Mastery Progress Bar
  const prof = h(rightPage, 'div', 'prof-bar-wrap');
  h(prof, 'span', 'prof-lbl', '烘焙熟练度 Lv.4');
  const pTrack = h(prof, 'div', 'prof-track');
  h(pTrack, 'div', 'prof-fill');
  h(prof, 'span', 'prof-val', '大成功概率 28%');

  // Bottom Craft Action Bar
  const foot = h(rightPage, 'div', 'craft-footer');
  const qty = h(foot, 'div', 'qty-control');
  h(qty, 'div', 'qty-btn', '−');
  h(qty, 'div', 'qty-num', '2');
  h(qty, 'div', 'qty-btn', '+');

  const meta = h(foot, 'div', 'craft-meta');
  h(meta, 'div', '', '预计耗时 8 秒');
  h(meta, 'div', '', '工坊经验 +70');

  const craftBtn = h(foot, 'div', 'craft-btn');
  const spSvg = s(craftBtn, 'svg', { width: 22, height: 22, viewBox: '0 0 22 22' });
  s(spSvg, 'polygon', { points: '11,1 13.5,8.5 21,11 13.5,13.5 11,21 8.5,13.5 1,11 8.5,8.5', fill: '#fff8c9' });
  h(craftBtn, 'span', '', '开始制作  ×2');
})();
