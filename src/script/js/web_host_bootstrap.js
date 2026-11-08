// Experimental owner-hosted platform/CSSOM subset. Nodes and tree operations
// remain native C++; the style declaration reflects the native style attribute.
(() => {
  const prototype = Object.getPrototypeOf(document);
  globalThis.Node = function Node() { throw new TypeError('Illegal constructor'); };
  Node.prototype = prototype;
  Object.defineProperty(prototype,'constructor',{value:Node,writable:true,configurable:true});
  globalThis.HTMLElement = function HTMLElement() { throw new TypeError('Illegal constructor'); };
  Object.defineProperty(HTMLElement, Symbol.hasInstance, { value: n => n instanceof Node && n.nodeType === 1 && n.namespaceURI === 'http://www.w3.org/1999/xhtml' });
  globalThis.HTMLIFrameElement = function HTMLIFrameElement() { throw new TypeError('Illegal constructor'); };
  Object.defineProperty(HTMLIFrameElement, Symbol.hasInstance, { value: n => n instanceof HTMLElement && n.localName === 'iframe' });
  const styleKey = Symbol('native-style-declaration');
  // CSS names of the style object's property names (camelCase to kebab-case; custom properties as they are), kept for
  // the names a page uses.
  const cssNames = new Map();
  const cssName = name => {
    let css = cssNames.get(name);
    if (css === undefined) {
      css = name.startsWith('--') ? name : name.replace(/[A-Z]/g, c => '-' + c.toLowerCase());
      if (cssNames.size < 1024) cssNames.set(name, css);
    }
    return css;
  };
  const cssProperties = new Set(('display position width height min-width min-height max-width max-height left right top bottom margin padding border border-width border-radius border-color border-style box-sizing gap row-gap column-gap flex flex-direction flex-wrap flex-grow flex-shrink flex-basis align-items align-self align-content justify-content justify-items grid-template-columns place-items overflow opacity transform transform-origin background background-color background-image background-size background-position box-shadow animation animation-delay animation-duration animation-timing-function transition transition-property transition-duration transition-delay transition-timing-function pointer-events cursor z-index text-align white-space font-size font-weight font-family line-height letter-spacing color fill fill-opacity fill-rule stroke stroke-width stroke-opacity stroke-linecap stroke-linejoin stroke-dasharray stroke-dashoffset text-anchor dominant-baseline touch-action user-select will-change').split(' '));
  for(const side of ['top','right','bottom','left'])for(const prefix of ['margin-','padding-','border-'])cssProperties.add(prefix+side+(prefix==='border-'?'-width':''));
  // The declaration lives in the style attribute: the node parses it, changes one property and writes it back in one
  // native call (__ceStyleSet, __ceStyleGet in src/script/script_dom.cpp): "name: value;" joined by spaces, a value
  // trimmed, one whose parentheses or quotes do not balance ignored, an empty one removing the property.
  Object.defineProperty(prototype, 'style', { get() {
    if (this.nodeType !== 1) return undefined;
    if (!this[styleKey]) {
      const node = this;
      const write = (name, value, priority = '') => {
        if (priority) throw new Error('WEFT_CSSOM_PROFILE: important unsupported in inline probe');
        node.__ceStyleSet(name, String(value));
      };
      const read = name => node.__ceStyleGet(name);
      const methods = {
        setProperty: (name, value, priority) => write(String(name), value, priority),
        getPropertyValue: name => read(String(name)),
        removeProperty: name => { const old = read(String(name)); write(String(name), ''); return old; },
      };
      this[styleKey] = new Proxy(methods, {
        has(target,name){return name in target || (typeof name==='string' && cssProperties.has(cssName(name)));},
        get(target, name) {
          if (name === 'cssText') return node.getAttribute('style') || '';
          if (name in target) return target[name];
          if (typeof name === 'symbol') return undefined;
          return read(cssName(name));
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
  let now = 0, next = 1, nextDue = Infinity, lastCallbacks = 0;
  const tasks = new Map();
  const taskErrors = [];
  const advanceClock = time => {
    if (!Number.isFinite(time) || time < now) throw new Error('WEFT_HOST_TIME: monotonic finite time required');
    now = time;
  };
  globalThis.performance = { now: () => now };
  const schedule = (repeat, callback, delay = 0, ...args) => {
    if (typeof callback !== 'function') throw new TypeError('WEFT_TASK_PROFILE: callback required');
    if (tasks.size >= 4096) throw new Error('WEFT_TASK_LIMIT: pending timer capacity');
    // WebIDL long coercion rejects BigInt/Symbol and wraps signed 32-bit.
    delay = +delay;
    if (!Number.isFinite(delay)) delay = 0;
    delay = Math.max(repeat ? 1 : 0, delay | 0);
    const id = next++; tasks.set(id, { callback, args, due: now + delay, repeat, delay }); nextDue = Math.min(nextDue, now + delay); return id;
  };
  globalThis.setTimeout = (callback, delay, ...args) => schedule(false, callback, delay, ...args);
  globalThis.setInterval = (callback, delay, ...args) => schedule(true, callback, delay, ...args);
  globalThis.clearTimeout = globalThis.clearInterval = id => {
    id = +id | 0; const task = tasks.get(id); tasks.delete(id);
    if (task && task.due === nextDue) { nextDue = Infinity; for (const task of tasks.values()) nextDue = Math.min(nextDue, task.due); }
  };
  globalThis.queueMicrotask = callback => {
    if (typeof callback !== 'function') throw new TypeError('WEFT_TASK_PROFILE: microtask callback required');
    Promise.resolve().then(callback).catch(error => taskErrors.push(error));
  };
  globalThis.console = { error: (...args) => taskErrors.push(new Error('WEFT_CONSOLE_ERROR: ' + args.join(' '))), log() {}, warn() {} };
  Object.defineProperty(globalThis, '__ceHost', { value: Object.freeze({
    pump(time) {
      lastCallbacks = 0;
      if (taskErrors.length) throw taskErrors.shift();
      advanceClock(time);
      if (nextDue > now) return 0;
      const ready = Array.from(tasks).filter(([, task]) => task.due <= now);
      if (ready.length > 1000) throw new Error('WEFT_TASK_LIMIT: ready batch');
      for (const [id] of ready) {
        const task = tasks.get(id);
        if (task && task.due <= now) {
          ++lastCallbacks;
          // One opportunity per interval, even after an owner pause. Re-arm
          // before invocation so self-cancellation and exceptions are safe.
          if (task.repeat) task.due = now + task.delay; else tasks.delete(id);
          try { Reflect.apply(task.callback, globalThis, task.args); }
          catch (error) { taskErrors.push(error); }
        }
      }
      let readyCount = 0; nextDue = Infinity;
      for (const task of tasks.values()) { nextDue = Math.min(nextDue, task.due); if (task.due <= now) ++readyCount; }
      return readyCount;
    },
    // Private typed native result; the existing pump result remains unchanged.
    pumpNative(time) { const ready = this.pump(time); return lastCallbacks * 4097 + ready; },
    pending: () => tasks.size,
    advanceClock,
  }) });
  const mount = document.createElement('div'); mount.setAttribute('id', 'root'); document.body.appendChild(mount);
})();
