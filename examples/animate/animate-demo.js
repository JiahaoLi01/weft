// The animate page (animate-demo.css, docs/web-animations.md). First it runs tests/fixtures/web_animations/
// animations.js's steps in real frames and compares what they log with Chrome's log of them (chrome-log.js), entry by
// entry, and says how many match (and stdout says it too). Then element.animate() at work: animations that run on
// their own (alternate, linear, steps(), keyframes with offsets), one to play, pause, reverse, finish, cancel and
// speed up or slow down by tapping, and animations that replace one another on one element until one is committed.
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
  const button = (parent, text, action) => {
    const b = el(parent, 'div', 'button', text);
    b.addEventListener('click', action);
    return b;
  };
  const page = el(root, 'div', 'page');
  el(page, 'div', 'title', 'element.animate()');
  const check = el(page, 'div', 'check', 'Checking the steps against Chrome…');

  // The steps, then the log against Chrome's, as tests/web_animations_tests.cpp compares them.
  const compare = () => {
    const got = window.__log;
    const want = window.__chromeLog.log;
    const count = Math.max(got.length, want.length);
    let same = 0;
    let first = -1;
    for (let i = 0; i < count; ++i) {
      if (JSON.stringify(got[i]) === JSON.stringify(want[i])) {
        ++same;
      } else if (first < 0) {
        first = i;
      }
    }
    const browser = window.__chromeLog.browser.replace('/', ' ');
    if (first < 0) {
      check.textContent = `${same}/${count} log entries match ${browser}`;
    } else {
      check.className = 'check wrong';
      check.textContent = `${same}/${count} log entries match ${browser}; the first that differs, ${first}: ` +
        `${JSON.stringify(got[first])} where Chrome has ${JSON.stringify(want[first])}`;
    }
    console.log('animate check: ' + check.textContent);
  };

  const running = () => {
    const row = el(page, 'div', 'row');
    const slide = el(el(card(row, 'alternate, ease-in-out'), 'div', 'track short'), 'div', 'ball');
    slide.animate({transform: ['translateX(0px)', 'translateX(200px)']},
      {duration: 1200, iterations: Infinity, direction: 'alternate', easing: 'ease-in-out'});
    const spin = el(card(row, 'linear, infinite'), 'div', 'square');
    spin.animate({transform: ['rotate(0deg)', 'rotate(360deg)']}, {duration: 1600, iterations: Infinity});
    const stepped = el(el(card(row, 'steps(6)'), 'div', 'track short'), 'div', 'ball');
    stepped.animate({transform: ['translateX(0px)', 'translateX(200px)']},
      {duration: 1800, iterations: Infinity, easing: 'steps(6, jump-none)'});
    const pulse = el(card(row, 'offsets, per-keyframe easing'), 'div', 'square');
    pulse.animate([
      {backgroundColor: '#f0c040', transform: 'scale(1)', easing: 'ease-out'},
      {backgroundColor: '#e0508a', transform: 'scale(1.25)', offset: 0.3},
      {backgroundColor: '#6a5cff', transform: 'scale(0.8)', offset: 0.7},
      {backgroundColor: '#f0c040', transform: 'scale(1)'},
    ], {duration: 2400, iterations: Infinity});
  };

  // One animation to drive by tapping, and what it says about itself every frame.
  const control = () => {
    const box = card(page, 'tap to control: 3 s, fill both');
    const ball = el(el(box, 'div', 'track long'), 'div', 'ball');
    const a = ball.animate({transform: ['translateX(0px)', 'translateX(600px)'], opacity: [1, 0.35]},
      {duration: 3000, fill: 'both', easing: 'ease-in-out'});
    const events = [];
    const happened = (what) => {
      events.push(what);
      if (events.length > 6) events.shift();
      log.textContent = 'events: ' + events.join(', ');
    };
    const watch = () => {
      a.finished.then(() => happened('finished resolved'), (e) => happened('finished ' + e.name));
    };
    a.onfinish = () => happened('finish');
    a.oncancel = () => happened('cancel');
    watch();
    const buttons = el(box, 'div', 'buttons');
    const act = (text, f) => button(buttons, text, () => {
      const settled = a.playState === 'finished' || a.playState === 'idle';
      f();
      if (settled && a.playState !== 'finished' && a.playState !== 'idle') watch();
    });
    act('Play', () => a.play());
    act('Pause', () => a.pause());
    act('Reverse', () => a.reverse());
    act('Finish', () => a.finish());
    act('Cancel', () => a.cancel());
    act('× 0.5', () => a.updatePlaybackRate(a.playbackRate * 0.5));
    act('× 2', () => a.updatePlaybackRate(a.playbackRate * 2));
    act('Seek 1.5 s', () => {
      a.currentTime = 1500;
    });
    const state = el(box, 'div', 'state');
    const log = el(box, 'div', 'state', 'events: none yet');
    const show = () => {
      const t = a.currentTime;
      const p = a.effect.getComputedTiming().progress;
      state.textContent = `${a.playState}${a.pending ? ' (pending)' : ''}, current time ` +
        `${t === null ? 'none' : Math.round(t) + ' ms'}, rate ${a.playbackRate}, progress ` +
        `${p === null ? 'none' : p.toFixed(3)}`;
      requestAnimationFrame(show);
    };
    show();
  };

  // Animations on one square, each filling forwards: once one finishes, the ones under it are removed (unless
  // persisted), and Commit keeps the top one's value as the square's own style.
  const replace = () => {
    const box = card(page, 'replace, persist, commitStyles');
    const line = el(box, 'div', 'buttons');
    const square = el(line, 'div', 'square small');
    const colors = ['#f0c040', '#e0508a', '#6a5cff', '#3fb950', '#ff5a36'];
    let next = 0;
    let removed = 0;
    let persisted = false;
    const add = () => {
      const color = colors[next++ % colors.length];
      const a = square.animate({backgroundColor: color, transform: `rotate(${next * 30}deg)`},
        {duration: 400, fill: 'forwards'});
      a.onremove = () => {
        ++removed;
        show();
      };
      if (persisted) a.persist();
      a.finished.then(show, () => {});
      show();
    };
    button(line, 'Add', add);
    const keep = button(line, 'Persist next: off', () => {
      persisted = !persisted;
      keep.textContent = `Persist next: ${persisted ? 'on' : 'off'}`;
    });
    button(line, 'Commit', () => {
      const list = square.getAnimations();
      if (list.length) list[list.length - 1].commitStyles();
      for (const a of list) a.cancel();
      show();
    });
    const state = el(box, 'div', 'state');
    const show = () => {
      state.textContent = `animations on it: ${square.getAnimations().length}, removed: ${removed}, its own style: ` +
        `${square.getAttribute('style') || 'none'}`;
    };
    show();
  };

  window.__run().then(() => {
    compare();
    running();
    control();
    replace();
  });
})();
