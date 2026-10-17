// The weft logo, drawn by the page as one SVG in #root: a woven mark (three warp threads and three weft threads in plain
// weave) and the word "weft" in the same thread, whose f and t share one crossbar that passes over the f and under
// the t. Where a thread passes over another, a band of the background on either side of it parts the thread beneath.
//
// It is SVG strokes only (lines and paths with round caps and joins), so it draws with any font. Animated, the warp
// threads are drawn first, then the weft threads are pulled through one after another like a shuttle's passes, then
// the letters are written and the crossbar is pulled through f and t; it holds its last frame. A variant script sets
// the palette and whether it moves before this one runs:
//
//   globalThis.weftLogo = {palette: 'dark' | 'light' | 'color', animated: true | false};
(function () {
  const options = globalThis.weftLogo || {};
  const palettes = {
    // Light threads for dark backgrounds.
    dark: {background: '#0f1117', warp: '#59607a', weft: ['#f4f1ea', '#f4f1ea', '#f4f1ea'], ink: '#f4f1ea', bar: '#f4f1ea'},
    // Dark threads for light backgrounds.
    light: {background: '#f7f5f0', warp: '#b3b7c4', weft: ['#14161f', '#14161f', '#14161f'], ink: '#14161f', bar: '#14161f'},
    // Indigo warp, amber, coral and teal weft, ink letters with a coral crossbar.
    color: {background: '#faf8f3', warp: '#4f5bd5', weft: ['#f5a524', '#f25f5c', '#25b5a6'], ink: '#1b1e2b', bar: '#f25f5c'},
  };
  const palette = palettes[options.palette] || palettes.color;
  const animated = options.animated !== false;

  // Design units: the thread is 12 wide, a band of 3 parts crossing threads, threads are 28 apart.
  const scale = 2;
  const thread = 12;
  const band = 3;
  const pitch = 28;
  const reach = 14; // how far a thread runs past the last thread it crosses
  const pace = 190; // design units a second a pulled thread travels

  const NS = 'http://www.w3.org/2000/svg';
  const root = document.getElementById('root');
  root.style.backgroundColor = palette.background;
  const svg = document.createElementNS(NS, 'svg');
  svg.setAttribute('class', 'weft-logo');

  const u = v => (v * scale).toFixed(2);

  // A stroke along a path (d in design units, drawn in order), its length in design units for the drawing: revealed
  // over duration seconds after delay seconds with the easing, or shown at once.
  function stroke(d, length, colour, width, cap, motion) {
    const path = document.createElementNS(NS, 'path');
    path.setAttribute('d', d);
    path.setAttribute('fill', 'none');
    path.setAttribute('stroke', colour);
    path.setAttribute('stroke-width', u(width));
    path.setAttribute('stroke-linecap', cap);
    path.setAttribute('stroke-linejoin', 'round');
    if (animated && motion) {
      const dash = u(length + 1);
      path.setAttribute('stroke-dasharray', dash);
      path.setAttribute('style', `stroke-dashoffset:${dash};animation:weft-logo-draw ${motion.duration.toFixed(3)}s ` +
        `${motion.easing} ${motion.delay.toFixed(3)}s 1 forwards`);
    }
    svg.appendChild(path);
    return path;
  }

  // A thread with the band that parts what it passes over: the band first (the background, wider), then the thread.
  function over(d, length, colour, cap, motion) {
    stroke(d, length, palette.background, thread + 2 * band, cap, motion);
    stroke(d, length, colour, thread, cap, motion);
  }

  const line = (x1, y1, x2, y2) => `M ${u(x1)} ${u(y1)} L ${u(x2)} ${u(y2)}`;

  // ---- The mark: warp threads at x = 0, 28, 56, weft threads at y = 0, 28, 56; a weft thread passes over the warp
  // where (warp + weft) is even.
  const markX = 20 + reach + thread / 2;
  const markY = 20 + reach + thread / 2;
  const span = 2 * pitch + 2 * reach;
  const weftStart = j => 0.5 + 0.16 * j;
  const pulled = (start, from, length) => ({delay: start + from / pace, duration: length / pace, easing: 'linear'});

  // The weft threads, whole, beneath everything.
  for (let j = 0; j < 3; ++j) {
    const y = markY + j * pitch;
    stroke(line(markX - reach, y, markX - reach + span, y), span, palette.weft[j], thread, 'round',
           pulled(weftStart(j), 0, span));
  }
  // The warp threads over them, each with its band.
  for (let i = 0; i < 3; ++i) {
    const x = markX + i * pitch;
    over(line(x, markY - reach, x, markY - reach + span), span, palette.warp, 'round',
         {delay: 0.05 + 0.09 * i, duration: 0.5, easing: 'ease-out'});
  }
  // Where a weft thread passes over the warp: a piece of it over the warp thread and its band, pulled through as the
  // whole thread reaches it.
  for (let j = 0; j < 3; ++j) {
    for (let i = 0; i < 3; ++i) {
      if ((i + j) % 2) {
        continue;
      }
      const x = markX + i * pitch;
      const y = markY + j * pitch;
      const half = thread / 2 + band;
      const from = x - half - (markX - reach);
      over(line(x - half, y, x + half, y), 2 * half, palette.weft[j], 'butt', pulled(weftStart(j), from, 2 * half));
    }
  }

  // ---- The word: x-height 44, f 72 and t 62 high, letters in the same thread; its x-height band is centred on the
  // mark's middle.
  const height = 44;
  const base = markY + pitch + height / 2;
  let x = markX + 2 * pitch + reach + thread / 2 + 30;
  const write = (start, duration) => ({delay: start, duration, easing: 'ease-in-out'});

  // w: two vees, the middle peak lower than the x-height.
  {
    const p = [[0, -height], [12.5, 0], [25, -height * 0.74], [37.5, 0], [50, -height]];
    let length = 0;
    for (let k = 1; k < p.length; ++k) {
      length += Math.hypot(p[k][0] - p[k - 1][0], p[k][1] - p[k - 1][1]);
    }
    const d = p.map(([px, py], k) => `${k ? 'L' : 'M'} ${u(x + px)} ${u(base + py)}`).join(' ');
    stroke(d, length, palette.ink, thread, 'round', write(1.1, 0.55));
    x += 50 + 18;
  }
  // e: its bar, then round from the right up over the top to an opening at the lower right.
  {
    const r = height / 2 - thread / 2;
    const cx = x + height / 2;
    const cy = base - height / 2;
    const turn = 70 * Math.PI / 180;
    const d = `M ${u(cx - r)} ${u(cy)} L ${u(cx + r)} ${u(cy)} ` +
              `A ${u(r)} ${u(r)} 0 1 0 ${u(cx + r * Math.cos(turn))} ${u(cy + r * Math.sin(turn))}`;
    stroke(d, 2 * r + r * (2 * Math.PI - turn), palette.ink, thread, 'round', write(1.25, 0.6));
    x += height + 24;
  }
  // f and t: their stems, the crossbar between (over the f, under the t), the t over it.
  const fx = x;
  const tx = fx + 42;
  const hook = 14;
  const cross = base - height;
  {
    const top = base - 72;
    const d = `M ${u(fx)} ${u(base)} L ${u(fx)} ${u(top + hook)} ` +
              `A ${u(hook)} ${u(hook)} 0 0 1 ${u(fx + hook)} ${u(top)} L ${u(fx + hook + 8)} ${u(top)}`;
    stroke(d, 72 - hook + Math.PI / 2 * hook + 8, palette.ink, thread, 'round', write(1.45, 0.5));
  }
  {
    const from = fx - 14;
    const to = tx + 16;
    over(line(from, cross, to, cross), to - from, palette.bar, 'round', pulled(1.95, 0, to - from));
  }
  {
    const top = base - 62;
    const d = `M ${u(tx)} ${u(top)} L ${u(tx)} ${u(base - hook)} ` +
              `A ${u(hook)} ${u(hook)} 0 0 0 ${u(tx + hook)} ${u(base)} L ${u(tx + hook + 6)} ${u(base)}`;
    over(d, 62 - hook + Math.PI / 2 * hook + 6, palette.ink, 'round', write(1.6, 0.45));
  }

  const width = tx + hook + 6 + thread / 2 + 20;
  const bottom = Math.max(markY - reach + span, base) + thread / 2 + 20;
  svg.setAttribute('width', u(width));
  svg.setAttribute('height', u(bottom));
  root.appendChild(svg);
})();
