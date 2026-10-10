// Real Chromium reference via CDP. Offline files only; remote assets are
// reported as blocked resources, not renderer failures or successful cases.
import {createServer} from 'node:http';
import {spawn} from 'node:child_process';
import {readFileSync,writeFileSync,mkdirSync,existsSync} from 'node:fs';
import {resolve,extname} from 'node:path';
import {repo,sha} from './gallery_runtime.mjs';
const directory=resolve(repo,'examples/crpg-gallery/native');
const out=resolve(process.argv[2] ?? resolve(repo,'artifacts/local/gallery-canvas/reference'));
mkdirSync(out,{recursive:true});
const executable=process.argv[3] ?? 'C:/Program Files/Google/Chrome/Application/chrome.exe';
if(!existsSync(executable))throw new Error('Provide a local Chromium executable as the third argument');
const mime={'.html':'text/html','.js':'application/javascript','.css':'text/css','.json':'application/json'};
const server=createServer((request,response)=>{
  const name=new URL(request.url,'http://localhost').pathname.slice(1)||'canvas.html';
  if(!/^[A-Za-z0-9_.-]+$/.test(name)){response.writeHead(404);response.end();return;}
  try{const data=name==='canvas.html'?Buffer.from('<!doctype html><html><head><link rel=stylesheet href=canvas-profile.css></head><body style="margin:0"><div id=root></div><script src=platform.js></script><script src=WEBGL_TESTS.js></script></body></html>'):readFileSync(name==='canvas-profile.css'?resolve(repo,'examples/crpg-gallery/canvas-profile.css'):resolve(directory,name));response.writeHead(200,{'Content-Type':mime[extname(name)]||'text/plain'});response.end(data);}
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
  await call('Runtime.enable');await call('Page.enable');
  await call('Emulation.setDeviceMetricsOverride',{width:800,height:600,deviceScaleFactor:1,mobile:false});
  await call('Fetch.enable',{patterns:[{urlPattern:'http*',requestStage:'Request'}]});
  await call('Page.navigate',{url:origin+'/'});
  const evalJS=async(expression)=>{
    const r=await call('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true});
    if(r.exceptionDetails)throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
    return r.result.value;
  };
  for(let i=0;i<40;++i){if(await evalJS("typeof __ceGallery !== 'undefined'"))break;await new Promise(r=>setTimeout(r,100));}
  if(!await evalJS("typeof __ceGallery !== 'undefined'"))throw new Error('Browser bundle initialization failed: '+errors.join('; '));
  const records=[];
  for(let index=0;index<2;++index){
    errors=[];blocked=[];
    await evalJS(`(()=>{let seed=0xce202610;Math.random=()=>((seed=(Math.imul(seed,1664525)+1013904223)>>>0)/4294967296);
      let next=0;const queue=new Map();
      window.requestAnimationFrame=callback=>{queue.set(++next,callback);return next};
      window.cancelAnimationFrame=id=>queue.delete(id);
      window.__canvasStep=time=>{const batch=Array.from(queue);for(const [id,callback] of batch){if(!queue.delete(id))continue;callback(time)}return queue.size};
      window.__canvasPending=()=>queue.size;
      document.getElementById('root').style.width='800px';document.getElementById('root').style.height='600px';
    })()`);
    await evalJS(`__ceGallery.mount('WEBGL_TESTS',${index})`);
    for(let i=0;i<100;++i){if(await evalJS("!!document.querySelector('canvas') && __canvasPending()===1"))break;await new Promise(r=>setTimeout(r,10));}
    if(!await evalJS("!!document.querySelector('canvas') && __canvasPending()===1"))throw new Error('Original Canvas mount failed');
    const captures=[];
    for(let frame=0;frame<64;++frame){
      if(frame===32 || frame===48)await evalJS(`document.getElementById('root').style.width='${frame===32?400:800}px';document.getElementById('root').style.height='${frame===32?300:600}px'`);
      if(frame===2 || frame===34)await evalJS(`document.querySelector('canvas').dispatchEvent(new MouseEvent('${index?'click':'mousemove'}',{clientX:${frame===2?260:130},clientY:${frame===2?180:90},bubbles:true,cancelable:true}))`);
      if(await evalJS(`__canvasStep(${frame*16+.25})`)!==1)throw new Error('Original Canvas did not reschedule');
      if([11,39,63].includes(frame)){
        const name=`webgl-0${index}-${frame}`;
        const bitmap=await evalJS(`(()=>{const c=document.querySelector('canvas'),data=c.getContext('2d').getImageData(0,0,c.width,c.height).data;
          let binary='';for(let start=0;start<data.length;start+=8192)binary+=String.fromCharCode(...data.subarray(start,start+8192));
          const r=c.getBoundingClientRect();return {base64:btoa(binary),png:c.toDataURL('image/png').split(',')[1],width:c.width,height:c.height,rect:[r.x,r.y,r.width,r.height]};})()`);
        writeFileSync(resolve(out,name+'.rgba'),Buffer.from(bitmap.base64,'base64'));delete bitmap.base64;writeFileSync(resolve(out,name+'-bitmap.png'),Buffer.from(bitmap.png,'base64'));delete bitmap.png;
        const png=await call('Page.captureScreenshot',{format:'png',captureBeyondViewport:false});writeFileSync(resolve(out,name+'.png'),Buffer.from(png.data,'base64'));
        captures.push({frame,...bitmap});
      }
    }
    const state=await evalJS('__ceGallery.state()');await evalJS('__ceGallery.unmount()');
    if(await evalJS('__canvasPending()')!==0 || state.errors.length || errors.length || blocked.length)throw new Error('Canvas reference errors/unmount: '+JSON.stringify({state,errors,blocked}));
    records.push({id:`webgl-0${index}`,captures,state,errors,blocked});
  }
  const version=await call('Browser.getVersion');
  writeFileSync(resolve(out,'report.json'),JSON.stringify({format:'gallery-canvas-browser-v1',version,seed:0xce202610,frames_per_case:64,
    conditions:{viewport:[800,600],backing:[800,600],dpr:1,css:'explicit positioned Canvas adapter',input_events:2,
      scheduler:'deterministic host rAF queue; real Canvas2D and unmodified original component bodies'},cases:records},null,2)+'\n');
  console.log(`PASS 2 original Canvas references: 64 deterministic frames/input/resize/unmount; ${version.product}`);
  await call('Browser.close').catch(()=>{});
}finally{socket?.close();child.kill();await new Promise(r=>server.close(r));}
