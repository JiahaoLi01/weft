// Optional platform module: the engine bridge (docs/engine-communication.md, 6). The owner hands the page what the
// engine gave at its safe points through __ceEngineHost; weft.engine sends, calls and invokes through the document's
// native methods. Install after WebHost.
(() => {
  if (typeof __ceHost === 'undefined' || typeof __ceEngineHost !== 'undefined') throw new Error('WEFT_ENGINE_INSTALL');
  const doc = document, invoke = Reflect.apply, freeze = Object.freeze;
  // Each name's handlers in order, the onAny handlers, the open calls by id, the view models by name, and the
  // messages of what handlers threw since the owner last took them.
  const handlers = new Map(), any = [], calls = new Map(), models = new Map();
  let errors = [];
  const fail = error => {
    if (errors.length >= 64) return;
    let message = 'WEFT_ENGINE_HANDLER_EXCEPTION';
    try { message = String(error).slice(0, 512); } catch (_) {}
    errors.push(message);
  };
  const named = (name, what) => {
    if (typeof name !== 'string') throw new TypeError(`weft.engine.${what}: a name`);
    return name;
  };
  const callable = (handler, what) => {
    if (typeof handler !== 'function') throw new TypeError(`weft.engine.${what}: a function`);
    return handler;
  };
  // A handler in list; its off() takes it out (and calls empty when none is left), and one taken out during a delivery
  // is not called.
  const listen = (list, handler, once, empty) => {
    const entry = {handler, once, removed: false};
    entry.off = () => {
      if (entry.removed) return;
      entry.removed = true;
      list.splice(list.indexOf(entry), 1);
      if (!list.length) empty?.();
    };
    list.push(entry);
    return entry.off;
  };
  const listenNamed = (name, handler, once) => {
    let list = handlers.get(name);
    if (!list) handlers.set(name, list = []);
    return listen(list, handler, once, () => { if (handlers.get(name) === list) handlers.delete(name); });
  };
  // A view model: its fields so far, frozen and replaced by each batch, and who subscribed.
  const model = name => {
    let entry = models.get(name);
    if (entry) return entry;
    const subscribers = new Set();
    entry = {value: undefined, subscribers};
    entry.handle = freeze({
      name,
      get value() { return entry.value; },
      // callback(value, changed) after each batch; returns the unsubscribe function.
      subscribe(callback) {
        const record = {callback: callable(callback, 'model().subscribe')};
        subscribers.add(record);
        return () => { subscribers.delete(record); };
      },
    });
    models.set(name, entry);
    return entry;
  };
  const engine = freeze({
    // handler(value) for each event of the name; returns off().
    on: (name, handler) => listenNamed(named(name, 'on'), callable(handler, 'on'), false),
    // handler(value) for the next event of the name; returns off().
    once: (name, handler) => listenNamed(named(name, 'once'), callable(handler, 'once'), true),
    // handler(name, value) for every event, after the name's handlers; returns off().
    onAny: handler => listen(any, callable(handler, 'onAny'), false),
    // A message for the engine.
    send(name, value) { doc.__ceEngineSend(named(name, 'send'), value); },
    // A call the engine completes later: the promise resolves with its value, or rejects with an Error whose value is
    // the engine's. options.timeout (ms) rejects it after that long; the engine's completion is then stale.
    call(name, value, options) {
      try {
        named(name, 'call');
        const timeout = options?.timeout;
        if (timeout !== undefined && !(typeof timeout === 'number' && timeout >= 0 && timeout < 2 ** 31)) {
          throw new TypeError('weft.engine.call: a timeout in ms');
        }
        let call, id = 0;
        const promise = new Promise((resolve, reject) => { call = {name, resolve, reject, timer: undefined}; });
        if (timeout !== undefined) {
          call.timer = setTimeout(() => {
            if (!id || calls.get(id) !== call) return;
            calls.delete(id);
            doc.__ceEngineClose(id);
            call.reject(new Error(`WEFT_ENGINE_TIMEOUT: ${name}`));
          }, timeout);
        }
        try { id = doc.__ceEngineCall(name, value); }
        catch (error) {
          if (call.timer !== undefined) clearTimeout(call.timer);
          throw error;
        }
        calls.set(id, call);
        return promise;
      } catch (error) {
        return Promise.reject(error);
      }
    },
    // The host's sync function: its result at once.
    invoke: (name, value) => doc.__ceEngineInvoke(named(name, 'invoke'), value),
    // The view model's handle: {name, value, subscribe}. For React, useSyncExternalStore(m.subscribe, () => m.value).
    model: name => model(named(name, 'model')).handle,
  });
  Object.defineProperty(globalThis, '__ceEngineHost', { value: freeze({
    event(name, value) {
      const list = handlers.get(name);
      if (list) {
        for (const entry of list.slice()) {
          if (entry.removed) continue;
          if (entry.once) entry.off();
          try { invoke(entry.handler, undefined, [value]); } catch (error) { fail(error); }
        }
      }
      for (const entry of any.slice()) {
        if (entry.removed) continue;
        try { invoke(entry.handler, undefined, [name, value]); } catch (error) { fail(error); }
      }
    },
    // Whether the call was open: an unknown id is a completion that came after its timeout.
    settle(id, succeeded, value) {
      const call = calls.get(id);
      if (!call) return false;
      calls.delete(id);
      if (call.timer !== undefined) clearTimeout(call.timer);
      if (succeeded) {
        call.resolve(value);
      } else {
        const message = typeof value === 'string' ? value
          : typeof value?.message === 'string' ? value.message : `weft.engine.call ${call.name} failed`;
        const error = new Error(message);
        error.value = value;
        call.reject(error);
      }
      return true;
    },
    model(name, fields) {
      const entry = model(name);
      freeze(fields);
      entry.value = freeze({...entry.value, ...fields});
      for (const record of [...entry.subscribers]) {
        if (!entry.subscribers.has(record)) continue;
        try { invoke(record.callback, undefined, [entry.value, fields]); } catch (error) { fail(error); }
      }
    },
    errors() {
      const out = errors;
      errors = [];
      return out;
    },
  }) });
  globalThis.weft = freeze({...(globalThis.weft ?? {}), engine});
})();
