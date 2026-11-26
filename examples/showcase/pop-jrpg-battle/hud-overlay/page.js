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

  const slash = h(root, 'div', 'corner-slash-tl');
  h(slash, 'div', 'corner-slash-dots');

  const dc = h(root, 'div', 'date-cluster');
  const dm = h(dc, 'div', 'date-main');
  h(dm, 'div', 'date-num', '11/26');
  h(dm, 'div', 'date-day', 'THU');
  const ph = h(dm, 'div', 'date-phase');
  const moon = s(ph, 'svg', { width: 16, height: 16, viewBox: '0 0 18 18' });
  s(moon, 'path', { d: 'M12.5 2.5 C7.5 3.2 4 7.5 5 12.2 C6 15.8 10.5 17 14 14.5 C10.4 14.8 7.8 12.4 7.8 9 C7.8 6 9.8 3.6 12.5 2.5 Z', fill: '#ffe600' });
  h(ph, 'span', '', 'NIGHT');

  const turn = h(dc, 'div', 'turn-banner');
  h(turn, 'span', 'turn-tag', 'TURN 03');
  h(turn, 'span', '', 'VELVET ARENA // 3D IN-GAME UI');

  const ctrls = h(root, 'div', 'control-strip');
  for (const [k, label] of [['A', 'CONFIRM'], ['B', 'BACK'], ['RB', 'ANALYZE'], ['LB', 'BATON PASS']]) {
    const pill = h(ctrls, 'div', 'ctrl-pill');
    h(pill, 'span', 'ctrl-btn', k);
    h(pill, 'span', '', label);
  }
})();
