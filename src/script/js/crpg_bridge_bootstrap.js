// Optional platform module: CRPG Framework's engine bridge over weft.engine (docs/engine-communication.md, 6.6), for
// pages written for its Ultralight host. Install after Engine and before the page.
(() => {
  const engine = globalThis.weft?.engine;
  if (!engine || typeof __ceCrpgBridge !== 'undefined') throw new Error('WEFT_CRPG_BRIDGE_INSTALL');
  Object.defineProperty(globalThis, '__ceCrpgBridge', { value: true });
  // window.ue.sendEvent(name, data) sends the engine a message, as CRPG's host (UEBridgeSendEvent) does: a name that
  // is not a string is ignored, no data (or null) is {}, and what JSON.stringify drops or turns into null (functions,
  // NaN) is dropped or null; values that cross as they are (most) are not stringified. window.ue stays the page's to
  // add to (ue.interface).
  const ue = typeof window.ue === 'object' && window.ue !== null ? window.ue : (window.ue = {});
  ue.sendEvent = (name, data) => {
    if (typeof name !== 'string') return;
    data ??= {};
    try {
      engine.send(name, data);
    } catch (error) {
      if (!String(error?.message).startsWith('WEFT_ENGINE_VALUE')) throw error;
      const text = JSON.stringify(data);
      engine.send(name, text === undefined ? {} : JSON.parse(text));
    }
  };
  // The engine's CallUI(fn, json) events, to the dispatcher the page defines when it does.
  engine.onAny((name, value) => {
    const dispatch = window.__ue_dispatch;
    if (typeof dispatch === 'function') dispatch(name, value);
  });
  // The fields each batch of the view model "vm" changed (FieldNotify's delta), to window.__vm_update when defined.
  engine.model('vm').subscribe((value, changed) => {
    const update = window.__vm_update;
    if (typeof update === 'function') update(changed);
  });
})();
