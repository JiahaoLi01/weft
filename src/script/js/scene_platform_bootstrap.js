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
  // The host's input devices (__ceScenePlatform.input): navigator.maxTouchPoints, and whether it has a touch screen.
  const input={points:0,touch:false};
  // navigator.clipboard (the host's clipboard, through __ceEditing): writeText and readText, which settle at once.
  const clipboard=Object.freeze({writeText(text){return new Promise((resolve)=>{document.__ceEditing('write',String(text));resolve();});},readText(){return new Promise((resolve)=>resolve(document.__ceEditing('read')));}});
  globalThis.navigator = Object.freeze({get maxTouchPoints(){return input.points;},getGamepads(){return pads.map(s=>s&&gamepadGesture?new Gamepad(gamepadToken,s,true):null);},clipboard}); // Current primary mouse host.
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
  // matchMedia: the View's @media evaluation (its size and input devices) read when asked, of the queries a scene page may
  // ask (others: WEFT_MEDIA_PROFILE); a change event where a watched list's outcome changed (__ceScenePlatform.mediaChanged,
  // after a resize or a change of the devices). Without a host to ask, a mouse device's answers.
  const known={'all':true,'screen':true,'(pointer:fine)':true,'(pointer:coarse)':false,'(pointer:none)':false,'(pointer)':true,'(any-pointer:fine)':true,'(any-pointer:coarse)':false,'(any-pointer:none)':false,'(any-pointer)':true,'(hover:hover)':true,'(hover:none)':false,'(hover)':true,'(any-hover:hover)':true,'(any-hover:none)':false,'(any-hover)':true,'(prefers-reduced-motion)':false,'(prefers-reduced-motion:reduce)':false,'(prefers-reduced-motion:no-preference)':true};
  const normal=query=>query.replace(/\s+/g,'').toLowerCase();
  const evaluate=query=>{try{return document.__ceMatchMedia(query);}catch(e){return known[normal(query)]??false;}};
  const watched=new Set();
  const mediaList=query=>{const t=new Target();t.media=query;let last=evaluate(query),handler=null;Object.defineProperty(t,'matches',{get:()=>evaluate(query),enumerable:true});const watch=()=>{last=evaluate(query);watched.add(t);};const add=t.addEventListener.bind(t);t.addEventListener=(type,c,o)=>{add(type,c,o);if(type==='change')watch();};Object.defineProperty(t,'onchange',{get:()=>handler,set:v=>{handler=typeof v==='function'?v:null;if(handler)watch();},enumerable:true});t.addListener=c=>t.addEventListener('change',c);t.removeListener=c=>t.removeEventListener('change',c);Object.defineProperty(t,'__ceCheck',{value:()=>{const m=evaluate(query);if(m===last)return;last=m;const e=new Event('change');e.matches=m;e.media=query;t.dispatchEvent(e);if(handler)handler.call(t,e);}});return t;};
  globalThis.matchMedia=query=>{query=String(query);if(!(normal(query) in known))throw Error('WEFT_MEDIA_PROFILE: '+query);return mediaList(query);};
  // ontouchstart and the like on window and nodes while the host has a touch screen (Chrome has them only then: pages
  // test 'ontouchstart' in window), as event handlers (a handler that returns false cancels).
  const touchTypes=['touchstart','touchmove','touchend','touchcancel'];
  const handlers=new WeakMap();
  const handlerProperty=type=>({configurable:true,enumerable:true,get(){return handlers.get(this)?.[type]?.handler??null;},set(v){let own=handlers.get(this);if(!own){own={};handlers.set(this,own);}if(own[type])this.removeEventListener(type,own[type].listener);own[type]=null;if(typeof v==='function'){const listener=function(e){if(v.call(this,e)===false)e.preventDefault();};own[type]={handler:v,listener};this.addEventListener(type,listener);}}});
  // HTMLImageElement and Image beside the native img prototype's complete, naturalWidth, naturalHeight, currentSrc and
  // src: alt; width and height, the content box's (rounded) while the img is rendered, else the attribute's integer,
  // else the natural size; decode(), which settles when the img has loaded (an EncodingError without an image); onload
  // and onerror as event handlers.
  {
    const imageProto=Object.getPrototypeOf(document.createElement('img'));
    const domError=(message,name)=>{if(globalThis.DOMException)return new DOMException(message,name);const e=new Error(message);e.name=name;return e;};
    const attributeSize=(e,name)=>{const v=e.getAttribute(name);if(v===null)return null;const m=/^[\t\n\f\r ]*(\d+)/.exec(v);return m?Number(m[1]):null;};
    const rendered=e=>{if(!e.isConnected)return false;for(let x=e;x&&x.nodeType===1;x=x.parentNode)if(getComputedStyle(x).display==='none')return false;return true;};
    const size=(e,axis)=>{if(rendered(e)){const s=getComputedStyle(e),g=e.__ceGeometry().offset,v=k=>parseFloat(s[k])||0;const box=axis?g[3]-v('border-top-width')-v('border-bottom-width')-v('padding-top')-v('padding-bottom'):g[2]-v('border-left-width')-v('border-right-width')-v('padding-left')-v('padding-right');return Math.max(0,Math.round(box));}const a=attributeSize(e,axis?'height':'width');return a!==null?a:axis?e.naturalHeight:e.naturalWidth;};
    Object.defineProperties(imageProto,{
      alt:{configurable:true,enumerable:true,get(){return this.getAttribute('alt')??'';},set(v){this.setAttribute('alt',String(v));}},
      width:{configurable:true,enumerable:true,get(){return size(this,0);},set(v){this.setAttribute('width',String(Number(v)>>>0));}},
      height:{configurable:true,enumerable:true,get(){return size(this,1);},set(v){this.setAttribute('height',String(Number(v)>>>0));}},
      onload:handlerProperty('load'),
      onerror:handlerProperty('error'),
      decode:{configurable:true,writable:true,value(){return new Promise((resolve,reject)=>{const fail=()=>reject(domError('The source image cannot be decoded.','EncodingError'));if(!this.getAttribute('src')){fail();return;}if(this.complete){if(this.naturalWidth)resolve();else fail();return;}const done=e=>{this.removeEventListener('load',done);this.removeEventListener('error',done);if(e.type==='load')resolve();else fail();};this.addEventListener('load',done);this.addEventListener('error',done);});}}
    });
    globalThis.HTMLImageElement=function HTMLImageElement(){throw new TypeError('Illegal constructor');};
    HTMLImageElement.prototype=imageProto;
    Object.defineProperty(imageProto,'constructor',{configurable:true,writable:true,value:HTMLImageElement});
    // document.execCommand: copy, cut, undo, redo and selectAll as the shortcuts run them (__ceEditing); paste and the
    // rest do not run (false), as for a page in Chrome.
    document.execCommand=(command)=>document.__ceEditing('command',String(command));
    document.queryCommandSupported=(command)=>['copy','cut','paste','undo','redo','selectAll'].includes(String(command));
    globalThis.Image=function Image(width,height){const e=document.createElement('img');if(width!==undefined)e.width=width;if(height!==undefined)e.height=height;return e;};
    Image.prototype=imageProto;
  }
  globalThis.innerWidth=800;globalThis.innerHeight=600;globalThis.scrollX=0;globalThis.scrollY=0;
  const observers=new Set();let observed=0;
  globalThis.ResizeObserver=class ResizeObserver {
    constructor(callback){if(typeof callback!=='function')throw TypeError('ResizeObserver callback required');if(observers.size>=64)throw Error('WEFT_OBSERVER_LIMIT');this.callback=callback;this.targets=new Map();observers.add(this);}
    observe(n,options={}){if(!(n instanceof Element))throw TypeError('Element required');if(options.box && options.box!=='content-box' && options.box!=='border-box')throw Error('WEFT_OBSERVER_PROFILE');if(!this.targets.has(n)){if(observed>=1024)throw Error('WEFT_OBSERVER_LIMIT');++observed;this.targets.set(n,null);}observers.add(this);}
    unobserve(n){if(this.targets.delete(n))--observed;}
    disconnect(){observed-=this.targets.size;this.targets.clear();observers.delete(this);}
  };
  globalThis.__ceScenePlatform=Object.freeze({deliverObservers(){for(const o of Array.from(observers)){const entries=[];for(const [n,last] of o.targets){const g=n.__ceGeometry().offset,s=getComputedStyle(n),v=k=>parseFloat(s[k])||0,w=Math.max(0,g[2]-v('border-left-width')-v('border-right-width')-v('padding-left')-v('padding-right')),h=Math.max(0,g[3]-v('border-top-width')-v('border-bottom-width')-v('padding-top')-v('padding-bottom'));if(!last || last[0]!==w || last[1]!==h || last[2]!==g[2] || last[3]!==g[3]){o.targets.set(n,[w,h,g[2],g[3]]);entries.push({target:n,contentRect:{x:v('padding-left'),y:v('padding-top'),width:w,height:h},contentBoxSize:[{inlineSize:w,blockSize:h}],borderBoxSize:[{inlineSize:g[2],blockSize:g[3]}]});}}if(entries.length)o.callback(entries,o);}},input(points,touch){input.points=points;if(touch===input.touch)return;input.touch=touch;const nodes=Object.getPrototypeOf(document);for(const t of touchTypes)for(const o of [globalThis,nodes]){if(touch)Object.defineProperty(o,'on'+t,handlerProperty(t));else delete o['on'+t];}},mediaChanged(){for(const t of Array.from(watched))t.__ceCheck();},mediaList});
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

// ---- Web Animations 1: element.animate(), getAnimations(), Animation, KeyframeEffect, document.timeline. The timing
// model runs here, as the specification's procedures go; the effects' values are the scene's, solved at each frame's
// time (src/scene/scene.cpp) from what each animation last told its target element through __ceAnimation: its keyframes
// as CSS declarations, its timing, and its start or hold time and rate (src/dom/animation.hpp). The frame's update step
// (__ceUpdateAnimations, from the animation frame host before its callbacks) moves the timeline, completes pending play
// and pause tasks, updates finished states and removes replaced animations; the events it and scripts queue go out
// before the frame's callbacks (__ceSendAnimationEvents). Outside this profile: composite operations other than
// replace, iterationComposite, pseudo-elements, timelines other than the document's, CSS animations and transitions in
// getAnimations().
(() => {
  const p = Node.prototype;
  const domError = (message, name) => {
    if (globalThis.DOMException) return new DOMException(message, name);
    const e = new Error(message);
    e.name = name;
    return e;
  };
  let timelineTime = performance.now();
  // Animations that are not idle, in composite order (their order, from the global list).
  const live = [];
  let nextOrder = 1;
  let queued = [];

  // ---- Easing functions (CSS Easing 1), as the scene evaluates them.
  const bezier = (x1, y1, x2, y2) => {
    if (!(x1 >= 0 && x1 <= 1 && x2 >= 0 && x2 <= 1)) throw new TypeError('cubic-bezier x values must be in [0, 1]');
    const curve = (t, a, b) => 3 * (1 - t) * (1 - t) * t * a + 3 * (1 - t) * t * t * b + t * t * t;
    return (x) => {
      x = Math.min(Math.max(x, 0), 1);
      let lo = 0, hi = 1;
      for (let i = 0; i < 32; ++i) {
        const mid = (lo + hi) / 2;
        if (curve(mid, x1, x2) < x) lo = mid; else hi = mid;
      }
      return curve((lo + hi) / 2, y1, y2);
    };
  };
  const steps = (n, at) => {
    const start = at === 'start' || at === 'jump-start', both = at === 'jump-both', none = at === 'jump-none';
    const jumps = both ? n + 1 : none ? n - 1 : n;
    return (x) => {
      x = Math.min(Math.max(x, 0), 1);
      let step = Math.floor(x * n);
      if (start || both) ++step;
      return Math.min(Math.max(step, 0), jumps) / jumps;
    };
  };
  const named = {
    linear: (x) => x, ease: bezier(0.25, 0.1, 0.25, 1), 'ease-in': bezier(0.42, 0, 1, 1),
    'ease-out': bezier(0, 0, 0.58, 1), 'ease-in-out': bezier(0.42, 0, 0.58, 1), 'step-start': steps(1, 'start'),
    'step-end': steps(1, 'end'),
  };
  const easing = (text) => {
    const t = String(text).trim();
    if (named[t]) return named[t];
    let m = /^cubic-bezier\(([^()]*)\)$/.exec(t);
    if (m) {
      const v = m[1].split(',').map((x) => Number(x.trim()));
      if (v.length === 4 && v.every(Number.isFinite)) return bezier(...v);
    }
    m = /^steps\(\s*(\d+)\s*(?:,\s*(start|end|jump-start|jump-end|jump-none|jump-both)\s*)?\)$/.exec(t);
    if (m && Number(m[1]) >= (m[2] === 'jump-none' ? 2 : 1)) return steps(Number(m[1]), m[2] ?? 'end');
    throw new TypeError(`'${t}' is not a valid value for easing`);
  };

  // ---- Keyframes (Web Animations 1 §5.3.3, process a keyframes argument).
  const reserved = new Set(['offset', 'easing', 'composite']);
  const cssName = (k) => (k === 'cssFloat' ? 'float' : k === 'cssOffset' ? 'offset' :
    k.replace(/[A-Z]/g, (c) => '-' + c.toLowerCase()));
  const composite = (c) => {
    const v = c === undefined || c === null ? 'auto' : String(c);
    if (v === 'auto' || v === 'replace') return v;
    if (v === 'add' || v === 'accumulate') throw domError(`composite '${v}' is outside the scene profile`, 'NotSupportedError');
    throw new TypeError(`'${v}' is not a valid value for composite`);
  };
  const keyframes = (input) => {
    if (input === undefined || input === null) return [];
    if (typeof input !== 'object') throw new TypeError("Failed to execute 'animate': the keyframes are not an object");
    let frames;
    if (typeof input[Symbol.iterator] === 'function') {
      frames = Array.from(input, (k) => {
        if (k === null || typeof k !== 'object') throw new TypeError('Keyframes must be objects');
        const f = {
          offset: k.offset === undefined || k.offset === null ? null : Number(k.offset),
          easing: k.easing === undefined ? 'linear' : String(k.easing), composite: composite(k.composite), values: {},
        };
        for (const key of Object.keys(k)) if (!reserved.has(key)) f.values[key] = String(k[key]);
        return f;
      });
    } else {
      // Each property's values spread evenly over [0, 1] (one value at 1), merged where their offsets meet; then the
      // offset, easing and composite lists, in keyframe order (easings repeat).
      const byOffset = new Map();
      for (const key of Object.keys(input)) {
        if (reserved.has(key)) continue;
        const list = typeof input[key] === 'object' && input[key] !== null && typeof input[key][Symbol.iterator] ===
          'function' && typeof input[key] !== 'string' ? Array.from(input[key]) : [input[key]];
        list.forEach((v, i) => {
          const o = list.length === 1 ? 1 : i / (list.length - 1);
          if (!byOffset.has(o)) byOffset.set(o, {offset: null, computed: o, easing: 'linear', composite: 'auto', values: {}});
          byOffset.get(o).values[key] = String(v);
        });
      }
      frames = [...byOffset.values()].sort((a, b) => a.computed - b.computed);
      const list = (v) => (v === undefined ? [] : typeof v === 'object' && v !== null && typeof v !== 'string' ?
        Array.from(v) : [v]);
      list(input.offset).forEach((o, i) => { if (i < frames.length) frames[i].offset = o === null ? null : Number(o); });
      const eases = list(input.easing);
      if (eases.length) frames.forEach((f, i) => { f.easing = String(eases[i % eases.length]); });
      const composites = list(input.composite);
      if (composites.length) frames.forEach((f, i) => { f.composite = composite(composites[i % composites.length]); });
    }
    let last = -Infinity;
    for (const f of frames) {
      if (f.offset !== null) {
        if (!(f.offset >= 0 && f.offset <= 1)) throw new TypeError('Offsets must be null or in the range [0,1].');
        if (f.offset < last) throw new TypeError('Offsets must be monotonically non-decreasing.');
        last = f.offset;
      }
      easing(f.easing);
    }
    return frames;
  };
  // The computed offsets (§5.3.3, compute missing keyframe offsets): the first 0 and the last 1 where not given, those
  // between spaced evenly; a property-indexed keyframe's own spread where no offset is given.
  const computedOffsets = (frames) => {
    const out = frames.map((f) => (f.offset !== null ? f.offset : f.computed !== undefined ? f.computed : null));
    if (!out.length) return out;
    if (out.length > 1 && out[0] === null) out[0] = 0;
    if (out[out.length - 1] === null) out[out.length - 1] = 1;
    for (let i = 0; i < out.length;) {
      if (out[i] !== null) { ++i; continue; }
      let j = i;
      while (out[j] === null) ++j;
      const a = out[i - 1], b = out[j];
      for (let k = i; k < j; ++k) out[k] = a + (b - a) * (k - i + 1) / (j - i + 1);
      i = j;
    }
    return out;
  };

  // ---- Timing (§4, EffectTiming).
  const defaults = {delay: 0, direction: 'normal', duration: 'auto', easing: 'linear', endDelay: 0, fill: 'auto',
    iterationStart: 0, iterations: 1};
  const timing = (options, base = defaults) => {
    const t = {...base};
    if (typeof options === 'number' || (options !== null && typeof options === 'object' && options instanceof Number)) {
      options = {duration: Number(options)};
    } else if (options === undefined || options === null) {
      options = {};
    } else if (typeof options !== 'object') {
      throw new TypeError('The timing options are not an object or a number');
    }
    const finite = (k) => {
      const v = Number(options[k]);
      if (!Number.isFinite(v)) throw new TypeError(`${k} must be a finite number`);
      return v;
    };
    if (options.delay !== undefined) t.delay = finite('delay');
    if (options.endDelay !== undefined) t.endDelay = finite('endDelay');
    if (options.iterationStart !== undefined) {
      t.iterationStart = finite('iterationStart');
      if (t.iterationStart < 0) throw new TypeError('iterationStart must be non-negative');
    }
    if (options.iterations !== undefined) {
      t.iterations = Number(options.iterations);
      if (Number.isNaN(t.iterations) || t.iterations < 0) throw new TypeError('iterations must be non-negative');
    }
    if (options.duration !== undefined) {
      const d = options.duration;
      if (d === 'auto') t.duration = 'auto';
      else if (typeof d === 'string') throw new TypeError(`'${d}' is not a valid duration`);
      else {
        t.duration = Number(d);
        if (Number.isNaN(t.duration) || t.duration < 0) throw new TypeError('duration must be non-negative or auto');
      }
    }
    if (options.direction !== undefined) {
      t.direction = String(options.direction);
      if (!['normal', 'reverse', 'alternate', 'alternate-reverse'].includes(t.direction)) {
        throw new TypeError(`'${t.direction}' is not a valid value for direction`);
      }
    }
    if (options.fill !== undefined) {
      t.fill = String(options.fill);
      if (!['none', 'forwards', 'backwards', 'both', 'auto'].includes(t.fill)) {
        throw new TypeError(`'${t.fill}' is not a valid value for fill`);
      }
    }
    if (options.easing !== undefined) {
      t.easing = String(options.easing);
      easing(t.easing);
    }
    return t;
  };
  // The computed timing of an effect at a local time (§4.6 to §4.9), with the playback direction's sign; and its
  // phase.
  const computed = (t, local, rate) => {
    const duration = t.duration === 'auto' ? 0 : t.duration;
    const fill = t.fill === 'auto' ? 'none' : t.fill;
    const active = duration === 0 || t.iterations === 0 ? 0 : duration * t.iterations;
    const end = Math.max(t.delay + active + t.endDelay, 0);
    const out = {delay: t.delay, direction: t.direction, duration, easing: t.easing, endDelay: t.endDelay, fill,
      iterationStart: t.iterationStart, iterations: t.iterations, activeDuration: active, endTime: end,
      localTime: local, progress: null, currentIteration: null};
    if (local === null) return {out, phase: 'idle'};
    const beforeActive = Math.max(Math.min(t.delay, end), 0);
    const activeAfter = Math.max(Math.min(t.delay + active, end), 0);
    const backwards = rate < 0;
    const phase = local < beforeActive || (backwards && local === beforeActive) ? 'before' :
      local > activeAfter || (!backwards && local === activeAfter) ? 'after' : 'active';
    let activeTime = null;
    if (phase === 'before') {
      if (fill === 'backwards' || fill === 'both') activeTime = Math.max(local - t.delay, 0);
    } else if (phase === 'active') {
      activeTime = local - t.delay;
    } else if (fill === 'forwards' || fill === 'both') {
      activeTime = Math.max(Math.min(local - t.delay, active), 0);
    }
    if (activeTime === null) return {out, phase};
    let overall = (duration === 0 ? (phase === 'before' ? 0 : t.iterations) : activeTime / duration) + t.iterationStart;
    let simple = overall === Infinity ? t.iterationStart % 1 : overall % 1;
    if (simple === 0 && phase !== 'before' && activeTime === active && t.iterations !== 0) simple = 1;
    const iteration = phase === 'after' && t.iterations === Infinity ? Infinity :
      simple === 1 ? Math.floor(overall) - 1 : Math.floor(overall);
    let forwards = t.direction !== 'reverse';
    if (t.direction === 'alternate' || t.direction === 'alternate-reverse') {
      const d = t.direction === 'alternate' ? iteration : iteration + 1;
      forwards = d === Infinity || d % 2 === 0;
    }
    out.progress = easing(t.easing)(forwards ? simple : 1 - simple);
    out.currentIteration = iteration;
    return {out, phase};
  };

  // ---- Effects.
  globalThis.AnimationEffect = class AnimationEffect {
    getTiming() {
      const t = this._timing;
      return {delay: t.delay, direction: t.direction, duration: t.duration, easing: t.easing, endDelay: t.endDelay,
        fill: t.fill, iterationStart: t.iterationStart, iterations: t.iterations};
    }
    getComputedTiming() {
      const a = this._animation;
      return computed(this._timing, a ? a.currentTime : null, a ? a._rate : 1).out;
    }
    updateTiming(options = {}) {
      this._timing = timing(options, this._timing);
      this._changed();
    }
    _changed() {
      const a = this._animation;
      if (a) { a._updateFinished(false, false); a._sync(); }
    }
  };
  globalThis.KeyframeEffect = class KeyframeEffect extends AnimationEffect {
    constructor(target, frames, options) {
      super();
      if (target instanceof KeyframeEffect && frames === undefined) {
        const source = target;
        this._target = source._target;
        this._frames = source._frames.map((f) => ({...f, values: {...f.values}}));
        this._timing = {...source._timing};
      } else {
        if (target !== null && !(target instanceof Element)) throw new TypeError('The target is not an element');
        this._target = target;
        this._frames = keyframes(frames);
        this._timing = timing(options);
      }
      this._animation = null;
      this.pseudoElement = null;
      this.composite = 'replace';
    }
    get target() { return this._target; }
    set target(v) {
      if (v !== null && !(v instanceof Element)) throw new TypeError('The target is not an element');
      this._target = v;
      this._changed();
    }
    getKeyframes() {
      const offsets = computedOffsets(this._frames);
      return this._frames.map((f, i) => ({offset: f.offset, easing: f.easing, composite: f.composite, ...f.values,
        computedOffset: offsets[i]}));
    }
    setKeyframes(frames) {
      this._frames = keyframes(frames);
      this._changed();
    }
    // The CSS properties its keyframes name.
    _properties() {
      const out = new Set();
      for (const f of this._frames) for (const k of Object.keys(f.values)) out.add(cssName(k));
      return [...out];
    }
  };

  // ---- Animations.
  globalThis.AnimationTimeline = class AnimationTimeline {
    get currentTime() { return timelineTime; }
  };
  globalThis.DocumentTimeline = class DocumentTimeline extends AnimationTimeline {};
  const timeline = new DocumentTimeline();
  globalThis.AnimationPlaybackEvent = class AnimationPlaybackEvent extends Event {
    constructor(type, init = {}) {
      super(type, init);
      this.currentTime = init.currentTime ?? null;
      this.timelineTime = init.timelineTime ?? null;
    }
  };
  // A promise with its settling functions; rejected ones are handled, as the specification marks them.
  const deferred = () => {
    const d = {settled: false};
    d.promise = new Promise((resolve, reject) => { d.resolve = resolve; d.reject = reject; });
    d.promise.catch(() => {});
    return d;
  };
  const settled = (value) => {
    const d = deferred();
    d.resolve(value);
    d.settled = true;
    return d;
  };
  const handler = (type) => ({
    configurable: true, enumerable: true,
    get() { return this._handlers[type] ?? null; },
    set(v) {
      if (!(type in this._handlers)) {
        this.addEventListener(type, (e) => { const h = this._handlers[type]; if (typeof h === 'function') h.call(this, e); });
      }
      this._handlers[type] = typeof v === 'function' ? v : null;
    },
  });

  class Animation extends EventTarget {
    constructor(effect = null, line = timeline) {
      super();
      if (effect !== null && !(effect instanceof AnimationEffect)) throw new TypeError('The effect is not an AnimationEffect');
      if (line !== null && line !== undefined && line !== timeline) {
        throw domError('timelines other than the document timeline are outside the scene profile', 'NotSupportedError');
      }
      this.id = '';
      this._handlers = {};
      this._effect = null;
      this._startTime = null;
      this._holdTime = null;
      this._rate = 1;
      this._pendingRate = null;
      this._pendingPlay = false;
      this._pendingPause = false;
      this._previousCurrentTime = null;
      this._replaceState = 'active';
      this._order = 0;
      this._synced = null;
      this._sent = '';
      this._ready = settled(this);
      this._finished = deferred();
      this._notifying = false;
      if (effect) this.effect = effect;
    }
    get timeline() { return timeline; }
    get effect() { return this._effect; }
    set effect(e) {
      if (e === this._effect) return;
      if (e !== null && !(e instanceof AnimationEffect)) throw new TypeError('The effect is not an AnimationEffect');
      if (e && e._animation) e._animation.effect = null;
      if (this._effect) this._effect._animation = null;
      this._effect = e;
      if (e) e._animation = this;
      this._updateFinished(false, false);
      this._sync();
    }
    get startTime() { return this._startTime; }
    set startTime(v) {
      const time = v === null ? null : Number(v);
      const previous = this.currentTime;
      this._applyPendingRate();
      this._startTime = time;
      this._holdTime = time !== null && this._rate !== 0 ? null : previous;
      if (this._pendingPlay || this._pendingPause) {
        this._pendingPlay = this._pendingPause = false;
        this._resolveReady();
      }
      this._updateFinished(true, false);
      this._sync();
    }
    get currentTime() {
      if (this._holdTime !== null) return this._holdTime;
      if (this._startTime === null) return null;
      return (timelineTime - this._startTime) * this._rate;
    }
    set currentTime(v) {
      this._setCurrentTime(v === null ? null : Number(v));
      if (this._pendingPause) {
        this._holdTime = this.currentTime;
        this._applyPendingRate();
        this._startTime = null;
        this._pendingPause = false;
        this._resolveReady();
      }
      this._updateFinished(true, false);
      this._sync();
    }
    get playbackRate() { return this._rate; }
    set playbackRate(v) {
      this._pendingRate = null;
      const previous = this.currentTime;
      this._rate = Number(v);
      if (previous !== null) this._setCurrentTime(previous);
      this._updateFinished(false, false);
      this._sync();
    }
    get playState() {
      const current = this.currentTime;
      if (current === null && this._startTime === null && !this._pendingPlay && !this._pendingPause) return 'idle';
      if (this._pendingPause || (this._startTime === null && !this._pendingPlay)) return 'paused';
      const rate = this._effectiveRate();
      if (current !== null && ((rate > 0 && current >= this._end()) || (rate < 0 && current <= 0))) return 'finished';
      return 'running';
    }
    get replaceState() { return this._replaceState; }
    get pending() { return this._pendingPlay || this._pendingPause; }
    get ready() { return this._ready.promise; }
    get finished() { return this._finished.promise; }
    play() {
      this._play(true);
      this._sync();
    }
    pause() {
      if (this._pendingPause) return;
      let seek = null;
      if (this.currentTime === null) {
        if (this._rate >= 0) seek = 0;
        else if (this._end() === Infinity) throw domError('Cannot pause an animation that plays backwards from an infinite end', 'InvalidStateError');
        else seek = this._end();
      }
      if (seek !== null) this._holdTime = seek;
      if (this._pendingPlay) this._pendingPlay = false;
      else this._ready = deferred();
      this._pendingPause = true;
      this._attach();
      this._updateFinished(false, false);
      this._sync();
    }
    finish() {
      const rate = this._effectiveRate();
      if (rate === 0) throw domError('Cannot finish an animation with a playback rate of 0', 'InvalidStateError');
      if (rate > 0 && this._end() === Infinity) {
        throw domError('Cannot finish an animation with an infinite end', 'InvalidStateError');
      }
      this._applyPendingRate();
      const limit = this._rate > 0 ? this._end() : 0;
      this._setCurrentTime(limit);
      if (this._startTime === null) this._startTime = timelineTime - limit / this._rate;
      if (this._pendingPause && this._startTime !== null) {
        this._holdTime = null;
        this._pendingPause = false;
        this._resolveReady();
      }
      if (this._pendingPlay && this._startTime !== null) {
        this._pendingPlay = false;
        this._resolveReady();
      }
      this._attach();
      this._updateFinished(true, true);
      this._sync();
    }
    cancel() {
      if (this.playState !== 'idle') {
        if (this._pendingPlay || this._pendingPause) {
          this._pendingPlay = this._pendingPause = false;
          this._applyPendingRate();
          this._ready.reject(domError('The user aborted a request.', 'AbortError'));
          this._ready = settled(this);
        }
        if (!this._finished.settled) {
          this._finished.reject(domError('The user aborted a request.', 'AbortError'));
        }
        this._finished = deferred();
        this._queue('cancel', null);
      }
      this._holdTime = null;
      this._startTime = null;
      this._detach();
      this._sync();
    }
    reverse() {
      const original = this._pendingRate;
      this._pendingRate = -this._effectiveRate();
      try {
        this._play(true);
      } catch (e) {
        this._pendingRate = original;
        throw e;
      }
      this._sync();
    }
    updatePlaybackRate(rate) {
      const previous = this.playState;
      this._pendingRate = Number(rate);
      if (this._pendingPlay || this._pendingPause) return;
      if (previous === 'idle' || previous === 'paused' || this.currentTime === null) {
        this._applyPendingRate();
      } else if (previous === 'finished') {
        const unconstrained = this._startTime === null ? null : (timelineTime - this._startTime) * this._rate;
        this._startTime = this._pendingRate === 0 ? timelineTime : timelineTime - unconstrained / this._pendingRate;
        this._applyPendingRate();
        this._updateFinished(false, false);
      } else {
        this._play(false);
      }
      this._sync();
    }
    persist() {
      this._replaceState = 'persisted';
    }
    commitStyles() {
      const target = this._effect?._target;
      if (!target) throw domError('The animation has no target element', 'NoModificationAllowedError');
      // Its values over those beneath: the animations above it set aside while the values are read.
      const above = live.filter((a) => a._order > this._order && a._effect?._target === target && a._synced);
      for (const a of above) { target.__ceAnimation(a._order, null); a._sent = ''; }
      const style = getComputedStyle(target);
      const values = this._effect._properties().map((k) => [k, style.getPropertyValue(k)]);
      for (const a of above) a._sync();
      for (const [k, v] of values) target.style.setProperty(k, v);
    }
    // Play an animation (§4.4.4).
    _play(rewind) {
      const aborted = this._pendingPause;
      let pendingReady = false;
      let seek = null;
      const previous = this.currentTime;
      const rate = this._effectiveRate();
      const end = this._end();
      if (rewind) {
        if (rate > 0 && (previous === null || previous < 0 || previous >= end)) seek = 0;
        else if (rate < 0 && (previous === null || previous <= 0 || previous > end)) {
          if (end === Infinity) throw domError('Cannot play backwards from an infinite end', 'InvalidStateError');
          seek = end;
        }
      }
      if (rate === 0 && previous === null) seek = 0;
      if (seek !== null) {
        this._startTime = null;
        this._holdTime = seek;
      }
      if (this._holdTime !== null) this._startTime = null;
      if (this._pendingPlay || this._pendingPause) {
        this._pendingPause = false;
        pendingReady = true;
      }
      if (this._holdTime === null && seek === null && !aborted && this._pendingRate === null) return;
      if (!pendingReady) this._ready = deferred();
      this._pendingPlay = true;
      this._attach();
      this._updateFinished(false, false);
    }
    // The pending task, completed at the frame's time (§4.4.4, §4.4.10).
    _commitPending(time) {
      if (this._pendingPlay) {
        if (this._holdTime !== null) {
          this._applyPendingRate();
          this._startTime = this._rate === 0 ? time : time - this._holdTime / this._rate;
          if (this._rate !== 0) this._holdTime = null;
        } else if (this._startTime !== null && this._pendingRate !== null) {
          const current = (time - this._startTime) * this._rate;
          this._applyPendingRate();
          if (this._rate === 0) {
            this._holdTime = current;
            this._startTime = time;
          } else {
            this._startTime = time - current / this._rate;
          }
        }
        this._pendingPlay = false;
      } else if (this._pendingPause) {
        if (this._startTime !== null && this._holdTime === null) this._holdTime = (time - this._startTime) * this._rate;
        this._applyPendingRate();
        this._startTime = null;
        this._pendingPause = false;
      }
      this._resolveReady();
      this._updateFinished(false, false);
    }
    // Update an animation's finished state (§4.4.15).
    _updateFinished(didSeek, synchronously) {
      const unconstrained = didSeek ? this.currentTime :
        this._startTime === null ? null : (timelineTime - this._startTime) * this._rate;
      if (unconstrained !== null && this._startTime !== null && !this._pendingPlay && !this._pendingPause) {
        const end = this._end();
        if (this._rate > 0 && unconstrained >= end) {
          this._holdTime = didSeek ? unconstrained :
            Math.max(this._previousCurrentTime === null ? -Infinity : this._previousCurrentTime, end);
        } else if (this._rate < 0 && unconstrained <= 0) {
          this._holdTime = didSeek ? unconstrained :
            Math.min(this._previousCurrentTime === null ? Infinity : this._previousCurrentTime, 0);
        } else if (this._rate !== 0) {
          if (didSeek && this._holdTime !== null) this._startTime = timelineTime - this._holdTime / this._rate;
          this._holdTime = null;
        }
      }
      this._previousCurrentTime = this.currentTime;
      const finished = this.playState === 'finished';
      if (finished && !this._finished.settled) {
        if (synchronously) {
          this._notifying = false;
          this._notifyFinished();
        } else if (!this._notifying) {
          this._notifying = true;
          Promise.resolve().then(() => {
            if (!this._notifying) return;
            this._notifying = false;
            this._notifyFinished();
          });
        }
      }
      if (!finished && this._finished.settled) this._finished = deferred();
    }
    _notifyFinished() {
      if (this.playState !== 'finished') return;
      this._finished.resolve(this);
      this._finished.settled = true;
      this._queue('finish', this.currentTime);
    }
    _queue(type, currentTime) {
      queued.push([this, new AnimationPlaybackEvent(type, {currentTime, timelineTime})]);
    }
    _resolveReady() {
      if (!this._ready.settled) {
        this._ready.resolve(this);
        this._ready.settled = true;
      }
    }
    _effectiveRate() { return this._pendingRate ?? this._rate; }
    _applyPendingRate() {
      if (this._pendingRate !== null) {
        this._rate = this._pendingRate;
        this._pendingRate = null;
      }
    }
    _setCurrentTime(seek) {
      if (seek === null) {
        if (this.currentTime !== null) throw new TypeError('The current time cannot be set to null');
        return;
      }
      if (this._holdTime !== null || this._startTime === null || this._rate === 0) this._holdTime = seek;
      else this._startTime = timelineTime - seek / this._rate;
      this._previousCurrentTime = null;
      this._attach();
    }
    _end() {
      return this._effect ? computed(this._effect._timing, null, 1).out.endTime : 0;
    }
    // In the composite order of animations that are not idle: an idle one that starts goes last.
    _attach() {
      if (live.includes(this)) return;
      if (!this._order) this._order = nextOrder++;
      const at = live.findIndex((a) => a._order > this._order);
      live.splice(at < 0 ? live.length : at, 0, this);
    }
    _detach() {
      const at = live.indexOf(this);
      if (at >= 0) live.splice(at, 1);
      this._order = 0;
    }
    // In effect: its effect has a progress at its current time.
    _inEffect() {
      return !!this._effect && computed(this._effect._timing, this.currentTime, this._rate).out.progress !== null;
    }
    // Relevant (§5.4.1): current or in effect, and not removed.
    _relevant() {
      if (!this._effect || this.playState === 'idle' || this._replaceState === 'removed') return false;
      const {out, phase} = computed(this._effect._timing, this.currentTime, this._rate);
      return out.progress !== null || phase === 'before' || phase === 'active';
    }
    // What its target's scene takes: its keyframes, timing and time, or none (idle, removed, or no effect there).
    _sync() {
      const target = this._effect?._target ?? null;
      const show = target && this._order && this._replaceState !== 'removed' && this.playState !== 'idle' &&
        (this._relevant() || this.pending);
      if (this._synced && (this._synced !== target || !show || this._sentOrder !== this._order)) {
        this._synced.__ceAnimation(this._sentOrder, null);
        this._synced = null;
        this._sent = '';
      }
      if (!show) return;
      const e = this._effect, t = e._timing;
      const offsets = computedOffsets(e._frames);
      const frames = e._frames.map((f, i) => [offsets[i], f.easing,
        Object.entries(f.values).map(([k, v]) => cssName(k) + ':' + v).join(';')]);
      const times = [t.delay, t.endDelay, t.iterationStart, t.iterations, t.duration === 'auto' ? 0 : t.duration,
        t.direction, t.fill === 'auto' ? 'none' : t.fill, t.easing];
      const playback = [this._startTime, this._holdTime, this._rate];
      const sent = JSON.stringify([frames, times, playback]);
      if (sent === this._sent && this._synced === target) return;
      target.__ceAnimation(this._order, frames, times, playback);
      this._synced = target;
      this._sentOrder = this._order;
      this._sent = sent;
    }
  }
  Object.defineProperties(Animation.prototype, {onfinish: handler('finish'), oncancel: handler('cancel'),
    onremove: handler('remove')});
  globalThis.Animation = Animation;

  // The frame's update (HTML's update animations and send events): the timeline's time, pending tasks, finished states,
  // replaced animations; and its events, in the order they were queued.
  globalThis.__ceUpdateAnimations = (time) => {
    timelineTime = time;
    for (const a of live.slice()) {
      if (a._pendingPlay || a._pendingPause) a._commitPending(time);
      a._updateFinished(false, false);
    }
    // Remove replaced animations (§5.5): a finished animation in effect whose every property an animation above it on
    // the same element (finished and in effect too) also takes, unless it persists.
    const replaceable = live.filter((a) => a.playState === 'finished' && a._replaceState !== 'removed' &&
      a._effect?._target && a._inEffect());
    const taken = new Map();
    for (let i = replaceable.length - 1; i >= 0; --i) {
      const a = replaceable[i], target = a._effect._target, properties = a._effect._properties();
      const above = taken.get(target) ?? new Set();
      if (a._replaceState === 'active' && properties.length && properties.every((k) => above.has(k))) {
        a._replaceState = 'removed';
        a._queue('remove', a.currentTime);
      }
      for (const k of properties) above.add(k);
      taken.set(target, above);
    }
    for (const a of live.slice()) {
      a._sync();
      // A finished animation out of effect, or removed, keeps nothing on the frame's list.
      if (!a.pending && a.playState === 'finished' && (a._replaceState === 'removed' || !a._inEffect())) {
        const at = live.indexOf(a);
        if (at >= 0) live.splice(at, 1);
      }
    }
  };
  globalThis.__ceSendAnimationEvents = () => {
    const events = queued;
    queued = [];
    for (const [animation, event] of events) {
      try {
        animation.dispatchEvent(event);
      } catch (e) {
        Promise.reject(e);
      }
    }
  };

  // element.animate(), getAnimations() (of an element, or of the document), document.timeline.
  const animate = function(frames, options) {
    const effect = new KeyframeEffect(this, frames, options);
    const animation = new Animation(effect, timeline);
    if (options !== null && typeof options === 'object' && options.id !== undefined) animation.id = String(options.id);
    animation.play();
    return animation;
  };
  const relevant = () => live.filter((a) => a._relevant());
  p.animate = animate;
  p.getAnimations = function(options) {
    if (this.nodeType === 9) return relevant();
    const subtree = !!(options && options.subtree);
    return relevant().filter((a) => {
      const t = a._effect._target;
      return t === this || (subtree && this.contains(t));
    });
  };
  Element.prototype.animate = animate;
  Element.prototype.getAnimations = p.getAnimations;
  Object.defineProperty(document, 'timeline', {configurable: true, enumerable: true, get: () => timeline});
})();

