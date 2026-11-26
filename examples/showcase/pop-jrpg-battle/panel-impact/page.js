(() => {
  const SVG_NS = 'http://www.w3.org/2000/svg';
  const root = document.getElementById('root');
  const h = (p, t, c, txt) => {
    const el = document.createElement(t);
    if (c) el.className = c;
    if (txt !== undefined) el.textContent = txt;
    p.appendChild(el);
    return el;
  };
  const s = (p, t, a = {}) => {
    const el = document.createElementNS(SVG_NS, t);
    for (const [k, v] of Object.entries(a)) el.setAttribute(k, String(v));
    p.appendChild(el);
    return el;
  };

  const starSvg = s(root, 'svg', { class: 'starburst-svg', width: 260, height: 180, viewBox: '0 0 224 156' });
  const burstPts = '102,4 122,30 162,10 156,42 208,32 176,66 218,86 170,96 196,134 142,114 134,150 100,118 68,148 70,112 20,126 48,88 6,72 52,58 26,24 76,40';
  s(starSvg, 'polygon', { points: burstPts, transform: 'translate(6, 6)', fill: '#0a0a0e' });
  s(starSvg, 'polygon', { points: burstPts, fill: '#ffe600', stroke: '#0a0a0e', 'stroke-width': 3.5, 'stroke-linejoin': 'miter' });
  s(starSvg, 'polygon', { points: '104,24 118,46 148,34 140,58 174,56 148,78 166,102 128,94 114,122 94,96 64,108 74,82 40,74 72,62 60,38 90,50', fill: '#fff8bf' });

  const ws = h(root, 'div', 'weak-stamp');
  h(ws, 'div', 'weak-word', 'WEAK!!');
  h(ws, 'div', 'one-more', '1 MORE!');

  const dp = h(root, 'div', 'dmg-pop');
  h(dp, 'div', 'dmg-crit', 'CRITICAL');
  h(dp, 'div', 'dmg-num', '2,480');
})();
