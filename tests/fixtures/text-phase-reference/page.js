// "H" at 16 phases of a CSS px (i / 16) in cells 40 px apart: the first row moves its top, the second its left.
(function () {
  const root = document.getElementById('root');
  for (let row = 0; row < 2; ++row) {
    for (let i = 0; i < 16; ++i) {
      const cell = document.createElement('div');
      cell.className = 'cell';
      cell.textContent = 'H';
      const left = 10 + i * 40 + (row === 1 ? i / 16 : 0);
      const top = 10 + row * 60 + (row === 0 ? i / 16 : 0);
      cell.setAttribute('style', 'left:' + left + 'px;top:' + top + 'px');
      root.appendChild(cell);
    }
  }
})();
