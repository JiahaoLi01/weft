// The effects page (effects-demo.css, docs/visual-effects.md and docs/img.md): clip-path shapes that take taps only
// inside them, mask-image gradients and image masks, mix-blend-mode over a picture, and images drawn smaller than
// they are (mipmaps) or larger and pixelated. Plain DOM, as React would build it; the images are this folder's PNGs.
(() => {
  const root = document.getElementById('root');
  const el = (parent, tag, cls, text, attributes = {}) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    for (const [k, v] of Object.entries(attributes)) e.setAttribute(k, v);
    parent.appendChild(e);
    return e;
  };
  const card = (parent, label) => {
    const c = el(parent, 'div', 'card');
    el(c, 'div', 'label', label);
    return c;
  };
  const section = (title) => {
    el(root, 'div', 'section', title);
    return el(root, 'div', 'row');
  };

  el(root, 'div', 'title', 'Visual effects');
  const status = el(root, 'div', 'status', 'Tap a shape: only taps inside it count.');
  const taps = new Map();
  const tapped = (e, name) => {
    e.addEventListener('click', () => {
      taps.set(name, (taps.get(name) || 0) + 1);
      status.textContent = `Tapped ${name} (${taps.get(name)})`;
    });
  };

  // clip-path: the basic shapes, polygon() under both fill rules, path(), and a transition between two insets.
  const clips = section('clip-path');
  const shapes = [
    ['inset round', 'inset(12px round 28px)'],
    ['circle', 'circle(52px)'],
    ['ellipse', 'ellipse(48% 34% at 50% 50%)'],
    ['star nonzero', 'polygon(50% 2%, 61% 36%, 97% 36%, 68% 57%, 79% 92%, 50% 71%, 21% 92%, 32% 57%, 3% 36%, 39% 36%)'],
    ['pentagram evenodd', 'polygon(evenodd, 50% 2%, 79% 92%, 3% 36%, 97% 36%, 21% 92%)'],
    ['heart path', 'path("M70,104 C38,80 6,62 6,34 C6,16 20,6 36,6 C52,6 64,16 70,30 C76,16 88,6 104,6 C120,6 134,16 134,34 C134,62 102,80 70,104 Z")'],
  ];
  for (const [name, value] of shapes) {
    const shape = el(card(clips, name), 'div', 'shape', undefined, {style: 'clip-path:' + value});
    tapped(shape, name);
  }
  const opening = el(card(clips, 'transition (tap)'), 'div', 'shape opening');
  opening.addEventListener('click', () => {
    opening.classList.toggle('open');
    status.textContent = opening.classList.contains('open') ? 'Opened the inset' : 'Closed the inset';
  });

  // mask-image: gradients (alpha and luminance), tiles, two layers intersected, and an image mask by alpha and by
  // luminance.
  const masks = section('mask-image');
  const masked = [
    ['linear fade', 'mask-image:linear-gradient(to bottom, #000 35%, transparent)'],
    ['radial spotlight', 'mask-image:radial-gradient(circle, #000 30%, transparent 68%)'],
    ['repeating stripes', 'mask-image:repeating-linear-gradient(45deg, #000 0 8px, transparent 8px 16px)'],
    ['tiled dots', 'mask-image:radial-gradient(circle, #000 50%, transparent 54%);mask-size:20px 20px'],
    ['intersect', 'mask-image:linear-gradient(90deg, #000, transparent), radial-gradient(circle, #000 55%, transparent 58%);mask-composite:intersect'],
    ['image, alpha', 'mask-image:url(icon.png);mask-size:36px 36px'],
    ['image, luminance', 'mask-image:url(lum.png);mask-mode:luminance;mask-size:100% 100%'],
  ];
  for (const [name, style] of masked) {
    tapped(el(card(masks, name), 'div', 'picture', undefined, {style}), name);
  }

  // mix-blend-mode: a disc of each mode over the picture, the separable modes and the non-separable ones.
  const blends = section('mix-blend-mode');
  for (const mode of ['multiply', 'screen', 'overlay', 'difference', 'color-dodge', 'hue', 'luminosity']) {
    const backdrop = el(card(blends, mode), 'div', 'picture');
    el(backdrop, 'div', 'disc', undefined, {style: 'mix-blend-mode:' + mode});
  }

  // Images drawn at a quarter and an eighth of their size (stripes 256 px, the photo 240 x 160): mipmaps keep them
  // smooth. The 8 x 8 image twelve times larger: smooth, and pixelated.
  const images = section('images');
  el(card(images, 'stripes, 1/4'), 'img', 'quarter', undefined, {src: 'stripes.png'});
  el(card(images, 'stripes, 1/8'), 'img', 'eighth', undefined, {src: 'stripes.png'});
  el(card(images, 'photo, 1/4'), 'img', 'photo-quarter', undefined, {src: 'photo.png'});
  el(card(images, 'photo, 1/2'), 'img', '', undefined, {src: 'photo.png', width: '120'});
  el(card(images, '8 px, smooth'), 'img', 'big', undefined, {src: 'pixel.png'});
  el(card(images, '8 px, pixelated'), 'img', 'big pixelated', undefined, {src: 'pixel.png'});
})();
