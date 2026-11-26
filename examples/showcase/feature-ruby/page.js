(() => {
  const root = document.getElementById('root');
  const h = (p, t, c, txt) => {
    const e = document.createElement(t);
    if (c) e.className = c;
    if (txt !== undefined) e.textContent = txt;
    p.appendChild(e);
    return e;
  };
  const addRuby = (p, pairs) => {
    const rb = document.createElement('ruby');
    for (const [b, r] of pairs) {
      rb.appendChild(document.createTextNode(b));
      const rt = document.createElement('rt');
      rt.textContent = r;
      rb.appendChild(rt);
    }
    p.appendChild(rb);
  };
  const tx = (p, s) => p.appendChild(document.createTextNode(s));

  // Left: Japanese Furigana <ruby>
  const cJp = h(root, 'div', 'chip chip-jp');
  h(cJp, 'span', 'tag tag-gold', '<ruby> · JP');
  const jp = h(cJp, 'div', 'jp-line');
  tx(jp, '「');
  const f = h(jp, 'span', 'hl-fire');
  addRuby(f, [['紅', 'ぐ'], ['蓮', 'れん']]);
  tx(jp, 'の');
  addRuby(jp, [['聖', 'せい'], ['剣', 'けん']]);
  tx(jp, 'を');
  addRuby(jp, [['掲', 'かか']]);
  tx(jp, 'げ、');
  addRuby(jp, [['深', 'しん'], ['淵', 'えん']]);
  tx(jp, 'の');
  addRuby(jp, [['魔', 'ま'], ['王', 'おう']]);
  tx(jp, 'を');
  addRuby(jp, [['討', 'う']]);
  tx(jp, 'つ！」');

  // Right: Chinese Pinyin <ruby>
  const cCn = h(root, 'div', 'chip chip-cn');
  h(cCn, 'span', 'tag', '<ruby> · ZH');
  const cn = h(cCn, 'div', 'cn-line');
  const cg = h(cn, 'span', 'cn-gold');
  addRuby(cg, [['饕', 'tāo '], ['餮', 'tiè']]);
  tx(cn, '青铜');
  addRuby(cn, [['鼎', 'dǐng']]);
  tx(cn, ' · ');
  addRuby(cn, [['魑', 'chī '], ['魅', 'mèi '], ['魍', 'wǎng '], ['魉', 'liǎng']]);
})();
