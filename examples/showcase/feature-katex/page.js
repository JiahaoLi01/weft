(() => {
  const SVG_NS = 'http://www.w3.org/2000/svg';
  const root = document.getElementById('root');

  const st = (el, styles) => {
    for (const [k, v] of Object.entries(styles)) el.style[k] = v;
    return el;
  };
  const h = (p, t, styles, txt) => {
    const e = document.createElement(t);
    if (styles) st(e, styles);
    if (txt !== undefined) e.textContent = txt;
    p.appendChild(e);
    return e;
  };
  const s = (p, t, attrs = {}) => {
    const e = document.createElementNS(SVG_NS, t);
    for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, String(v));
    p.appendChild(e);
    return e;
  };

  st(document.body, {
    margin: '0', padding: '0', width: '920px', height: '118px',
    overflow: 'hidden', backgroundColor: '#0d1017', color: '#f4f4f5',
    fontFamily: '"Segoe UI", sans-serif',
  });
  st(root, {
    boxSizing: 'border-box', width: '920px', height: '118px',
    padding: '10px 12px', backgroundColor: '#0d1017',
    border: '1px solid #242936', borderRadius: '8px',
    display: 'flex', gap: '10px', color: '#f8fafc',
  });

  const formulas = [
    {
      tag: 'KaTeX', col: '#fbbf24', bd: 'rgba(251, 191, 36, 0.30)', flex: '1.15',
      tex: String.raw`D_{\text{eff}} = D_0 \cdot \frac{100}{100 + A(1 - p)}`,
    },
    {
      tag: 'Spring', col: '#38bdf8', bd: 'rgba(56, 189, 248, 0.30)', flex: '1.15',
      tex: String.raw`\theta(t) = \frac{J}{\omega_d}\,e^{-\zeta\omega t}\sin(\omega_d t)`,
    },
    {
      tag: 'Elo', col: '#a78bfa', bd: 'rgba(167, 139, 250, 0.30)', flex: '1',
      tex: String.raw`E_A = \frac{1}{1 + 10^{(R_B - R_A)/400}}`,
    },
  ];

  for (const f of formulas) {
    const chip = h(root, 'div', {
      position: 'relative', boxSizing: 'border-box', flex: f.flex,
      borderRadius: '6px', backgroundColor: '#131722', border: '1px solid ' + f.bd,
      display: 'flex', alignItems: 'center', justifyContent: 'center',
      padding: '14px 10px 6px 10px', fontSize: '13.5px', color: '#f8fafc', overflow: 'hidden',
    });
    katex.render(f.tex, chip, { output: 'html', throwOnError: false, displayMode: true });
    const kd = chip.querySelector('.katex-display');
    if (kd) st(kd, { marginTop: '0', marginBottom: '0' });
    h(chip, 'span', {
      position: 'absolute', left: '10px', top: '6px',
      fontFamily: '"JetBrains Mono", monospace', fontSize: '9px',
      fontWeight: '700', letterSpacing: '0.08em', color: f.col,
    }, f.tag);
  }

  // Mini SVG curve chip on right (D3/SVG native vector)
  const svgChip = h(root, 'div', {
    position: 'relative', boxSizing: 'border-box', width: '132px', flexShrink: '0',
    borderRadius: '6px', backgroundColor: '#131722', border: '1px solid rgba(52, 211, 153, 0.32)',
    display: 'flex', alignItems: 'flex-end', justifyContent: 'center', padding: '8px',
  });
  h(svgChip, 'span', {
    position: 'absolute', left: '9px', top: '6px',
    fontFamily: '"JetBrains Mono", monospace', fontSize: '9px',
    fontWeight: '700', letterSpacing: '0.08em', color: '#34d399',
  }, 'SVG');
  const svg = s(svgChip, 'svg', { width: 112, height: 68, viewBox: '0 0 112 68' });
  const pts = [];
  for (let i = 0; i <= 24; i++) {
    const t = i / 24;
    const y = Math.max(0.08, 0.45 + 0.65 * Math.exp(-3.1 * t) * Math.sin(13.0 * t) * 0.72);
    pts.push([4 + t * 104, 62 - y * 52]);
  }
  s(svg, 'line', { x1: 4, y1: 62, x2: 108, y2: 62, stroke: '#262d3f', 'stroke-width': 1 });
  s(svg, 'polygon', {
    points: '4,62 ' + pts.map(p => `${p[0].toFixed(1)},${p[1].toFixed(1)}`).join(' ') + ' 108,62',
    fill: 'rgba(52, 211, 153, 0.18)',
  });
  s(svg, 'polyline', {
    points: pts.map(p => `${p[0].toFixed(1)},${p[1].toFixed(1)}`).join(' '),
    fill: 'none', stroke: '#34d399', 'stroke-width': 1.8,
  });
})();
