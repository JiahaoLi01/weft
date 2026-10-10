// Browser-only independent fixture. The normal app remains interactive.
if (new URLSearchParams(location.search).has('probe')) {
  (async () => {
  const sleep = () => new Promise(resolve => setTimeout(resolve, 20));
  const frame = () => new Promise(resolve => requestAnimationFrame(resolve));
  try {
    for (let i = 0; i < 200 && !document.getElementById('page'); i++) await sleep();
    if (!document.getElementById('page')) throw Error('React did not mount');
    await document.fonts.load('16px ProbeRoboto');
    await document.fonts.ready;
    await frame();
    const capture = () => ({
      state: caseElephantReact.snapshot(),
      layout: { boxes: [document.getElementById('page'), ...document.getElementById('page').querySelectorAll('*')].map(node => {
        const b = node.getBoundingClientRect();
        return { id: node.id, x: b.x, y: b.y, width: b.width, height: b.height };
      }) },
    });
    const ember = document.getElementById('item-ember');
    const initial = capture();
    document.getElementById('increment').dispatchEvent(new Event('click', { bubbles: true }));
    await frame(); await sleep();
    const count = capture();
    document.getElementById('reverse').dispatchEvent(new Event('click', { bubbles: true }));
    await frame(); await sleep();
    const reordered = capture();
    const report = {
      fixture: 'caseelephant-react-vite-v1', react: caseElephantReact.version, vite: '8.3.3',
      frames: [initial, count, reordered], keyed_identity_preserved: ember === document.getElementById('item-ember'),
      font_loaded: document.fonts.check('16px ProbeRoboto'), dpr: devicePixelRatio, errors: caseElephantReact.errors,
    };
    const pre = document.createElement('pre'); pre.id = 'ce-reference'; pre.style.display = 'none'; pre.textContent = JSON.stringify(report); document.body.appendChild(pre);
    document.documentElement.setAttribute('data-probe-done', 'true');
  } catch (error) {
    document.documentElement.setAttribute('data-probe-error', String(error));
  }
  })();
}
