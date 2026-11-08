// The touch page (touch-demo.css): plain DOM. Every panel logs what it gets to the log panel.
(() => {
  const root = document.getElementById('root');
  const el = (parent, cls, text) => {
    const e = document.createElement('div');
    e.className = cls;
    if (text !== undefined) e.textContent = text;
    parent.appendChild(e);
    return e;
  };
  const head = el(root, 'head');
  el(head, 'title', 'Touch');
  const devices = el(head, 'devices');
  const showDevices = () => {
    const q = x => matchMedia(x).matches;
    devices.textContent = 'pointer: ' + (q('(pointer: coarse)') ? 'coarse' : q('(pointer: fine)') ? 'fine' : 'none') +
      ' · hover: ' + (q('(hover: hover)') ? 'hover' : 'none') + ' · maxTouchPoints: ' + navigator.maxTouchPoints +
      ' · ontouchstart: ' + ('ontouchstart' in window);
  };
  showDevices();
  matchMedia('(pointer: coarse)').addListener(showDevices);
  const grid = el(root, 'grid');
  const panel = label => {
    const p = el(grid, 'panel');
    el(p, 'label', label);
    return p;
  };
  const lines = [];
  let logBox;
  const log = text => {
    lines.unshift(text);
    lines.length = Math.min(lines.length, 15);
    if (logBox) logBox.textContent = lines.join('\n');
  };

  // Taps: clicks with their detail, double taps; hover after a tap.
  const taps = panel('Tap: click (detail counts taps), dblclick on the second; :active while it clicks');
  const tapRow = el(taps, 'row');
  const button = el(tapRow, 'button', 'Tap me');
  const counts = el(tapRow, 'counts', 'clicks 0 · double taps 0');
  let clicks = 0, doubles = 0;
  button.addEventListener('click', e => {
    clicks++;
    counts.textContent = 'clicks ' + clicks + ' (detail ' + e.detail + ', ' + e.pointerType + ') · double taps ' + doubles;
    log('click detail ' + e.detail + ' ' + e.pointerType);
  });
  button.addEventListener('dblclick', () => {
    doubles++;
    counts.textContent = 'clicks ' + clicks + ' · double taps ' + doubles;
    log('dblclick');
  });
  const hover = panel('Hover after a tap: the left box has :hover, the right one :hover inside @media (hover: hover)');
  const hoverRow = el(hover, 'row');
  el(hoverRow, 'hoverbox plain', ':hover');
  el(hoverRow, 'hoverbox guarded', '(hover: hover)');

  // A list that pans and flings.
  const listPanel = panel('Pan and fling: the list scrolls with the finger and flings on');
  const list = el(listPanel, 'list');
  for (let i = 1; i <= 60; i++) el(list, 'item', 'Row ' + i);
  const listState = el(listPanel, 'label', 'scrollTop 0');
  list.addEventListener('scroll', () => { listState.textContent = 'scrollTop ' + Math.round(list.scrollTop); });

  // Cards across: touch-action pan-x, so a drag down on them is no pan.
  const stripPanel = panel('Across: touch-action: pan-x (drag the cards sideways)');
  const strip = el(stripPanel, 'strip');
  const cards = el(strip, 'cards');
  for (let i = 1; i <= 12; i++) el(cards, 'card', String(i));

  // Every finger on the pad: a dot per pointer (touch-action: none, so the page keeps the pointers).
  const padPanel = panel('Fingers: touch-action: none, a dot per pointer (try several fingers)');
  const pad = el(padPanel, 'pad');
  const padState = el(padPanel, 'label', 'pointers 0 · touches 0');
  const dots = new Map();
  const colours = ['#f2b23a', '#4fd18b', '#8bc5ff', '#ff7a7a', '#c58bff', '#ffd166', '#7ae7ff', '#ff9ad5', '#b8f27a', '#ffffff'];
  let touches = 0;
  const place = (e) => {
    const box = pad.getBoundingClientRect();
    const d = dots.get(e.pointerId);
    if (d) d.style.transform = 'translate(' + (e.clientX - box.left - 28) + 'px, ' + (e.clientY - box.top - 28) + 'px)';
  };
  const showPad = () => { padState.textContent = 'pointers ' + dots.size + ' · touches ' + touches; };
  pad.addEventListener('pointerdown', e => {
    const d = el(pad, 'dot', String(e.pointerId));
    d.style.backgroundColor = colours[dots.size % colours.length];
    dots.set(e.pointerId, d);
    place(e);
    showPad();
    log('pointerdown ' + e.pointerId + (e.isPrimary ? ' primary' : '') + ' ' + e.pointerType);
  });
  pad.addEventListener('pointermove', place);
  const lift = e => {
    const d = dots.get(e.pointerId);
    if (d) { d.remove(); dots.delete(e.pointerId); }
    showPad();
    log(e.type + ' ' + e.pointerId);
  };
  pad.addEventListener('pointerup', lift);
  pad.addEventListener('pointercancel', lift);
  for (const t of ['touchstart', 'touchmove', 'touchend', 'touchcancel']) {
    pad.addEventListener(t, e => { touches = e.touches.length; showPad(); });
  }

  // A slider that captures its pointer: the knob follows the finger off the track too.
  const sliderPanel = panel('Slider: setPointerCapture keeps the drag when the finger leaves the track');
  const track = el(sliderPanel, 'track');
  el(track, 'rail');
  const knob = el(track, 'knob');
  const value = el(sliderPanel, 'label', 'value 0');
  let dragging = null;
  const slide = e => {
    const box = track.getBoundingClientRect();
    const x = Math.max(0, Math.min(box.width - 44, e.clientX - box.left - 22));
    knob.style.transform = 'translateX(' + x + 'px)';
    value.textContent = 'value ' + Math.round(100 * x / (box.width - 44)) + (track.hasPointerCapture(e.pointerId) ? ' (captured)' : '');
  };
  track.addEventListener('pointerdown', e => { dragging = e.pointerId; track.setPointerCapture(e.pointerId); slide(e); });
  track.addEventListener('pointermove', e => { if (dragging === e.pointerId) slide(e); });
  track.addEventListener('pointerup', e => { if (dragging === e.pointerId) { dragging = null; log('slider let go'); } });
  track.addEventListener('lostpointercapture', () => log('slider lostpointercapture'));

  const logPanel = panel('Log');
  logBox = el(logPanel, 'log');
  logBox.style.whiteSpace = 'pre-line';
  log('ready');
})();
