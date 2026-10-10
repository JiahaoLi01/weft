// Experimental owner-hosted platform/CSSOM subset. Nodes and tree operations
// remain native C++; the style declaration reflects the native style attribute.
(() => {
  const prototype = Object.getPrototypeOf(document);
  globalThis.Node = function Node() { throw new TypeError('Illegal constructor'); };
  Node.prototype = prototype;
  globalThis.HTMLElement = function HTMLElement() { throw new TypeError('Illegal constructor'); };
  Object.defineProperty(HTMLElement, Symbol.hasInstance, { value: n => n instanceof Node && n.nodeType === 1 && n.namespaceURI === 'http://www.w3.org/1999/xhtml' });
  globalThis.HTMLIFrameElement = function HTMLIFrameElement() { throw new TypeError('Illegal constructor'); };
  Object.defineProperty(HTMLIFrameElement, Symbol.hasInstance, { value: n => n instanceof HTMLElement && n.localName === 'iframe' });
  Object.defineProperty(prototype, 'activeElement', { get() { return this.nodeType === 9 ? this.body : undefined; } });
  const styleKey = Symbol('native-style-declaration');
  const cssName = name => name.startsWith('--') ? name : name.replace(/[A-Z]/g, c => '-' + c.toLowerCase());
  const declarations = node => {
    const result = new Map();
    for (const part of (node.getAttribute('style') || '').split(';')) {
      const colon = part.indexOf(':');
      if (colon > 0) result.set(part.slice(0, colon).trim(), part.slice(colon + 1).trim());
    }
    return result;
  };
  Object.defineProperty(prototype, 'style', { get() {
    if (this.nodeType !== 1) return undefined;
    if (!this[styleKey]) {
      const node = this;
      const write = (name, value, priority = '') => {
        if (priority) throw new Error('CE_CSSOM_PROFILE: important unsupported in inline probe');
        const items = declarations(node);
        if (String(value).trim()) items.set(name, String(value).trim()); else items.delete(name);
        node.setAttribute('style', Array.from(items, ([k, v]) => k + ': ' + v + ';').join(' '));
      };
      const methods = {
        setProperty: (name, value, priority) => write(String(name), value, priority),
        getPropertyValue: name => declarations(node).get(String(name)) || '',
        removeProperty: name => { const old = declarations(node).get(String(name)) || ''; write(String(name), ''); return old; },
      };
      this[styleKey] = new Proxy(methods, {
        get(target, name) {
          if (name === 'cssText') return node.getAttribute('style') || '';
          if (name in target) return target[name];
          if (typeof name === 'symbol') return undefined;
          return declarations(node).get(cssName(name)) || '';
        },
        set(_target, name, value) {
          if (name === 'cssText') node.setAttribute('style', String(value));
          else write(cssName(String(name)), value);
          return true;
        },
      });
    }
    return this[styleKey];
  } });
  let now = 0, next = 1;
  const tasks = new Map();
  const taskErrors = [];
  const advanceClock = time => {
    if (!Number.isFinite(time) || time < now) throw new Error('CE_HOST_TIME: monotonic finite time required');
    now = time;
  };
  globalThis.performance = { now: () => now };
  const schedule = (repeat, callback, delay = 0, ...args) => {
    if (typeof callback !== 'function') throw new TypeError('CE_TASK_PROFILE: callback required');
    if (tasks.size >= 4096) throw new Error('CE_TASK_LIMIT: pending timer capacity');
    // WebIDL long coercion rejects BigInt/Symbol and wraps signed 32-bit.
    delay = +delay;
    if (!Number.isFinite(delay)) delay = 0;
    delay = Math.max(repeat ? 1 : 0, delay | 0);
    const id = next++; tasks.set(id, { callback, args, due: now + delay, repeat, delay }); return id;
  };
  globalThis.setTimeout = (callback, delay, ...args) => schedule(false, callback, delay, ...args);
  globalThis.setInterval = (callback, delay, ...args) => schedule(true, callback, delay, ...args);
  globalThis.clearTimeout = globalThis.clearInterval = id => tasks.delete(+id | 0);
  globalThis.queueMicrotask = callback => {
    if (typeof callback !== 'function') throw new TypeError('CE_TASK_PROFILE: microtask callback required');
    Promise.resolve().then(callback).catch(error => taskErrors.push(error));
  };
  globalThis.console = { error: (...args) => taskErrors.push(new Error('CE_CONSOLE_ERROR: ' + args.join(' '))), log() {}, warn() {} };
  Object.defineProperty(globalThis, '__ceHost', { value: Object.freeze({
    pump(time) {
      if (taskErrors.length) throw taskErrors.shift();
      advanceClock(time);
      const ready = Array.from(tasks).filter(([, task]) => task.due <= now);
      if (ready.length > 1000) throw new Error('CE_TASK_LIMIT: ready batch');
      for (const [id] of ready) {
        const task = tasks.get(id);
        if (task && task.due <= now) {
          // One opportunity per interval, even after an owner pause. Re-arm
          // before invocation so self-cancellation and exceptions are safe.
          if (task.repeat) task.due = now + task.delay; else tasks.delete(id);
          try { Reflect.apply(task.callback, globalThis, task.args); }
          catch (error) { taskErrors.push(error); }
        }
      }
      return Array.from(tasks.values()).filter(task => task.due <= now).length;
    },
    pending: () => tasks.size,
    advanceClock,
  }) });
  const mount = document.createElement('div'); mount.setAttribute('id', 'root'); document.body.appendChild(mount);
})();
