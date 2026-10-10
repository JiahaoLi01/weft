// Browser reference harness only. Uses real DOM, CSS, SVG and original bundles.
// No computed browser style or screenshot is ever fed to the native renderer.
(() => {
  let seed=0xce202610;Math.random=()=>((seed=(Math.imul(seed,1664525)+1013904223)>>>0)/4294967296);
  let now=0,next=1;const tasks=new Map(),frames=new Map(),origins=new WeakMap();let hovered=null;
  Object.defineProperty(performance,'now',{value:()=>now});
  const schedule=(repeat,callback,delay=0,...args)=>{const id=next++;delay=Math.max(repeat?1:0,Number(delay)||0);tasks.set(id,{repeat,callback,delay,args,due:now+delay});return id};
  window.setTimeout=(callback,delay,...args)=>schedule(false,callback,delay,...args);
  window.setInterval=(callback,delay,...args)=>schedule(true,callback,delay,...args);
  window.clearTimeout=window.clearInterval=id=>tasks.delete(Number(id));
  window.requestAnimationFrame=callback=>{const id=next++;frames.set(id,callback);return id};window.cancelAnimationFrame=id=>frames.delete(id);
  const pump=async()=>{for(let turn=0;turn<32;turn++){const ready=Array.from(tasks).filter(([,t])=>t.due<=now);if(!ready.length)return;for(const [id] of ready){const t=tasks.get(id);if(!t||t.due>now)continue;if(t.repeat)t.due=now+t.delay;else tasks.delete(id);Reflect.apply(t.callback,window,t.args);}await Promise.resolve();}throw Error('reference task quota')};
  function styles(time){for(const a of document.getAnimations()){if(!origins.has(a)){origins.set(a,a instanceof CSSTransition?now:0);a.pause();}a.currentTime=time-origins.get(a);}document.getElementById('root')?.getBoundingClientRect();}
  window.__ceReference={
    async settle(){await pump();styles(0);},
    async step(time){if(time<now)throw Error('reference clock regression');now=time;await pump();const batch=Array.from(frames);for(const [id,callback] of batch){if(frames.delete(id)){callback(now);await Promise.resolve();}}await pump();styles(time);},styles,
    input(type,x,y){const target=document.elementFromPoint(x,y);if(type==='move'&&hovered!==target){hovered?.dispatchEvent(new MouseEvent('mouseout',{clientX:x,clientY:y,relatedTarget:target,bubbles:true}));target?.dispatchEvent(new MouseEvent('mouseover',{clientX:x,clientY:y,relatedTarget:hovered,bubbles:true}));hovered=target;}target?.dispatchEvent(new MouseEvent(type==='move'?'mousemove':'click',{clientX:x,clientY:y,bubbles:true}));},
  };
})();
