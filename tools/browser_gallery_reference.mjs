// Real Chromium reference via CDP. Offline files only; remote assets are
// reported as blocked resources, not renderer failures or successful cases.
import {createServer} from 'node:http';
import {spawn} from 'node:child_process';
import {readFileSync,writeFileSync,mkdirSync,existsSync} from 'node:fs';
import {resolve,extname} from 'node:path';
import {repo,sha} from './gallery_runtime.mjs';
const directory=resolve(repo,'examples/crpg-gallery/native');
const out=resolve(process.argv[2] ?? resolve(repo,'artifacts/local/gallery-foundation/browser'));
mkdirSync(out,{recursive:true});
const executable=process.argv[3] ?? 'C:/Program Files/Google/Chrome/Application/chrome.exe';
if(!existsSync(executable))throw new Error('Provide a local Chromium executable as the third argument');
const mime={'.html':'text/html','.js':'application/javascript','.css':'text/css','.json':'application/json'};
const server=createServer((request,response)=>{
  const name=new URL(request.url,'http://localhost').pathname.slice(1)||'index.html';
  if(!/^[A-Za-z0-9_.-]+$/.test(name)){response.writeHead(404);response.end();return;}
  try{const data=readFileSync(resolve(directory,name));response.writeHead(200,{'Content-Type':mime[extname(name)]||'text/plain'});response.end(data);}
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
  const inventory=JSON.parse(readFileSync(resolve(directory,'cases.json'),'utf8')),records=[];
  for(const group of inventory.groups)for(const c of group.cases){
    errors=[];blocked=[];
    await evalJS(`__ceGallery.mount(${JSON.stringify(group.name)},${c.index})`);
    // A short observation window, not an animation/interaction acceptance test.
    await evalJS('new Promise(r=>setTimeout(r,250))');
    const observation=await evalJS(`(()=>{
      const root=document.getElementById('root'),nodes=root.querySelectorAll('*');
      return {state:__ceGallery.state(),elements:nodes.length,svg:root.querySelectorAll('svg').length,
        canvas:Array.from(root.querySelectorAll('canvas'),canvas=>{
          const gl=canvas.getContext('webgl2');let context=gl?'webgl2':null,nonzero=0,renderer=null;
          if(gl){const e=gl.getExtension('WEBGL_debug_renderer_info');renderer=e?gl.getParameter(e.UNMASKED_RENDERER_WEBGL):gl.getParameter(gl.RENDERER);}
          else{const ctx=canvas.getContext('2d');context=ctx?'2d':null;if(ctx){const bytes=ctx.getImageData(0,0,canvas.width,canvas.height).data;for(let i=0;i<bytes.length;i+=4)if(bytes[i]||bytes[i+1]||bytes[i+2])++nonzero;}}
          return {width:canvas.width,height:canvas.height,context,nonzero_rgb_pixels:gl?null:nonzero,renderer};
        })};})()`);
    records.push({id:c.id,label:c.label,observation,errors:[...errors],blocked_resources:[...new Set(blocked)],
      pixels_verified:false,interaction_verified:false,performance_verified:false});
    if(['css-00','webgl-00','webgl-02'].includes(c.id)){
      const png=await call('Page.captureScreenshot',{format:'png',captureBeyondViewport:false});
      writeFileSync(resolve(out,c.id+'.png'),Buffer.from(png.data,'base64'));
    }
  }
  const version=await call('Browser.getVersion');
  const report={format:'gallery-browser-observation-v1',version,case_count:records.length,
    conditions:{viewport:[800,600],dpr:1,observation_ms:250,external_network:'blocked',css:'fixed Tailwind build; original font assets omitted',input:false},
    note:'Browser reference observations only. Does not prove native rendering or original-case visual/interaction correctness.',
    bundle_metadata_sha256:sha(readFileSync(resolve(directory,'metadata.json'))),cases:records};
  writeFileSync(resolve(out,'browser-observations.json'),JSON.stringify(report,null,2)+'\n');
  console.log(`Observed ${records.length} original cases in ${version.product}; external requests blocked. Reference only.`);
  await call('Browser.close').catch(()=>{});
}finally{socket?.close();child.kill();await new Promise(r=>server.close(r));}
