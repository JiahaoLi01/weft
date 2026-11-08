// Web Animations (element.animate()) as a page sees them, step by step, logged in window.__log: states, timing,
// computed values at chosen times, and the order of events and promises. Only what the frames' times do not change is
// logged: a paused animation's values at set current times, the states animations pass through, and the order things
// happen in. window.__done turns true when the steps are over. tools/browser_web_animations_reference.mjs runs it in
// Chrome; tests/web_animations_tests.cpp in a View.
(() => {
  const log = (...entry) => window.__log.push(entry);
  window.__log = [];
  window.__done = false;
  const root = document.getElementById('root');
  const frame = () => new Promise((resolve) => requestAnimationFrame(() => resolve()));
  const frames = async (n) => {
    for (let i = 0; i < n; ++i) await frame();
  };
  const round = (v) => (typeof v === 'number' && Number.isFinite(v) ? Math.round(v * 1000) / 1000 : v);
  const make = (style = '') => {
    const e = document.createElement('div');
    e.setAttribute('style', 'position:absolute;left:0;top:0;width:10px;height:10px;' + style);
    root.appendChild(e);
    return e;
  };
  // A computed value as numbers: opacity, a length, a color's channels, a transform's matrix.
  const value = (e, property) => {
    const v = getComputedStyle(e)[property];
    const numbers = String(v).match(/-?\d*\.?\d+(e-?\d+)?/g);
    return numbers ? numbers.map((n) => round(Number(n))) : v;
  };
  const at = (animation, time, property) => {
    animation.currentTime = time;
    return value(animation.effect.target, property);
  };
  const timing = (animation) => {
    const t = animation.effect.getComputedTiming();
    return [round(t.localTime), round(t.progress), t.currentIteration, round(t.activeDuration), round(t.endTime)];
  };
  const error = (f) => {
    try {
      f();
      return 'none';
    } catch (e) {
      return e.name;
    }
  };

  const steps = [
    async function basic() {
      const e = make('opacity:0.5');
      const a = e.animate([{opacity: 0}, {opacity: 1}], 1000);
      log('created', a.playState, a.pending, a.currentTime, a.startTime === null, a.effect.target === e,
        a.timeline === document.timeline);
      await a.ready;
      log('ready', a.playState, a.pending, a.startTime !== null);
      a.pause();
      log('pausing', a.playState, a.pending);
      await a.ready;
      log('paused', a.playState, a.pending, a.startTime === null);
      for (const t of [0, 250, 500, 999, 1000, 1500]) log('opacity', t, at(a, t, 'opacity'), timing(a));
      a.cancel();
      log('cancelled', a.playState, a.currentTime, value(e, 'opacity'));
    },
    async function formats() {
      const e = make('opacity:1;width:10px;left:0px;background-color:#ff0000');
      const indexed = e.animate({opacity: [0, 1], width: ['10px', '110px']}, {duration: 1000, fill: 'both'});
      indexed.pause();
      for (const t of [0, 500, 1000]) log('indexed', t, at(indexed, t, 'opacity'), value(e, 'width'));
      indexed.cancel();
      const offsets = e.animate([{opacity: 0}, {opacity: 1, offset: 0.25}, {opacity: 0.5}], 1000);
      offsets.pause();
      for (const t of [100, 250, 625, 1000 - 1]) log('offsets', t, at(offsets, t, 'opacity'));
      offsets.cancel();
      const single = e.animate({opacity: 0}, 1000);
      single.pause();
      for (const t of [0, 500]) log('implicit from', t, at(single, t, 'opacity'));
      single.cancel();
      const camel = e.animate({backgroundColor: ['#000000', '#ffffff'], left: ['0px', '100px']}, 1000);
      camel.pause();
      log('camel', at(camel, 500, 'backgroundColor'), value(e, 'left'));
      camel.cancel();
      const transform = e.animate({transform: ['translateX(0px) rotate(0deg)', 'translateX(100px) rotate(90deg)']},
        1000);
      transform.pause();
      log('transform', at(transform, 500, 'transform'));
      transform.cancel();
      const eased = e.animate([{opacity: 0, easing: 'ease-in'}, {opacity: 1}], 1000);
      eased.pause();
      log('keyframe easing', at(eased, 500, 'opacity'));
      eased.cancel();
      log('keyframes', JSON.stringify(e.animate([{opacity: 0}, {opacity: 1, offset: 0.8, easing: 'ease-out'}, {opacity: 0.5}], 10)
        .effect.getKeyframes()));
      log('indexed keyframes', JSON.stringify(e.animate({opacity: [0, 1], offset: [0, 1]}, 10).effect.getKeyframes()));
      for (const a of e.getAnimations()) a.cancel();
    },
    async function timings() {
      const e = make('opacity:1');
      const cases = [
        ['delay', {duration: 1000, delay: 500}, [0, 400, 500, 1000, 1500, 2000]],
        ['delay backwards', {duration: 1000, delay: 500, fill: 'backwards'}, [0, 500, 1000]],
        ['forwards', {duration: 1000, fill: 'forwards'}, [500, 1000, 3000]],
        ['iterations', {duration: 1000, iterations: 2.5, fill: 'forwards'}, [0, 500, 1000, 1500, 2499, 2500, 3000]],
        ['iteration start', {duration: 1000, iterations: 2, iterationStart: 0.5}, [0, 499, 500, 1000, 1999]],
        ['reverse', {duration: 1000, direction: 'reverse'}, [0, 250, 999]],
        ['alternate', {duration: 1000, iterations: 3, direction: 'alternate'}, [250, 1250, 2250]],
        ['alternate-reverse', {duration: 1000, iterations: 2, direction: 'alternate-reverse'}, [250, 1250]],
        ['ease-in-out', {duration: 1000, easing: 'ease-in-out'}, [250, 500, 750]],
        ['steps', {duration: 1000, easing: 'steps(4)'}, [0, 249, 250, 999]],
        ['cubic-bezier', {duration: 1000, easing: 'cubic-bezier(0.1, 0.7, 1, 0.1)'}, [300, 700]],
        ['end delay', {duration: 1000, endDelay: 500, fill: 'forwards'}, [1000, 1200, 1500]],
        ['zero duration', {duration: 0, fill: 'both'}, [0, 1]],
        ['infinite', {duration: 1000, iterations: Infinity}, [2500]],
      ];
      for (const [name, options, times] of cases) {
        const a = e.animate([{opacity: 0}, {opacity: 1}], options);
        a.pause();
        log(name, times.map((t) => [t, at(a, t, 'opacity'), timing(a)]));
        log(name + ' timing', JSON.stringify(a.effect.getTiming()));
        a.cancel();
      }
    },
    async function playback() {
      const e = make('opacity:1');
      const a = e.animate([{opacity: 0}, {opacity: 1}], {duration: 1000});
      a.pause();
      a.currentTime = 400;
      a.playbackRate = 2;
      log('rate', a.playbackRate, a.currentTime, at(a, 400, 'opacity'));
      a.reverse();
      log('reversed', a.playbackRate, a.currentTime, a.playState, a.pending);
      a.pause();
      a.currentTime = 300;
      log('reversed paused', value(e, 'opacity'), timing(a));
      a.finish();
      log('finish backwards', a.playState, a.currentTime, a.playbackRate);
      a.playbackRate = 1;
      a.finish();
      log('finish', a.playState, a.currentTime, value(e, 'opacity'));
      a.play();
      log('replay', a.playState, a.currentTime, a.pending);
      a.updatePlaybackRate(0.5);
      log('update rate', a.playbackRate, a.pending);
      await a.ready;
      log('rate applied', a.playbackRate);
      a.cancel();
      log('errors', error(() => e.animate({opacity: [0, 1]}, -1)), error(() => e.animate({opacity: [0, 1]},
        {duration: 10, easing: 'nope'})), error(() => e.animate([{opacity: 0, offset: 0.6}, {opacity: 1, offset: 0.2}],
        10)), error(() => a.finish()));
      const zero = e.animate([{opacity: 0}, {opacity: 1}], {duration: 1000});
      zero.playbackRate = 0;
      log('zero rate', zero.playState, error(() => zero.finish()));
      zero.cancel();
      const seek = e.animate([{opacity: 0}, {opacity: 1}], {duration: 1000});
      seek.currentTime = 500;
      log('seek while pending', seek.currentTime, seek.pending, seek.playState);
      await seek.ready;
      log('seek ready', seek.pending, seek.playState);
      seek.cancel();
      log('cancelled', seek.currentTime, seek.startTime, seek.playState);
    },
    async function events() {
      const e = make('opacity:1');
      const order = [];
      const a = e.animate([{opacity: 0}, {opacity: 1}], 50);
      a.onfinish = (event) => order.push(['onfinish', event.type, event.currentTime === a.currentTime,
        event.timelineTime !== null]);
      a.addEventListener('finish', () => order.push(['finish listener']));
      a.finished.then((v) => order.push(['finished', v === a]));
      await a.finished;
      await frames(2);
      log('finish order', JSON.stringify(order), a.playState);
      const b = e.animate([{opacity: 0}, {opacity: 1}], 1000);
      const caught = [];
      b.finished.catch((err) => caught.push(err.name));
      b.oncancel = (event) => caught.push(['oncancel', event.type, event.currentTime]);
      b.cancel();
      await frames(2);
      log('cancel order', JSON.stringify(caught), b.playState);
      const c = e.animate([{opacity: 0}, {opacity: 1}], 1000);
      const first = c.finished;
      c.finish();
      await first;
      c.play();
      log('finished replaced', c.finished !== first);
      c.cancel();
      const d = e.animate([{opacity: 0}, {opacity: 1}], 1000);
      const readyBefore = d.ready;
      d.pause();
      log('ready same while pending', d.ready === readyBefore);
      await d.ready;
      d.play();
      log('ready replaced', d.ready !== readyBefore);
      d.cancel();
    },
    async function stacking() {
      const e = make('opacity:1');
      const low = e.animate({opacity: [0, 0.2]}, {duration: 1000, id: 'low'});
      const high = e.animate({opacity: [0.6, 0.8]}, {duration: 1000, id: 'high'});
      low.pause();
      high.pause();
      low.currentTime = 500;
      high.currentTime = 500;
      log('top wins', value(e, 'opacity'), e.getAnimations().map((x) => x.id), document.getAnimations().length);
      high.cancel();
      log('lower shows', value(e, 'opacity'), e.getAnimations().map((x) => x.id));
      low.cancel();
      log('none', e.getAnimations().length, value(e, 'opacity'));
      const filled = e.animate({opacity: [0, 0.3]}, {duration: 20, fill: 'forwards'});
      const ended = e.animate({opacity: [0, 0.4]}, {duration: 20});
      await Promise.all([filled.finished, ended.finished]);
      await frames(2);
      log('after finishing', e.getAnimations().length, value(e, 'opacity'), filled.playState, ended.playState);
      filled.cancel();
    },
    async function replacement() {
      const e = make('opacity:1');
      const removed = [];
      const first = e.animate({opacity: [0, 0.1]}, {duration: 20, fill: 'forwards'});
      first.onremove = (event) => removed.push(['first', event.type]);
      const kept = e.animate({opacity: [0, 0.2]}, {duration: 20, fill: 'forwards'});
      kept.persist();
      const second = e.animate({opacity: [0, 0.3]}, {duration: 20, fill: 'forwards'});
      await Promise.all([first.finished, kept.finished, second.finished]);
      await frames(3);
      log('replaced', first.replaceState, kept.replaceState, second.replaceState, JSON.stringify(removed),
        e.getAnimations().length, value(e, 'opacity'));
      second.commitStyles();
      log('committed', e.style.opacity);
      for (const a of [first, kept, second]) a.cancel();
      log('after commit', value(e, 'opacity'));
    },
    async function constructed() {
      const e = make('opacity:1');
      const effect = new KeyframeEffect(e, [{opacity: 0}, {opacity: 1}], {duration: 1000, fill: 'forwards'});
      const a = new Animation(effect, document.timeline);
      log('constructed', a.playState, a.currentTime, e.getAnimations().length);
      a.currentTime = 500;
      log('set time idle', a.playState, value(e, 'opacity'));
      effect.updateTiming({duration: 2000});
      log('updated timing', value(e, 'opacity'), timing(a));
      effect.setKeyframes({opacity: [1, 0]});
      log('set keyframes', value(e, 'opacity'));
      a.cancel();
      log('detached', value(e, 'opacity'));
    },
  ];
  window.__run = async () => {
    for (const step of steps) {
      log('step', step.name);
      try {
        await step();
      } catch (e) {
        log('exception', step.name, String(e && e.message || e));
      }
    }
    window.__done = true;
  };
})();
