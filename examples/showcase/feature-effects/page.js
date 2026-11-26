(() => {
  const root = document.getElementById('root');
  const h = (p, t, c, txt) => {
    const e = document.createElement(t);
    if (c) e.className = c;
    if (txt !== undefined) e.textContent = txt;
    p.appendChild(e);
    return e;
  };
  const tile = (label) => {
    const t = h(root, 'div', 'tile');
    h(t, 'span', 'lbl', label);
    return t;
  };

  // 1. backdrop-filter
  const t1 = tile('backdrop-filter');
  h(t1, 'div', 'bf-c1');
  h(t1, 'div', 'bf-c2');
  h(t1, 'div', 'bf-glass', 'blur(12px)');

  // 2. clip-path
  const t2 = tile('clip-path');
  const r2 = h(t2, 'div', 'cp-row');
  h(r2, 'div', 'cp-hex', 'HEX');
  h(r2, 'div', 'cp-bar', '88%');

  // 3. mask-image
  const t3 = tile('mask-image');
  const c3 = h(t3, 'div', 'mk-col');
  const cp = h(c3, 'div', 'mk-comp');
  for (const m of ['345', 'N', '015', 'NE', '060']) h(cp, 'span', '', m);
  h(c3, 'div', 'mk-rad');

  // 4. mix-blend-mode
  const t4 = tile('mix-blend-mode');
  const bw = h(t4, 'div', 'bl-wrap');
  h(bw, 'div', 'orb orb-r');
  h(bw, 'div', 'orb orb-g');
  h(bw, 'div', 'orb orb-b');

  // 5. conic-gradient
  const t5 = tile('conic-gradient');
  const r5 = h(t5, 'div', 'cg-row');
  const b5 = h(r5, 'div', 'cd-box');
  h(b5, 'div', 'cd-sw');
  h(b5, 'span', 'cd-num', '4.2s');
  const rg = h(r5, 'div', 'cd-ring');
  h(rg, 'div', 'cd-hole', '72%');
})();
