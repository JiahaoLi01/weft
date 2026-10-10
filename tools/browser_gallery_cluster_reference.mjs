// Real Chromium reference via CDP. Offline files only; remote assets are
// reported as blocked resources, not renderer failures or successful cases.
import {createServer} from 'node:http';
import {spawn} from 'node:child_process';
import {readFileSync,writeFileSync,mkdirSync,existsSync} from 'node:fs';
import {resolve,extname} from 'node:path';
import {repo,sha} from './gallery_runtime.mjs';
const directory=resolve(repo,'examples/crpg-gallery/native');
const specification=JSON.parse(readFileSync(resolve(repo,'examples/crpg-gallery/cluster-cases.json'),'utf8'));
let activeGroup='D3_INTERACTIVE_TESTS';
const out=resolve(process.argv[2] ?? resolve(repo,'artifacts/local/gallery-cluster/reference'));
mkdirSync(out,{recursive:true});
const executable=process.argv[3] ?? 'C:/Program Files/Google/Chrome/Application/chrome.exe';
if(!existsSync(executable))throw new Error('Provide a local Chromium executable as the third argument');
const mime={'.html':'text/html','.js':'application/javascript','.css':'text/css','.json':'application/json'};
const server=createServer((request,response)=>{
  const name=new URL(request.url,'http://localhost').pathname.slice(1)||'scene-css.html';
  if(!/^[A-Za-z0-9_.-]+$/.test(name)){response.writeHead(404);response.end();return;}
  try{const data=name.startsWith('scene-')&&name.endsWith('.html')?Buffer.from('<!doctype html><html><head><link rel=stylesheet href=gallery.css></head><body style="margin:0;background:#0a0806"><div id=root style="position:relative;overflow:hidden;background:#050403;color:#f5e6c8;font-family:system-ui;font-size:16px;line-height:1.5;"></div><script src=platform.js></script><script src='+activeGroup+'.js></script></body></html>'):readFileSync(resolve(directory,name));response.writeHead(200,{'Content-Type':mime[extname(name)]||'text/plain'});response.end(data);}
  catch{response.writeHead(404);response.end();}
});
await new Promise(r=>server.listen(0,'127.0.0.1',r));
const origin='http://127.0.0.1:'+server.address().port;
const child=spawn(executable,['--headless=new','--no-first-run','--disable-background-networking','--remote-debugging-port=0',
  '--force-device-scale-factor=1','--window-size=800,600','--hide-scrollbars',
  '--user-data-dir='+resolve(out,'browser-profile'),'about:blank'],{windowsHide:true,stdio:['ignore','ignore','pipe']});
let socket;
try{
  const endpoint=await new Promise((resolve,reject)=>{
    let text='';const limit=setTimeout(()=>reject(new Error('Chromium startup timeout')),20000);
    child.on('error',reject);child.stderr.on('data',chunk=>{text+=chunk;const m=text.match(/DevTools listening on ws:\/\/127\.0\.0\.1:(\d+)\//);if(m){clearTimeout(limit);resolve('http://127.0.0.1:'+m[1]);}});
  });
  const tabs=await (await fetch(endpoint+'/json/list')).json();
  const tab=tabs.find(t=>t.type==='page');
  socket=new WebSocket(tab.webSocketDebuggerUrl);
  await new Promise((r,j)=>{socket.addEventListener('open',r,{once:true});socket.addEventListener('error',j,{once:true});});
  let next=0;const requests=new Map();let errors=[],blocked=[];
  const call=(method,params={})=>new Promise((resolve,reject)=>{
    const id=++next,timer=setTimeout(()=>{requests.delete(id);reject(new Error('CDP timeout: '+method));},20000);
    requests.set(id,{resolve,reject,timer});socket.send(JSON.stringify({id,method,params}));
  });
  socket.addEventListener('message',event=>{
    const message=JSON.parse(event.data);
    if(message.id){const q=requests.get(message.id);if(q){requests.delete(message.id);clearTimeout(q.timer);message.error?q.reject(new Error(JSON.stringify(message.error))):q.resolve(message.result);}return;}
    if(message.method==='Runtime.exceptionThrown')errors.push(message.params.exceptionDetails.exception?.description ?? message.params.exceptionDetails.text);
    if(message.method==='Runtime.consoleAPICalled' && message.params.type==='error')errors.push(message.params.args.map(a=>a.value??a.description).join(' '));
    if(message.method==='Fetch.requestPaused'){
      const p=message.params;
      if(new URL(p.request.url).origin===origin)call('Fetch.continueRequest',{requestId:p.requestId}).catch(()=>{});
      else{blocked.push(p.request.url);call('Fetch.failRequest',{requestId:p.requestId,errorReason:'BlockedByClient'}).catch(()=>{});}
    }
  });
  await call('Runtime.enable');await call('Page.enable');await call('DOM.enable');await call('CSS.enable');
  await call('Emulation.setDeviceMetricsOverride',{width:800,height:600,deviceScaleFactor:1,mobile:false});
  await call('Fetch.enable',{patterns:[{urlPattern:'http*',requestStage:'Request'}]});
  await call('Page.addScriptToEvaluateOnNewDocument',{source:readFileSync(resolve(repo,'probes/gallery_reference_clock.js'),'utf8')+'\n'+readFileSync(resolve(repo,'probes/cluster_reference_clock.js'),'utf8')});
  await call('Page.navigate',{url:origin+'/'});
  const evalJS=async(expression)=>{
    const r=await call('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true});
    if(r.exceptionDetails)throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
    return r.result.value;
  };
  for(let i=0;i<40;++i){if(await evalJS("typeof __ceGallery !== 'undefined'"))break;await new Promise(r=>setTimeout(r,100));}
  if(!await evalJS("typeof __ceGallery !== 'undefined'"))throw new Error('Browser bundle initialization failed: '+errors.join('; '));
  const records=[];
  for(const group of specification.groups)for(let index=0;index<group.count;index++){
    errors=[];blocked=[];
    const id=group.prefix+'-'+String(index).padStart(2,'0'),width=group.width,height=group.height;activeGroup=group.name;
    await call('Emulation.setDeviceMetricsOverride',{width:Math.ceil(width),height,deviceScaleFactor:2,mobile:false});
    await call('Page.navigate',{url:origin+'/scene-cluster.html'});
    for(let i=0;i<100;i++){if(await evalJS("typeof __ceGallery !== 'undefined'"))break;await new Promise(r=>setTimeout(r,10));}
    await evalJS(`document.getElementById('root').style.width='${width}px';document.getElementById('root').style.height='${height}px';__ceGallery.mount('${group.name}',${index})`);
    // Real React MessageChannel tasks are drained while the document clock is
    // held at zero. Component effects and animation origin are deterministic.
    for(let i=0;i<30;i++){await evalJS('__ceReference.settle()');await new Promise(r=>setTimeout(r,2));}await evalJS('__ceReference.step(0)');
    if(!await evalJS("document.getElementById('root').children.length>0"))throw Error(id+' did not mount');
    const captures=[],input_trace=[];
    for(let frame=0;frame<specification.frames;frame++){
      const time=frame*(1000/60);await evalJS(`__ceReference.step(${time})`);
      for(const input of (specification.inputs[id]||[]).filter(i=>i.frame===frame)){input_trace.push({frame,...await evalJS('__ceClusterInput('+JSON.stringify(input)+')')});await evalJS(`__ceReference.step(${time})`);await new Promise(r=>setTimeout(r,1));await evalJS(`__ceReference.step(${time})`);}
      if(specification.capture_frames.includes(frame)){
        await evalJS(`__ceReference.styles(${time})`);
        if(frame===24){const doc=await call('DOM.getDocument');const root=await call('DOM.querySelector',{nodeId:doc.root.nodeId,selector:'#root span,#root .text-lg,#root .text-sm,#root .text-xs,svg text'});const fonts=root.nodeId?await call('CSS.getPlatformFontsForNode',{nodeId:root.nodeId}):{fonts:[]};writeFileSync(resolve(out,id+'-fonts.json'),JSON.stringify(fonts,null,2));}
        const geometry=await evalJS(`(()=>{const root=document.getElementById('root');const elements=[root,...root.querySelectorAll('*')].filter(n=>!n.closest('defs'));return elements.map(n=>{const r=n.getBoundingClientRect();return {tag:n.localName,rect:[r.x,r.y,r.width,r.height],style:Object.fromEntries(['width','height','font-size','line-height','transform','opacity','background-position','border-radius','border-color','color'].map(k=>[k,getComputedStyle(n).getPropertyValue(k)]))}})})()`);
        writeFileSync(resolve(out,`${id}-${frame}-geometry.json`),JSON.stringify(geometry,null,2)+'\n');
        const screenshot=await call('Page.captureScreenshot',{format:'png',captureBeyondViewport:false,fromSurface:true});writeFileSync(resolve(out,`${id}-${frame}.png`),Buffer.from(screenshot.data,'base64'));const snapshot=await evalJS('({state:__ceGallery.state(),dom:Array.from(document.getElementById("root").querySelectorAll("*"),n=>({tag:n.localName,style:n.getAttribute("style"),text:n.children.length?"":n.textContent}))})');writeFileSync(resolve(out,`${id}-${frame}-dom.json`),JSON.stringify(snapshot,null,2));captures.push({frame,time,geometry,state:snapshot.state});
      }
    }
    const state=await evalJS('__ceGallery.state()');await evalJS('__ceGallery.unmount()');await evalJS('__ceReference.step(5001)');
    if(state.errors.length||errors.length||blocked.length)throw Error(id+JSON.stringify({state,errors,blocked}));
    records.push({id,width,height,captures,input_trace,state,errors,blocked});console.log(id+' reference OK');
  }
  const version=await call('Browser.getVersion');writeFileSync(resolve(out,'report.json'),JSON.stringify({format:'gallery-shared-cluster-browser-v1',version,seed:0xce202610,dpr:2,frames_per_case:specification.frames,conditions:{css:'original pinned Tailwind gallery.css; original unchanged React/D3/Framer/Spring/Lucide/Anime bundles',scheduler:'controlled timer/rAF/performance/Date clock; original library JS fallback, real browser CSS animations/transitions paused and sampled via Web Animations'},cases:records},null,2)+'\n');
  console.log('PASS 30 original browser references / '+records.reduce((n,c)=>n+c.captures.length,0)+' captures / '+version.product);await call('Browser.close').catch(()=>{});
}finally{socket?.close();child.kill();await new Promise(r=>server.close(r));}
