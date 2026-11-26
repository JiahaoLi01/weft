(() => {
  const root = document.getElementById('root');
  const h = (p, t, c, txt, attrs = {}) => {
    const e = document.createElement(t);
    if (c) e.className = c;
    if (txt !== undefined) e.textContent = txt;
    for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, String(v));
    p.appendChild(e);
    return e;
  };

  // Left: Compact Arabic RTL Quest Strip (dir="rtl" + mixed LTR + RTL progress + RTL ellipsis)
  const ar = h(root, 'div', 'chip chip-ar', undefined, { dir: 'rtl' });
  const top = h(ar, 'div', 'ar-top');
  const bdg = h(top, 'div', 'ar-badges');
  h(bdg, 'span', 'ar-pill', 'مهمة رئيسية');
  h(bdg, 'span', 'ar-title', 'استعادة نصل الرماد الأسطوري');
  h(top, 'span', 'ar-xp', '+2,400 XP', { dir: 'ltr' });

  const line = h(ar, 'div', 'ar-line');
  line.appendChild(document.createTextNode('اهزم حارس اللهب ('));
  h(line, 'span', 'ar-kw', 'Boss LVL 42');
  line.appendChild(document.createTextNode(') لاستعادة السيف الأسطوري Flamebrand +12 في صحراء النجوم البعيدة والمنسية'));

  const prog = h(ar, 'div', 'rtl-bar-row');
  const tr = h(prog, 'div', 'rtl-track');
  h(tr, 'div', 'rtl-fill');
  h(prog, 'span', 'rtl-num', '3 / 4 · RTL', { dir: 'ltr' });

  // Right: <bdi> + Hebrew RTL + Thai / Hindi HarfBuzz
  const mu = h(root, 'div', 'chip chip-multi');

  const r1 = h(mu, 'div', 'srow');
  h(r1, 'span', 'scode', 'BDI');
  const t1 = h(r1, 'div', 'stxt');
  h(t1, 'bdi', 'bdi-tag', 'إيمان');
  t1.appendChild(document.createTextNode('  أنا بخير، شكراً! · Ready'));

  const r2 = h(mu, 'div', 'srow');
  h(r2, 'span', 'scode', 'HE');
  h(r2, 'div', 'stxt', 'שלום עולם, גרסה 2.0 (beta) זמינה עכשיו!', { dir: 'rtl' });

  const r3 = h(mu, 'div', 'srow');
  h(r3, 'span', 'scode scode-g', 'TH/HI');
  h(r3, 'div', 'stxt', 'ภาษาไทย กรุงเทพฯ  ·  हिन्दी देवनागरी');
})();
