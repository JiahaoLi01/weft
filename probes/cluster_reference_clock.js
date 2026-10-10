// Independent Chromium clock/input controller. Real DOM/layout/SVG/paint.
(() => {
  Date.now=()=>Math.floor(performance.now());
  // Exercise the libraries' own JS animation fallback on both hosts; browser
  // DOM/CSS/SVG rendering remains real. No library source is changed.
  Element.prototype.animate=undefined;
  let hover=null,pressed=null;
  const path=n=>{const a=[];for(;n instanceof Element;n=n.parentElement)a.push(n);return a;};
  const emit=(n,type,x,y,buttons=0,related=null)=>{if(!n)return;const options={clientX:x,clientY:y,buttons,relatedTarget:related,bubbles:!type.endsWith('enter')&&!type.endsWith('leave'),cancelable:true,view:window,pointerId:1,pointerType:'mouse',isPrimary:true};n.dispatchEvent(type.startsWith('pointer')?new PointerEvent(type,options):new MouseEvent(type,options));};
  window.__ceClusterInput=input=>{
    let {x,y}=input;if(input.selector){let list=Array.from(document.querySelectorAll(input.selector));if(input.visible)list=list.filter(n=>{const r=n.getBoundingClientRect(),x=r.x+r.width/2,y=r.y+r.height/2;return x>innerWidth*.15&&x<innerWidth*.85&&y>innerHeight*.15&&y<innerHeight*.85;});const n=list[input.index||0];if(!n)throw Error('missing input anchor '+input.selector);const r=n.getBoundingClientRect(),f=input.fraction||[.5,.5];x=r.x+r.width*f[0];y=r.y+r.height*f[1];}
    const target=document.elementFromPoint(x,y),buttons=input.buttons||0;
    if(input.action==='move' && hover!==target){const old=path(hover),next=path(target);emit(hover,'pointerout',x,y,buttons,target);emit(hover,'mouseout',x,y,buttons,target);for(const n of old)if(!next.includes(n)){emit(n,'pointerleave',x,y,buttons,target);emit(n,'mouseleave',x,y,buttons,target);}emit(target,'pointerover',x,y,buttons,hover);emit(target,'mouseover',x,y,buttons,hover);for(const n of next.slice().reverse())if(!old.includes(n)){emit(n,'pointerenter',x,y,buttons,hover);emit(n,'mouseenter',x,y,buttons,hover);}hover=target;}
    const destination=target||document.getElementById('root');
    if(input.action==='wheel')destination.dispatchEvent(new WheelEvent('wheel',{clientX:x,clientY:y,deltaY:input.delta,deltaMode:0,bubbles:true,cancelable:true,view:window}));
    if(input.action==='down'){emit(destination,'pointerdown',x,y,1);emit(destination,'mousedown',x,y,1);pressed=target;}
    if(input.action==='move'){emit(destination,'pointermove',x,y,buttons);emit(destination,'mousemove',x,y,buttons);}
    if(input.action==='up'){emit(destination,'pointerup',x,y);emit(destination,'mouseup',x,y);if(target&&target===pressed)emit(target,'click',x,y);pressed=null;}
    if(input.action==='click'){emit(destination,'pointerdown',x,y,1);emit(destination,'mousedown',x,y,1);emit(destination,'pointerup',x,y);emit(destination,'mouseup',x,y);emit(destination,'click',x,y);}
    return {x,y,target:target?.localName||null};
  };
})();
