// Demo-only owner clock, not requestAnimationFrame or CSS animations.
// The fixed original React bundle remains unchanged.
(() => {
  const gallery = document.getElementById('gallery');
  if (!gallery || document.getElementById('ce-motion')) throw new Error('CE_MOTION_MOUNT');
  const make = (tag, id, parent, text = '') => {
    const node = document.createElement(tag);
    node.id = id; if (text) node.textContent = text; parent.appendChild(node); return node;
  };
  const panel = make('div', 'ce-motion', gallery);
  make('span', 'ce-motion-title', panel, 'CONTINUOUS MOTION / LIVE vs 30 Hz');
  const pause = make('button', 'ce-motion-pause', panel, 'Pause');
  const speedButton = make('button', 'ce-motion-speed', panel, 'Speed 1x');
  make('span', 'ce-motion-live-label', panel, 'LIVE');
  make('span', 'ce-motion-reference-label', panel, '30 Hz');
  make('div', 'ce-motion-live-track', panel);
  make('div', 'ce-motion-reference-track', panel);
  const live = make('div', 'ce-motion-live', panel);
  const reference = make('div', 'ce-motion-reference', panel);
  const speeds = [1, 2, .5], travel = 476, referenceInterval = 1000 / 30;
  let last = null, motionMs = 0, referenceMs = 0, liveX = 0, referenceX = 0;
  let running = true, wasRunning = true, wasVisible = true, speedIndex = 0, disposed = false;
  const onPause = () => { running = !running; pause.textContent = running ? 'Pause' : 'Resume'; };
  const onSpeed = () => { speedIndex = (speedIndex + 1) % speeds.length; speedButton.textContent = 'Speed ' + speeds[speedIndex] + 'x'; };
  pause.addEventListener('click', onPause);
  speedButton.addEventListener('click', onSpeed);
  const position = ms => {
    const distance = (ms * .3) % (travel * 2);
    return distance <= travel ? distance : travel * 2 - distance;
  };
  const paint = () => {
    live.style.left = (76 + liveX).toFixed(3) + 'px';
    reference.style.left = (76 + referenceX).toFixed(3) + 'px';
  };
  paint();
  globalThis.__ceMotion = Object.freeze({
    tick(time, visible = true) {
      if (disposed) return;
      if (!Number.isFinite(time) || time < 0 || (last !== null && time < last)) throw new Error('CE_MOTION_TIME');
      const delta = last === null ? 0 : time - last; last = time;
      // Re-anchor on resume; hidden/minimized time never jumps the objects.
      if (visible && wasVisible && running && wasRunning) {
        motionMs = (motionMs + delta * speeds[speedIndex]) % (travel * 2 / .3);
        liveX = position(motionMs);
        referenceMs += delta;
        if (referenceMs >= referenceInterval) {
          referenceX = liveX; referenceMs %= referenceInterval;
        }
        paint();
      }
      wasVisible = visible; wasRunning = running;
    },
    snapshot: () => ({ running, speed: speeds[speedIndex], motion_ms: motionMs, live_x: liveX, reference_x: referenceX, reference_hz: 30, disposed }),
    dispose() {
      if (disposed) return;
      disposed = true;
      pause.removeEventListener('click', onPause); speedButton.removeEventListener('click', onSpeed);
      if (panel.parentNode) panel.parentNode.removeChild(panel);
    },
  });
})();
