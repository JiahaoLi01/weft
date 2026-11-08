// The img page (img-demo.css): plain DOM, as React would build it. Its images are PNG, JPEG and WebP files, and the
// badge again inline as a data: URL (as a bundler inlines a small image).
const BADGE = 'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAyklEQVR42mNgGAVUAr9OaCwD4ldAfASIrehpsRgQ/0fDP+jpgP84cAY9LD+OxwF7KDE4G4jXA/FWIK4FYkEsalzwWA7CL8i1fBMeQ/OBmA2ItQlYDsLvyLFcmQiDicWXyXGAHBUdsJ7aqZpuDmiloiM6gVhqIEMBGV8EYidiHcBBI0eA8HMgliTGEY40dMR3kCeJccRcGjqiaHA7YECjYMAT4YBmwwEviAa0KB7wymjAq+MBb5AMiibZoGiUDplm+aDomAx412zIAwAM9jEIcvCV9AAAAABJRU5ErkJggg==';
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
  el(root, 'div', 'title', 'Images');

  const sizes = el(root, 'div', 'row');
  const natural = card(sizes, 'PNG, natural size');
  const first = el(natural, 'img', '', undefined, {src: 'landscape.png'});
  const report = el(natural, 'div', 'loaded', 'loading…');
  first.addEventListener('load', () => {
    report.textContent = `loaded: ${first.naturalWidth} x ${first.naturalHeight}, complete ${first.complete}`;
  });
  el(card(sizes, 'JPEG, width="120"'), 'img', '', undefined, {src: 'landscape.jpg', width: '120'});
  const avatar = card(sizes, 'avatar, cover');
  el(avatar, 'img', 'avatar', undefined, {src: 'portrait.png'});

  const fits = el(root, 'div', 'row');
  for (const fit of ['fill', 'contain', 'cover', 'none', 'scale-down']) {
    const c = card(fits, 'object-fit: ' + fit);
    el(c, 'img', 'fit', undefined, {src: fit === 'none' ? 'landscape.png' : 'portrait.png', style: 'object-fit:' + fit});
  }

  const more = el(root, 'div', 'row');
  const line = card(more, 'in a line of text: PNG, WebP, data: URL');
  const text = el(line, 'div', '');
  const badge = (src) => el(text, 'img', '', undefined, {src, style: 'vertical-align:middle'});
  text.appendChild(document.createTextNode('Collect the badges '));
  badge('badge.png');
  text.appendChild(document.createTextNode(' '));
  badge('badge.webp');
  text.appendChild(document.createTextNode(' '));
  badge(BADGE);
  text.appendChild(document.createTextNode(' to unlock the next level.'));
  const responsive = card(more, 'max-width: 100% in 180px');
  el(el(responsive, 'div', 'narrow'), 'img', 'responsive', undefined, {src: 'landscape.png', width: '240', height: '160'});
  const positioned = card(more, 'object-position: right top');
  el(positioned, 'img', 'fit', undefined, {src: 'landscape.png', style: 'object-fit:none;object-position:right top'});
})();
