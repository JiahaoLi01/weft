// Shared unchanged native-DOM / Chromium input geometry fixture.
(() => {
  function node(tag, id, parent, text = '') {
    const e = document.createElement(tag); e.id = id;
    if (text) e.textContent = text; parent.appendChild(e); return e;
  }
  const root = node('div', 'inputpage', document.getElementById('surface') || document.body);
  const high = node('button', 'high', root, 'Upper');
  node('button', 'low', root, 'Lower');
  const scroll = node('div', 'scroller', root);
  const content = node('div', 'content', scroll);
  for (let i = 0; i < 12; ++i) {
    const row = node('button', `row-${i}`, content);
    node('span', `label-${i}`, row, `仓库 ${i} / Depot`);
  }
  const overlay = node('div', 'overlay', root);
  node('span', 'overlay-child', overlay, 'Overlay');
  const text = node('div', 'paragraph', root);
  text.appendChild(document.createTextNode('起点 '));
  const outer = node('span', 'outer', text, '仓库运输仓库');
  node('span', 'inner', outer, ' alerts / status');
  text.appendChild(document.createTextNode(' 终点仓库运输仓库运输'));
  const disabled = node('button', 'disabled', root);
  disabled.setAttribute('disabled', ''); node('span', 'disabled-child', disabled, 'Disabled');
  node('button', 'hidden', root, 'Hidden');
  node('button', 'round', root, 'Ellipse');
  node('button', 'edge', root, 'Outside');
  let frame = 0;
  globalThis.inputScenario = { clicks: [], wheels: [], blockWheel: false, advance() {
    if (++frame === 1) {
      scroll.scrollTop = 59.25; scroll.scrollLeft = 17.5;
      high.style.display = 'none'; overlay.style.pointerEvents = 'auto';
      document.getElementById('outer').style.fontSize = '20px';
    } else {
      for (let i = 0; i < 9; ++i) { const e = document.getElementById(`row-${i}`); if (e) e.parentNode.removeChild(e); }
      overlay.parentNode.removeChild(overlay);
      high.style.display = 'block'; document.getElementById('low').style.pointerEvents = 'none';
      document.getElementById('round').style.borderRadius = '12px 35px / 30px 8px';
    }
  }};
  root.addEventListener('click', e => inputScenario.clicks.push(e.target.id));
  root.addEventListener('wheel', e => {
    inputScenario.wheels.push([e.target.id, e.deltaX, e.deltaY, e.deltaMode, e.clientX, e.clientY,
      e.screenX, e.screenY, e.ctrlKey, e.shiftKey, e.altKey, e.metaKey, e.timeStamp, e.isTrusted, e instanceof WheelEvent]);
    if (inputScenario.blockWheel) e.preventDefault();
  });
})();
