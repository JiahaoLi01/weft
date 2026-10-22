// Optional owner platform for SVG interaction and library-driven animation.
// Native nodes remain authoritative. CSSOM/geometry reads flush native layout.
(() => {
  const p = Node.prototype;
  const svgPrototype=Object.create(p),createNS=p.createElementNS;
  p.createElementNS=function(ns,name){const n=createNS.call(this,ns,name);if(ns==='http://www.w3.org/2000/svg')Object.setPrototypeOf(n,svgPrototype);return n;};
  globalThis.Element = function Element(){throw new TypeError('Illegal constructor');};Object.defineProperty(Element,Symbol.hasInstance,{value:n=>n instanceof Node&&n.nodeType===1});
  globalThis.SVGElement = function SVGElement() { throw new TypeError('Illegal constructor'); };
  Object.defineProperty(SVGElement, Symbol.hasInstance, {value: n => n instanceof Node && n.namespaceURI === 'http://www.w3.org/2000/svg'});
  globalThis.SVGSVGElement = function SVGSVGElement() { throw new TypeError('Illegal constructor'); };
  Object.defineProperty(SVGSVGElement, Symbol.hasInstance, {value: n => n instanceof SVGElement && n.localName === 'svg'});
  globalThis.HTMLButtonElement=function HTMLButtonElement(){throw new TypeError('Illegal constructor');};Object.defineProperty(HTMLButtonElement,Symbol.hasInstance,{value:n=>n instanceof HTMLElement&&n.localName==='button'});
  const tokenLists=new WeakMap();
  Object.defineProperty(p,'classList',{get(){if(tokenLists.has(this))return tokenLists.get(this);const n=this,read=()=>Array.from(new Set((n.getAttribute('class')||'').split(/[\t\n\f\r ]+/).filter(Boolean))),check=t=>{t=String(t);if(!t||/[\t\n\f\r ]/.test(t))throw TypeError('Invalid class token');return t;},write=a=>n.setAttribute('class',a.join(' '));const list={contains:t=>read().includes(check(t)),add(...ts){ts=ts.map(check);write(Array.from(new Set([...read(),...ts])));},remove(...ts){ts=ts.map(check);write(read().filter(t=>!ts.includes(t)));},toggle(t,force){t=check(t);const a=read(),present=a.includes(t),on=force===undefined?!present:!!force;if(on&&!present)a.push(t);if(!on&&present)a.splice(a.indexOf(t),1);write(a);return on;},item:i=>read()[i]??null,[Symbol.iterator]:()=>read()[Symbol.iterator]()};Object.defineProperties(list,{value:{get:()=>n.getAttribute('class')||'',set:v=>n.setAttribute('class',String(v))},length:{get:()=>read().length}});tokenLists.set(n,list);return list;}});
  globalThis.NodeList = class NodeList extends Array { item(i) { return this[i] ?? null; } };
  globalThis.HTMLCollection = class HTMLCollection extends NodeList {};
  const query = p.querySelectorAll;
  p.querySelectorAll = function(s) { const nodes = query.call(this,s); Object.setPrototypeOf(nodes,NodeList.prototype); return nodes; };
  Object.defineProperties(p, {
    className: {get(){return this.getAttribute('class') || '';},set(v){this.setAttribute('class',String(v));}},
    parentElement: {get(){return this.parentNode?.nodeType === 1 ? this.parentNode : null;}},
    children: {get(){const a=this.childNodes.filter(n=>n.nodeType===1);Object.setPrototypeOf(a,HTMLCollection.prototype);return a;}},
    clientLeft: {get(){return parseFloat(getComputedStyle(this).borderLeftWidth)||0;}},
    clientTop: {get(){return parseFloat(getComputedStyle(this).borderTopWidth)||0;}},
  });
  Object.defineProperty(svgPrototype,'ownerSVGElement',{get(){let n=this.parentNode;while(n && !(n instanceof SVGSVGElement))n=n.parentNode;return n;}});
  p.remove = function(){this.parentNode?.removeChild(this);};
  // Gamepads, as the Gamepad API is in Chrome (the host's states come through __ceGamepad): navigator.getGamepads()
  // shows four slots, null where none is, and none at all until a gamepad's button is pressed or one of its axes is
  // past half (Chrome's user gesture); then gamepadconnected for each connected one, for one that connects later, and
  // gamepaddisconnected for one that goes. Each call makes Gamepad objects of the latest states (snapshots, as
  // Chrome's); a Gamepad's timestamp is the page's clock when its state last changed. A button pressed or above 0 is
  // touched too (Blink's GamepadButton).
  const pads=[null,null,null,null],gamepadToken=Symbol('gamepad'),gamepadState=new WeakMap();let gamepadGesture=false;
  globalThis.GamepadButton=class GamepadButton{constructor(t,pressed,touched,value){if(t!==gamepadToken)throw new TypeError('Illegal constructor');gamepadState.set(this,[pressed,touched,value]);}get pressed(){return gamepadState.get(this)[0];}get touched(){return gamepadState.get(this)[1];}get value(){return gamepadState.get(this)[2];}};
  globalThis.Gamepad=class Gamepad{constructor(t,s,connected){if(t!==gamepadToken)throw new TypeError('Illegal constructor');gamepadState.set(this,{s,connected,axes:Object.freeze(s.axes.slice()),buttons:Object.freeze(s.values.map((v,i)=>new GamepadButton(gamepadToken,!!(s.pressed>>>i&1),!!((s.pressed|s.touched)>>>i&1)||v>0,v)))});}get id(){return gamepadState.get(this).s.id;}get index(){return gamepadState.get(this).s.index;}get connected(){return gamepadState.get(this).connected;}get timestamp(){return gamepadState.get(this).s.time;}get mapping(){return 'standard';}get axes(){return gamepadState.get(this).axes;}get buttons(){return gamepadState.get(this).buttons;}get vibrationActuator(){return null;}};
  globalThis.GamepadEvent=class GamepadEvent extends Event{constructor(type,options){if(!options||!(options.gamepad instanceof Gamepad))throw new TypeError("GamepadEvent: the gamepad is required");super(type,options);this.gamepad=options.gamepad;}};
  const gamepadEvent=(type,s,connected)=>{const e=new GamepadEvent(type,{gamepad:new Gamepad(gamepadToken,s,connected)});Object.defineProperty(e,'isTrusted',{value:true});dispatchEvent(e);};
  globalThis.__ceGamepad=(index,connected,id,values,pressed,touched,axes,time)=>{
    const was=pads[index];
    if(!connected){pads[index]=null;if(was&&gamepadGesture)gamepadEvent('gamepaddisconnected',was,false);return;}
    const same=was&&was.id===id&&was.pressed===pressed&&was.touched===touched&&was.values.every((v,i)=>v===values[i])&&was.axes.every((v,i)=>v===axes[i]);
    const s={index,id,values,pressed,touched,axes,time:same?was.time:time};pads[index]=s;
    if(gamepadGesture){if(!was)gamepadEvent('gamepadconnected',s,true);return;}
    if(pressed||axes.some(v=>Math.abs(v)>0.5)){gamepadGesture=true;for(const p of pads)if(p)gamepadEvent('gamepadconnected',p,true);}
  };
  globalThis.navigator = Object.freeze({maxTouchPoints:0,getGamepads(){return pads.map(s=>s&&gamepadGesture?new Gamepad(gamepadToken,s,true):null);}}); // Current primary mouse host.
  // UI particles (docs/architecture.md, "粒子"): a burst of the host's registered particles from an element at the next
  // frame, options.count of them (1 by default) over options.duration seconds, flying to options.target (an element's
  // centre, or {x, y} in viewport px) when given. Throws without a particle host, for an unknown name, a bad count or
  // duration, or a target that is neither.
  globalThis.weft = Object.freeze({...(globalThis.weft ?? {}), emitParticles(element, name, options = {}) {
    if (!(element instanceof Element)) throw new TypeError('weft.emitParticles: an element');
    const target = options.target ?? null, node = target instanceof Element ? target : null;
    const x = target && !node ? Number(target.x) : NaN, y = target && !node ? Number(target.y) : NaN;
    if (target && !node && !(Number.isFinite(x) && Number.isFinite(y))) throw new TypeError('weft.emitParticles: a target element or {x, y}');
    element.__ceEmitParticles(String(name), options.count ?? 1, options.duration ?? 0, node, x, y);
  }});
  Date.now = () => Math.floor(performance.now()); // Host logical clock, epoch 0.
  Number.prototype.toLocaleString=function(locales,options){if(locales!==undefined&&locales!=='en-US'||options!==undefined&&Object.keys(options).length)throw Error('WEFT_NUMBER_FORMAT_PROFILE: en-US default integer format');return document.__ceFormatInteger(Number.prototype.valueOf.call(this));};
  globalThis.getComputedStyle = (n,pseudo) => {
    if (pseudo) throw Error('WEFT_CSSOM_PROFILE: pseudo style unavailable');
    const values=n.__ceComputedStyle();
    return new Proxy(values,{get(v,k){if(k==='getPropertyValue')return name=>v[String(name)]||'';if(typeof k==='symbol')return undefined;return v[k] ?? v[k.replace(/[A-Z]/g,c=>'-'+c.toLowerCase())] ?? '';}});
  };
  globalThis.DOMMatrix = class DOMMatrix {
    constructor(v=[1,0,0,1,0,0]){if(typeof v==='string'){if(v==='none'||v==='')v=[1,0,0,1,0,0];else if(/^matrix\([^()]+\)$/.test(v))v=v.slice(7,-1).split(',').map(Number);else throw Error('WEFT_MATRIX_PROFILE: affine matrix required');}if(v.length!==6||!v.every(Number.isFinite))throw TypeError('Invalid matrix');[this.a,this.b,this.c,this.d,this.e,this.f]=v;}
    inverse(){const q=this.a*this.d-this.b*this.c;if(!Number.isFinite(q)||Math.abs(q)<1e-12)throw Error('WEFT_MATRIX_SINGULAR');return new DOMMatrix([this.d/q,-this.b/q,-this.c/q,this.a/q,(this.c*this.f-this.d*this.e)/q,(this.b*this.e-this.a*this.f)/q]);}
    preMultiplySelf(m){const [a,b,c,d,e,f]=[m.a*this.a+m.c*this.b,m.b*this.a+m.d*this.b,m.a*this.c+m.c*this.d,m.b*this.c+m.d*this.d,m.a*this.e+m.c*this.f+m.e,m.b*this.e+m.d*this.f+m.f];Object.assign(this,{a,b,c,d,e,f});return this;}
  };
  globalThis.DOMPoint=class DOMPoint {constructor(x=0,y=0){this.x=x;this.y=y;}matrixTransform(m){return new DOMPoint(m.a*this.x+m.c*this.y+m.e,m.b*this.x+m.d*this.y+m.f);}};
  p.getScreenCTM=function(){return new DOMMatrix(this.__ceGeometry().matrix);};
  p.createSVGPoint=function(){return {x:0,y:0,matrixTransform(m){return {x:m.a*this.x+m.c*this.y+m.e,y:m.b*this.x+m.d*this.y+m.f};}};};
  Object.defineProperties(p,{
    viewBox:{get(){const a=(this.getAttribute('viewBox')||'0 0 0 0').trim().split(/[\s,]+/).map(Number);return {baseVal:{x:a[0],y:a[1],width:a[2],height:a[3]}};}},
    width:{get(){return {baseVal:{value:this.__ceGeometry().offset[2]}};}},
    height:{get(){return {baseVal:{value:this.__ceGeometry().offset[3]}};}},
  });
  class Target {
    constructor(){this.listeners=[];}
    addEventListener(type,callback,options={}){if(!callback)return;const capture=typeof options==='boolean'?options:!!options.capture;if(this.listeners.some(l=>l.type===type&&l.callback===callback&&l.capture===capture))return;if(options?.signal?.aborted)return;if(this.listeners.length>=1024)throw Error("WEFT_LISTENER_LIMIT");const entry={type,callback,capture,once:!!options.once,passive:!!options.passive};this.listeners.push(entry);options?.signal?.addEventListener('abort',()=>this.removeEventListener(type,callback,options),{once:true});}
    removeEventListener(type,callback,options={}){const capture=typeof options==='boolean'?options:!!options.capture;this.listeners=this.listeners.filter(l=>!(l.type===type&&l.callback===callback&&l.capture===capture));}
    dispatchEvent(e){e.target=this;e.eventPhase=2;this.phase(e,true,this);if(!e._immediateStopped)this.phase(e,false,this);e.currentTarget=null;e.eventPhase=0;return !e.defaultPrevented;}
    phase(e,capture,target){for(const l of this.listeners.slice()){if(l.type!==e.type||l.capture!==capture||!this.listeners.includes(l))continue;if(e._immediateStopped)break;if(l.once)this.removeEventListener(l.type,l.callback,l.capture);e.currentTarget=target;const passive=e._passiveListener;e._passiveListener=l.passive;try{if(typeof l.callback==='function')l.callback.call(target,e);else l.callback.handleEvent(e);}finally{e._passiveListener=passive;}}}
  }
  globalThis.EventTarget=Target;
  Object.defineProperty(Target,Symbol.hasInstance,{value:n=>n instanceof Node || Target.prototype.isPrototypeOf(n) || n===globalThis});
  const nativeAdd=p.addEventListener,nativeRemove=p.removeEventListener;
  p.addEventListener=function(type,callback,options){const signal=options&&typeof options==='object'?options.signal:undefined;if(signal?.aborted)return;nativeAdd.call(this,type,callback,options);if(signal&&callback){const target=this;signal.addEventListener('abort',()=>nativeRemove.call(target,type,callback,options),{once:true});}};
  const windowTarget=new Target();
  globalThis.addEventListener=windowTarget.addEventListener.bind(windowTarget);
  globalThis.removeEventListener=windowTarget.removeEventListener.bind(windowTarget);
  globalThis.dispatchEvent=e=>{e.target=window;e.eventPhase=2;windowTarget.phase(e,true,window);if(!e._immediateStopped)windowTarget.phase(e,false,window);e.currentTarget=null;e.eventPhase=0;return !e.defaultPrevented;};
  globalThis.__ceWindowEvent=(e,capture)=>{e.eventPhase=capture?1:3;windowTarget.phase(e,capture,window);};
  globalThis.AbortController=class AbortController {constructor(){this.signal=new Target();this.signal.aborted=false;}abort(reason){if(this.signal.aborted)return;this.signal.aborted=true;this.signal.reason=reason;this.signal.dispatchEvent(new Event('abort'));}};
  globalThis.PointerEvent=class PointerEvent extends MouseEvent {constructor(t,o={}){super(t,o);this.pointerId=o.pointerId??1;this.pointerType=o.pointerType??'mouse';this.isPrimary=o.isPrimary??true;}};
  globalThis.matchMedia=query=>{query=String(query);const t=new Target();t.media=query;const q=query.replace(/\s+/g,'').toLowerCase(),known={'all':true,'screen':true,'(pointer:fine)':true,'(pointer:coarse)':false,'(pointer:none)':false,'(hover:hover)':true,'(hover:none)':false,'(prefers-reduced-motion)':false,'(prefers-reduced-motion:reduce)':false,'(prefers-reduced-motion:no-preference)':true};if(!(q in known))throw Error('WEFT_MEDIA_PROFILE: '+query);t.matches=known[q];t.addListener=c=>t.addEventListener('change',c);t.removeListener=c=>t.removeEventListener('change',c);return t;};
  globalThis.innerWidth=800;globalThis.innerHeight=600;globalThis.scrollX=0;globalThis.scrollY=0;
  const observers=new Set();let observed=0;
  globalThis.ResizeObserver=class ResizeObserver {
    constructor(callback){if(typeof callback!=='function')throw TypeError('ResizeObserver callback required');if(observers.size>=64)throw Error('WEFT_OBSERVER_LIMIT');this.callback=callback;this.targets=new Map();observers.add(this);}
    observe(n,options={}){if(!(n instanceof Element))throw TypeError('Element required');if(options.box && options.box!=='content-box' && options.box!=='border-box')throw Error('WEFT_OBSERVER_PROFILE');if(!this.targets.has(n)){if(observed>=1024)throw Error('WEFT_OBSERVER_LIMIT');++observed;this.targets.set(n,null);}observers.add(this);}
    unobserve(n){if(this.targets.delete(n))--observed;}
    disconnect(){observed-=this.targets.size;this.targets.clear();observers.delete(this);}
  };
  globalThis.__ceScenePlatform=Object.freeze({deliverObservers(){for(const o of Array.from(observers)){const entries=[];for(const [n,last] of o.targets){const g=n.__ceGeometry().offset,s=getComputedStyle(n),v=k=>parseFloat(s[k])||0,w=Math.max(0,g[2]-v('border-left-width')-v('border-right-width')-v('padding-left')-v('padding-right')),h=Math.max(0,g[3]-v('border-top-width')-v('border-bottom-width')-v('padding-top')-v('padding-bottom'));if(!last || last[0]!==w || last[1]!==h || last[2]!==g[2] || last[3]!==g[3]){o.targets.set(n,[w,h,g[2],g[3]]);entries.push({target:n,contentRect:{x:v('padding-left'),y:v('padding-top'),width:w,height:h},contentBoxSize:[{inlineSize:w,blockSize:h}],borderBoxSize:[{inlineSize:g[2],blockSize:g[3]}]});}}if(entries.length)o.callback(entries,o);}}});
  const decode=s=>s.replace(/&(#x[\da-f]+|#\d+|amp|lt|gt|quot|apos|nbsp);/gi,(_,v)=>{const named={amp:'&',lt:'<',gt:'>',quot:'"',apos:"'",nbsp:'\u00a0'};if(v[0]!=='#')return named[v.toLowerCase()];const n=v[1].toLowerCase()==='x'?parseInt(v.slice(2),16):Number(v.slice(1));return String.fromCodePoint(n>0&&n<=0x10ffff&&!(n>=0xd800&&n<=0xdfff)?n:0xfffd);});
  const escape=s=>String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
  Object.defineProperty(p,'innerHTML',{get(){let count=0;const serialize=n=>{if(++count>256)throw Error('WEFT_FRAGMENT_LIMIT');if(n.nodeType===3)return escape(n.textContent);if(!['div','span','b','strong','br'].includes(n.localName))throw Error('WEFT_FRAGMENT_PROFILE: unsupported serialization');let s='<'+n.localName;for(const k of ['id','class','style']){const v=n.getAttribute(k);if(v!==null)s+=' '+k+'="'+escape(v)+'"';}s+='>';if(n.localName!=='br')s+=Array.from(n.childNodes,serialize).join('')+'</'+n.localName+'>';return s;};const v=Array.from(this.childNodes,serialize).join('');if(v.length>65536)throw Error('WEFT_FRAGMENT_LIMIT');return v;},set(source){
    source=String(source);if(source.length>65536)throw Error('WEFT_FRAGMENT_LIMIT');const plan={children:[]},stack=[plan];let count=0;
    const tokens=source.match(/<[^>]*>|[^<]+/g)||[];if(tokens.join('')!==source)throw Error('WEFT_FRAGMENT_PROFILE: unmatched opening delimiter');
    for(const token of tokens){
      if(token.startsWith('</')){const name=token.slice(2,-1).trim().toLowerCase();if(stack.length===1||stack.pop().tag!==name)throw Error('WEFT_FRAGMENT_PROFILE: closing tag');}
      else if(token.startsWith('<')){const m=/^<([a-z]+)([\s\S]*?)\/?\s*>$/i.exec(token);if(!m||!['div','span','b','strong','br'].includes(m[1].toLowerCase()))throw Error('WEFT_FRAGMENT_PROFILE: unsupported tag');const n={tag:m[1].toLowerCase(),attributes:[],children:[]};let attributes=m[2].trim();while(attributes){const a=/^(style|class|id)\s*=\s*(?:"([^"]*)"|'([^']*)')\s*/i.exec(attributes);if(!a)throw Error('WEFT_FRAGMENT_PROFILE: quoted style/class/id only');n.attributes.push([a[1].toLowerCase(),decode(a[2]??a[3])]);attributes=attributes.slice(a[0].length);}stack.at(-1).children.push(n);if(n.tag!=='br'&&!token.endsWith('/>'))stack.push(n);}
      else stack.at(-1).children.push({text:decode(token)});
      if(++count>256||stack.length>16)throw Error('WEFT_FRAGMENT_LIMIT');
    }
    if(stack.length!==1)throw Error('WEFT_FRAGMENT_PROFILE: unclosed tag');
    const build=n=>{if(n.text!==undefined)return document.createTextNode(n.text);const e=document.createElement(n.tag);for(const [k,v] of n.attributes)e.setAttribute(k,v);for(const c of n.children)e.appendChild(build(c));return e;};const children=plan.children.map(build);this.textContent='';for(const c of children)this.appendChild(c);
  }});
})();
