// Optional M0 platform module. The owner drives one batch per drawing opportunity;
// it calls next() directly with native per-callback microtask checkpoints.
(() => {
  if (typeof __ceHost === 'undefined' || typeof __ceAnimationHost !== 'undefined') throw new Error('WEFT_RAF_INSTALL');
  const pending = new Map(), invoke = Reflect.apply;
  let nextId = 1, batch = null, cursor = 0, errors = [], errorCount = 0, frameTime = 0, sent = true;
  globalThis.requestAnimationFrame = callback => {
    if (typeof callback !== 'function') throw new TypeError('WEFT_RAF_CALLBACK: function required');
    if (pending.size >= 4096) throw new Error('WEFT_RAF_LIMIT: pending capacity');
    if (nextId > 0xffffffff) throw new Error('WEFT_RAF_LIMIT: handle exhausted');
    const id = nextId++; pending.set(id, callback); return id;
  };
  globalThis.cancelAnimationFrame = id => {
    if (typeof id === 'bigint' || typeof id === 'symbol') throw new TypeError('WEFT_RAF_HANDLE: number required');
    pending.delete((+id) >>> 0);
  };
  Object.defineProperty(globalThis, '__ceAnimationHost', { value: Object.freeze({
    begin(time, drawable = true) {
      if (batch !== null) throw new Error('WEFT_RAF_BUSY: nested frame');
      const ready = drawable && pending.size ? Array.from(pending.keys()) : [];
      if (ready.length > 1000) throw new Error('WEFT_RAF_LIMIT: ready batch');
      __ceHost.advanceClock(time); // Validation precedes changes to frame state.
      errors = []; errorCount = 0; cursor = 0; frameTime = time; batch = drawable ? ready : null;
      // Web Animations' update step (the scene platform's): the timeline takes the frame's time, pending animations
      // start or pause, finished ones notify; its events go out at the first next(), after the microtasks it queued.
      globalThis.__ceUpdateAnimations?.(time); sent = false;
      return ready.length;
    },
    next() {
      if (!sent) { sent = true; globalThis.__ceSendAnimationEvents?.(); }
      if (batch === null) return 0;
      while (cursor < batch.length) {
        const id = batch[cursor++], callback = pending.get(id);
        if (!callback) continue; // Cancellation also works from a prior microtask.
        pending.delete(id);
        try { invoke(callback, globalThis, [frameTime]); }
        catch (error) {
          ++errorCount;
          if (errors.length < 64) {
            let message = 'WEFT_RAF_CALLBACK_EXCEPTION';
            try { message = String(error).slice(0, 512); } catch (_) {}
            errors.push(message);
          }
        }
        return 1;
      }
      batch = null; return 0;
    },
    pending: () => pending.size,
    errorCount: () => errorCount,
    errors: () => errors.slice(),
    abort() { batch = null; }, // Undispatched callbacks remain pending on host failure.
  }) });
})();
