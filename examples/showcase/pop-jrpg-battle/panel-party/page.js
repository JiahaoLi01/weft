(() => {
  const root = document.getElementById('root');
  const h = (p, t, c, txt, a = {}) => {
    const el = document.createElement(t);
    if (c) el.className = c;
    if (txt !== undefined) el.textContent = txt;
    for (const [k, v] of Object.entries(a)) el.setAttribute(k, String(v));
    p.appendChild(el);
    return el;
  };
  const dock = h(root, 'div', 'party-dock');
  for (const p of [
    { name: 'REN', lv: 'LV.42', hp: '384', hpPct: 91, sp: '168', spPct: 80, img: '../assets/char1.jpg', bg: '#e60023', active: true },
    { name: 'SORA', lv: 'LV.41', hp: '290', hpPct: 93, sp: '245', spPct: 94, img: '../assets/char2.jpg', bg: '#00b4d8', active: false },
    { name: 'HINA', lv: 'LV.42', hp: '412', hpPct: 89, sp: '96', spPct: 68, img: '../assets/char3.jpg', bg: '#ffd60a', active: false },
  ]) {
    const card = h(dock, 'div', 'party-card' + (p.active ? ' active' : ''));
    h(card, 'div', 'party-back');
    const inner = h(card, 'div', 'party-inner');
    const pcut = h(inner, 'div', 'portrait-cut', undefined, { style: 'background-color:' + p.bg });
    h(pcut, 'img', 'portrait-img', undefined, { src: p.img });
    const stats = h(inner, 'div', 'party-stats');
    const ph = h(stats, 'div', 'party-head');
    h(ph, 'div', 'party-name', p.name);
    h(ph, 'div', 'party-lv', p.lv);
    const hpM = h(stats, 'div', 'p-meter');
    const hpTop = h(hpM, 'div', 'p-meter-top');
    h(hpTop, 'span', 'p-tag-hp', 'HP');
    h(hpTop, 'span', 'p-val', p.hp);
    h(h(hpM, 'div', 'p-track'), 'div', 'p-fill-hp', undefined, { style: 'width:' + p.hpPct + '%' });
    const spM = h(stats, 'div', 'p-meter');
    const spTop = h(spM, 'div', 'p-meter-top');
    h(spTop, 'span', 'p-tag-sp', 'SP');
    h(spTop, 'span', 'p-val', p.sp);
    h(h(spM, 'div', 'p-track'), 'div', 'p-fill-sp', undefined, { style: 'width:' + p.spPct + '%' });
    if (p.active) h(card, 'div', 'active-badge', 'ACTIVE');
  }
})();
