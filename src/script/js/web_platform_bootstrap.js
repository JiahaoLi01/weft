// Optional platform module: the web platform pages written for browsers expect (WEFT_PAGE_WEB_PLATFORM). The console
// writes diagnostics; localStorage and sessionStorage are the View's storage areas (localStorage kept for the host to
// save); element.dataset, document.head and document.title; matchMedia answered as the stylesheet's @media;
// IntersectionObserver against the viewport; requestIdleCallback; navigator and location as an embedded page has them;
// toLocaleString with options. KeyboardEvent, FocusEvent and the focus (focus(), blur(), document.activeElement,
// tabIndex, scrollIntoView) are every page's (src/script/script_dom.cpp). Install after ScenePlatform and Engine.
(() => {
  if (typeof __ceScenePlatform === 'undefined' || typeof __ceWebPlatform !== 'undefined') throw new Error('WEFT_WEB_PLATFORM_INSTALL');
  Object.defineProperty(globalThis, '__ceWebPlatform', { value: true });
  const doc = document, p = Node.prototype;

  globalThis.DOMException ??= class DOMException extends Error {
    constructor(message = '', name = 'Error') { super(message); this.name = name; }
  };

  // ---- console: messages as diagnostics, at three levels (0 log, 1 warning, 2 error).
  const describe = (value, nested, seen = new Set()) => {
    switch (typeof value) {
      case 'string': return nested ? JSON.stringify(value) : value;
      case 'function': return `[Function ${value.name || '(anonymous)'}]`;
      case 'symbol': case 'bigint': case 'number': case 'boolean': case 'undefined': return String(value);
    }
    if (value === null) return 'null';
    if (value instanceof Error) return value.stack ? `${value.name}: ${value.message}\n${value.stack}` : `${value.name}: ${value.message}`;
    if (value instanceof Node) return value.nodeType === 1 ? `<${value.localName}${value.id ? '#' + value.id : ''}>` : `#${value.nodeName}`;
    if (seen.has(value) || seen.size > 8) return '[…]';
    seen.add(value);
    try {
      if (Array.isArray(value)) return `[${value.slice(0, 64).map(v => describe(v, true, seen)).join(', ')}${value.length > 64 ? ', …' : ''}]`;
      if (value instanceof Map) return `Map(${value.size}) {${Array.from(value).slice(0, 32).map(([k, v]) => `${describe(k, true, seen)} => ${describe(v, true, seen)}`).join(', ')}}`;
      if (value instanceof Set) return `Set(${value.size}) {${Array.from(value).slice(0, 32).map(v => describe(v, true, seen)).join(', ')}}`;
      const keys = Object.keys(value);
      const name = value.constructor && value.constructor !== Object ? value.constructor.name + ' ' : '';
      return `${name}{${keys.slice(0, 32).map(k => `${k}: ${describe(value[k], true, seen)}`).join(', ')}${keys.length > 32 ? ', …' : ''}}`;
    } catch (_) {
      return '[object]';
    } finally {
      seen.delete(value);
    }
  };
  // The first argument's %s %d %i %f %o %O %c substitutions, then the rest separated by spaces.
  const format = args => {
    const out = [];
    let rest = args;
    if (typeof args[0] === 'string' && args.length > 1) {
      let i = 1;
      out.push(args[0].replace(/%[sdifoOc%]/g, m => {
        if (m === '%%') return '%';
        if (i >= args.length) return m;
        const v = args[i++];
        switch (m[1]) {
          case 's': return typeof v === 'string' ? v : describe(v, false);
          case 'd': case 'i': return String(typeof v === 'bigint' ? v : Math.trunc(Number(v)));
          case 'f': return String(Number(v));
          case 'c': return '';
          default: return describe(v, true);
        }
      }));
      rest = args.slice(i);
    }
    for (const v of rest) out.push(describe(v, false));
    return out.join(' ');
  };
  let indent = '';
  const write = (level, args) => {
    let text;
    try { text = indent + format(args); } catch (_) { text = indent + '[unprintable]'; }
    doc.__ceConsole(level, text);
  };
  const counts = new Map(), timers = new Map();
  globalThis.console = {
    log: (...a) => write(0, a), info: (...a) => write(0, a), debug: (...a) => write(0, a),
    trace: (...a) => write(0, ['Trace:', ...a]), dir: v => write(0, [v]), table: v => write(0, [v]),
    warn: (...a) => write(1, a),
    error: (...a) => write(2, a),
    assert: (condition, ...a) => { if (!condition) write(2, ['Assertion failed:', ...a]); },
    group: (...a) => { if (a.length) write(0, a); indent += '  '; },
    groupCollapsed: (...a) => { if (a.length) write(0, a); indent += '  '; },
    groupEnd: () => { indent = indent.slice(2); },
    count: (label = 'default') => { const n = (counts.get(label) ?? 0) + 1; counts.set(label, n); write(0, [`${label}: ${n}`]); },
    countReset: (label = 'default') => { counts.delete(label); },
    time: (label = 'default') => { timers.set(label, performance.now()); },
    timeLog: (label = 'default', ...a) => { if (timers.has(label)) write(0, [`${label}: ${performance.now() - timers.get(label)} ms`, ...a]); },
    timeEnd: (label = 'default') => { if (timers.has(label)) { write(0, [`${label}: ${performance.now() - timers.get(label)} ms`]); timers.delete(label); } },
  };

  // ---- Web Storage over the View's areas (0 local, 1 session).
  const areas = new WeakMap();
  const area = s => {
    const a = areas.get(s);
    if (a === undefined) throw new TypeError('Illegal invocation');
    return a;
  };
  globalThis.Storage = class Storage {
    constructor() { throw new TypeError('Illegal constructor'); }
    get length() { return doc.__ceStorage(area(this), 5); }
    key(index) { return doc.__ceStorage(area(this), 4, Number(index) | 0); }
    getItem(key) { return doc.__ceStorage(area(this), 0, String(key)); }
    setItem(key, value) {
      if (!doc.__ceStorage(area(this), 1, String(key), String(value))) {
        throw new DOMException(`Failed to execute 'setItem' on 'Storage': Setting the value of '${key}' exceeded the quota.`, 'QuotaExceededError');
      }
    }
    removeItem(key) { doc.__ceStorage(area(this), 2, String(key)); }
    clear() { doc.__ceStorage(area(this), 3); }
  };
  const storage = index => {
    const target = Object.create(Storage.prototype);
    const own = k => typeof k === 'string' && !(k in Storage.prototype);
    const proxy = new Proxy(target, {
      get(t, k, receiver) { return own(k) ? (doc.__ceStorage(index, 0, k) ?? undefined) : Reflect.get(t, k, receiver); },
      set(t, k, v, receiver) { if (!own(k)) return Reflect.set(t, k, v, receiver); receiver.setItem(k, v); return true; },
      has(t, k) { return own(k) ? doc.__ceStorage(index, 0, k) !== null : Reflect.has(t, k); },
      deleteProperty(t, k) { if (own(k)) doc.__ceStorage(index, 2, k); return true; },
      ownKeys() { return doc.__ceStorage(index, 6); },
      getOwnPropertyDescriptor(t, k) {
        const v = own(k) ? doc.__ceStorage(index, 0, k) : null;
        return v === null ? undefined : { value: v, writable: true, enumerable: true, configurable: true };
      },
    });
    areas.set(proxy, index);
    return proxy;
  };
  const local = storage(0), session = storage(1);
  Object.defineProperty(globalThis, 'localStorage', { get: () => local, configurable: true });
  Object.defineProperty(globalThis, 'sessionStorage', { get: () => session, configurable: true });

  // ---- element.dataset, attributes by name, the ChildNode/ParentNode conveniences.
  const datasets = new WeakMap();
  const dataName = k => 'data-' + k.replace(/[A-Z]/g, c => '-' + c.toLowerCase());
  Object.defineProperty(p, 'dataset', { configurable: true, get() {
    if (this.nodeType !== 1) return undefined;
    let d = datasets.get(this);
    if (d) return d;
    const n = this;
    d = new Proxy({}, {
      get: (_, k) => { if (typeof k !== 'string') return undefined; const v = n.getAttribute(dataName(k)); return v === null ? undefined : v; },
      set: (_, k, v) => { if (typeof k === 'string') n.setAttribute(dataName(k), String(v)); return true; },
      has: (_, k) => typeof k === 'string' && n.hasAttribute(dataName(k)),
      deleteProperty: (_, k) => { if (typeof k === 'string') n.removeAttribute(dataName(k)); return true; },
    });
    datasets.set(this, d);
    return d;
  } });
  p.toggleAttribute = function (name, force) {
    const on = force === undefined ? !this.hasAttribute(name) : !!force;
    if (on) { if (!this.hasAttribute(name)) this.setAttribute(name, ''); } else this.removeAttribute(name);
    return on;
  };
  const nodes = items => items.map(v => v instanceof Node ? v : doc.createTextNode(String(v)));
  p.append = function (...items) { for (const n of nodes(items)) this.appendChild(n); };
  p.prepend = function (...items) { const first = this.firstChild; for (const n of nodes(items)) this.insertBefore(n, first); };
  p.replaceChildren = function (...items) { while (this.firstChild) this.removeChild(this.firstChild); this.append(...items); };
  p.before = function (...items) { const parent = this.parentNode; if (parent) for (const n of nodes(items)) parent.insertBefore(n, this); };
  p.after = function (...items) { const parent = this.parentNode, next = this.nextSibling; if (parent) for (const n of nodes(items)) parent.insertBefore(n, next); };
  p.replaceWith = function (...items) { const parent = this.parentNode; if (!parent) return; this.before(...items); parent.removeChild(this); };
  p.hasChildNodes = function () { return this.firstChild !== null; };

  // ---- document.head (made on first use, hidden in any stylesheet) and document.title.
  let head = null, title = '';
  Object.defineProperty(p, 'head', { configurable: true, get() {
    if (this.nodeType !== 9) return undefined;
    if (!head) {
      head = doc.createElement('head');
      head.setAttribute('style', 'display: none');
      doc.documentElement.insertBefore(head, doc.body);
    }
    return head;
  } });
  Object.defineProperty(p, 'title', { configurable: true,
    get() { return this.nodeType === 9 ? title : (this.getAttribute?.('title') ?? ''); },
    set(v) { if (this.nodeType === 9) title = String(v); else this.setAttribute('title', String(v)); } });

  // ---- matchMedia: the stylesheet's @media evaluation over the View's size and input devices, read when asked.
  // Any query, with the scene platform's lists (live, change events).
  globalThis.matchMedia = query => __ceScenePlatform.mediaList(String(query));

  // ---- IntersectionObserver against the viewport (or its root's box), checked each animation frame it observes.
  const watching = new Set();
  let checking = false;
  const check = () => {
    checking = false;
    for (const o of Array.from(watching)) o.__deliver();
    if (watching.size) { checking = true; requestAnimationFrame(check); }
  };
  globalThis.IntersectionObserver = class IntersectionObserver {
    constructor(callback, options = {}) {
      if (typeof callback !== 'function') throw new TypeError('IntersectionObserver: a callback');
      this.callback = callback;
      this.root = options.root ?? null;
      this.rootMargin = options.rootMargin ?? '0px';
      const t = options.threshold ?? 0;
      this.thresholds = (Array.isArray(t) ? t : [t]).map(Number).sort((a, b) => a - b);
      this.targets = new Map();
    }
    observe(target) {
      if (!(target instanceof Element)) throw new TypeError('IntersectionObserver.observe: an element');
      if (!this.targets.has(target)) this.targets.set(target, null);
      watching.add(this);
      if (!checking) { checking = true; requestAnimationFrame(check); }
    }
    unobserve(target) { this.targets.delete(target); if (!this.targets.size) watching.delete(this); }
    disconnect() { this.targets.clear(); watching.delete(this); }
    takeRecords() { return []; }
    __deliver() {
      const r = this.root ? this.root.getBoundingClientRect() : { left: 0, top: 0, right: innerWidth, bottom: innerHeight };
      const entries = [];
      for (const [target, last] of this.targets) {
        const b = target.getBoundingClientRect();
        const w = Math.max(0, Math.min(b.right, r.right) - Math.max(b.left, r.left));
        const h = Math.max(0, Math.min(b.bottom, r.bottom) - Math.max(b.top, r.top));
        // Rectangles that touch intersect (an empty target too), as IntersectionObserver's edge-adjacent rule says.
        const intersecting = target.isConnected && b.left <= r.right && b.right >= r.left && b.top <= r.bottom && b.bottom >= r.top;
        const area = b.width * b.height;
        const ratio = !intersecting ? 0 : area > 0 ? (w * h) / area : 1;
        const crossed = this.thresholds.filter(t => ratio >= t).length;
        if (last && last[0] === intersecting && last[1] === crossed) continue;
        this.targets.set(target, [intersecting, crossed]);
        entries.push({ target, isIntersecting: intersecting, intersectionRatio: intersecting ? ratio : 0, time: performance.now(),
          boundingClientRect: b, rootBounds: r,
          intersectionRect: { x: Math.max(b.left, r.left), y: Math.max(b.top, r.top), width: w, height: h } });
      }
      if (entries.length) this.callback(entries, this);
    }
  };

  // ---- Idle callbacks, scrolling, navigator, location, getComputedStyle for pseudo-elements.
  globalThis.requestIdleCallback = (callback, options) => setTimeout(() => callback({ didTimeout: false, timeRemaining: () => 0 }), Math.min(Number(options?.timeout) || 1, 50));
  globalThis.cancelIdleCallback = id => clearTimeout(id);
  globalThis.scrollTo = globalThis.scroll = globalThis.scrollBy = () => {};
  // The scene platform's navigator joins in as it is (maxTouchPoints a getter: the host's devices may change).
  globalThis.navigator = Object.freeze(Object.defineProperties({ userAgent: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) weft',
    language: 'en-US', languages: Object.freeze(['en-US']), platform: 'Win32', vendor: '', onLine: true, cookieEnabled: false,
    hardwareConcurrency: 1 }, Object.getOwnPropertyDescriptors(globalThis.navigator)));
  globalThis.location = Object.freeze({ href: 'weft:///', origin: 'null', protocol: 'weft:', host: '', hostname: '', port: '',
    pathname: '/', search: '', hash: '', assign() {}, replace() {}, reload() {}, toString: () => 'weft:///' });
  const computed = globalThis.getComputedStyle;
  globalThis.getComputedStyle = (n, pseudo) => pseudo ? new Proxy({}, { get: (_, k) => k === 'getPropertyValue' ? () => '' : '' }) : computed(n);

  // ---- toLocaleString with options: grouping by thousands and the fraction digits asked for (no Intl).
  Number.prototype.toLocaleString = function (locales, options = {}) {
    let v = Number.prototype.valueOf.call(this);
    if (!Number.isFinite(v)) return Number.isNaN(v) ? 'NaN' : (v < 0 ? '-∞' : '∞');
    const percent = options.style === 'percent';
    if (percent) v *= 100;
    const min = Math.min(20, Math.max(0, options.minimumFractionDigits ?? 0));
    const max = Math.min(20, Math.max(min, options.maximumFractionDigits ?? (percent ? Math.max(min, 0) : Math.max(min, 3))));
    let text = Math.abs(v).toFixed(max);
    if (max > min) text = text.replace(new RegExp(`\\.?0{1,${max - min}}$`), m => (m.startsWith('.') && min > 0 ? '.' : ''));
    let [whole, fraction] = text.split('.');
    if (options.useGrouping !== false) whole = whole.replace(/\B(?=(\d{3})+(?!\d))/g, ',');
    const negative = v < 0 && /[1-9]/.test(text);
    return (negative ? '-' : '') + whole + (fraction ? '.' + fraction : '') + (percent ? '%' : '');
  };
})();
