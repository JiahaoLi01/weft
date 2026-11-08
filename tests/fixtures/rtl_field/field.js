// Text fields of right-to-left and mixed text (tools/browser_rtl_field_reference.mjs, tests/scene_rtl_field_tests.cpp):
// each at its own place, its caret green and its selection blue, so that a screenshot shows where they are.
(() => {
  const root = document.getElementById('root');
  const fields = [
    ['input', 'rtl', 'שלום עולם'],
    ['input', 'rtl', 'שלום abc 12'],
    ['input', '', 'abc שלום def'],
    ['input', 'rtl', 'مرحبا بالعالم'],
    ['input', 'rtl', 'abc def'],
    ['textarea', 'rtl', 'שלום עולם\nשורה שנייה'],
  ];
  fields.forEach(([tag, dir, value], i) => {
    const e = document.createElement(tag);
    e.className = 'field ' + tag;
    if (dir) e.setAttribute('dir', dir);
    e.style.top = (10 + i * 50) + 'px';
    e.value = value;
    root.appendChild(e);
  });
})();
