// The bidi page (bidi-demo.css): plain DOM.
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
  const text = (parent, s) => parent.appendChild(document.createTextNode(s));
  el(root, 'div', 'title', 'Bidirectional text and complex scripts');
  const card = (label, attributes = {}) => {
    const c = el(root, 'div', 'card', undefined, attributes);
    el(c, 'div', 'label', label);
    return c;
  };
  el(card('Arabic, dir=rtl', {dir: 'rtl'}), 'div', '',
    'مرحبا بالعالم! هذه فقرة عربية تلتف على أكثر من سطر، وفيها أرقام ١٢٣ و 456 وكلمة English في وسطها.');
  el(card('Hebrew, dir=rtl', {dir: 'rtl'}), 'div', '',
    'שלום עולם, גרסה 2.0 של התוכנה (beta) זמינה עכשיו!');
  const mixed = el(card('Left to right with right-to-left phrases'), 'div', '');
  text(mixed, 'The word ');
  el(mixed, 'span', 'name', 'كتاب');
  text(mixed, ' means "book", and ');
  el(mixed, 'span', 'name', 'שלום');
  text(mixed, ' means "peace".');
  const users = el(card('User names isolated with <bdi>'), 'div', '');
  for (const [name, posts] of [['إيمان', 3], ['Bob', 5], ['דני', 12]]) {
    const line = el(users, 'div', '');
    text(line, 'User ');
    el(line, 'bdi', 'name', name);
    text(line, ': ' + posts + ' posts');
  }
  const chat = card('Chat lines, dir=auto');
  for (const message of ['Hello, how are you?', 'أنا بخير، شكرا!', 'מה שלומך היום?', '42 is the answer']) {
    el(chat, 'div', 'chat', message, {dir: 'auto'});
  }
  const boxes = card('Right to left: a row of boxes (flex), a truncated title, a field', {dir: 'rtl'});
  const row = el(boxes, 'div', 'row');
  for (const name of ['ראשון', 'שני', 'שלישי']) el(row, 'span', 'badge', name);
  el(boxes, 'div', 'title-line', 'כותרת ארוכה מאוד שלא נכנסת בשורה אחת ונחתכת בשלוש נקודות');
  el(boxes, 'input', 'field').value = 'שלום abc 123';
  el(card('Thai'), 'div', '', 'ภาษาไทย สวัสดีครับ ที่นี่ กรุงเทพฯ');
  el(card('Hindi (Devanagari)'), 'div', '', 'नमस्ते दुनिया, यह क्षेत्र हिन्दी में लिखा गया है।');
})();
