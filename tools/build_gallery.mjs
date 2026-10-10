import {readFileSync,writeFileSync,mkdirSync,readdirSync} from 'node:fs';
import {resolve,relative,dirname,extname} from 'node:path';
import {createRequire} from 'node:module';
import {repo,runtime,manifest,sha,verifyRuntime,restoreRuntimeLinks} from './gallery_runtime.mjs';
verifyRuntime();restoreRuntimeLinks();
const require=createRequire(resolve(runtime,'package.json'));
const esbuild=require('esbuild'),tailwind=require('tailwindcss');
const fixture=resolve(repo,'tests/fixtures/crpg-gallery/UI'),out=resolve(repo,'examples/crpg-gallery/native');
const entry=resolve(repo,'examples/crpg-gallery/src/harness.tsx');
const original=resolve(fixture,'src/components/debug/TestGallery.tsx'),text=readFileSync(original,'utf8');
const inventory=JSON.parse(readFileSync(resolve(fixture,'../inventory.json'),'utf8'));
mkdirSync(out,{recursive:true});
const inputs={},outputs={};
const remember=p=>inputs[relative(repo,p).replaceAll('\\','/')]=sha(readFileSync(p));
const emit=(name,bytes)=>{writeFileSync(resolve(out,name),bytes);outputs[relative(repo,resolve(out,name)).replaceAll('\\','/')]=sha(bytes);};
remember(original);remember(entry);remember(resolve(repo,'examples/crpg-gallery/src/platform.js'));
remember(resolve(runtime,'manifest.json'));remember(resolve(repo,'tools/build_gallery.mjs'));remember(resolve(repo,'tools/gallery_runtime.mjs'));
remember(resolve(fixture,'../inventory.json'));
for(const selected of ['all',...inventory.groups.map(g=>g.name)]){
  const exports=(selected==='all'?inventory.groups.map(g=>g.name):[selected]).join(',');
  const result=await esbuild.build({entryPoints:[entry],absWorkingDir:repo,bundle:true,write:false,
    format:'iife',platform:'browser',target:'es2020',jsx:'automatic',nodePaths:[resolve(runtime,'node_modules')],
    define:{'process.env.NODE_ENV':'"production"'},metafile:true,legalComments:'inline',minify:false,
    plugins:[{name:'immutable-gallery-exports',setup(build){
      build.onResolve({filter:/^ce-gallery-original$/},()=>({path:original,namespace:'original-gallery'}));
      build.onLoad({filter:/.*/,namespace:'original-gallery'},()=>({contents:text+'\nexport const __ceGalleryGroups={'+exports+'};\nexport const __ceOriginalGallery='+ (selected==='all'?'TestGallery':'()=>null')+';\n',loader:'tsx',resolveDir:dirname(original)}));
      build.onResolve({filter:/^@\//},args=>({path:resolve(fixture,'src',args.path.slice(2)+'.ts')}));
    }}]});
  for(const name of Object.keys(result.metafile.inputs))if(!name.startsWith('original-gallery:'))remember(resolve(repo,name));
  emit(selected+'.js',result.outputFiles[0].contents);
}
// Tailwind's pinned compiler is driven by a conservative static candidate set;
// no Vite/npm install or files from the changing original game are needed.
const candidates=new Set();
function scan(dir){for(const e of readdirSync(dir,{withFileTypes:true})){
  const p=resolve(dir,e.name);if(e.isDirectory()){scan(p);continue;}
  if(!['.tsx','.ts','.css'].includes(extname(p)))continue;
  remember(p);for(const token of readFileSync(p,'utf8').split(/[\s"'`<>=;{},()]+/))if(token)candidates.add(token);
}}
scan(resolve(fixture,'src'));
const loadStylesheet=async(id,base)=>{
  const path=id==='tailwindcss'?resolve(runtime,manifest.packages[manifest.imports.tailwindcss].source_dir,'index.css'):resolve(base,id);
  remember(path);return {path,base:dirname(path),content:readFileSync(path,'utf8')};
};
const compiled=await tailwind.compile(readFileSync(resolve(fixture,'src/index.css'),'utf8'),{base:resolve(fixture,'src'),loadStylesheet});
emit('gallery.css',compiled.build([...candidates])+'\nhtml,body{margin:0;background:#0a0806;color:#f5e6c8;} #root{position:relative;width:800px;height:600px;overflow:hidden;background:#0a0806;}');
emit('platform.js',readFileSync(resolve(repo,'examples/crpg-gallery/src/platform.js')));
emit('index.html',`<!doctype html><meta charset="utf-8"><title>CaseElephant fixed gallery reference</title><link rel="stylesheet" href="gallery.css">
<body><div id="root"></div><script src="platform.js"></script><script src="all.js"></script><script>
const p=new URLSearchParams(location.search); if(p.has('original'))__ceGallery.original();else __ceGallery.mount(p.get('group')||'CSS_TESTS',+(p.get('index')||0));
</script></body>`);
emit('LICENSES.txt',Object.values(manifest.packages).map(p=>p.name+' '+p.version+' ('+p.license+')\n'+p.licenses.map(n=>readFileSync(resolve(runtime,n),'utf8')).join('\n')).join('\n\n'));
emit('cases.json',JSON.stringify(inventory,null,2)+'\n');
emit('case-list.js','globalThis.__ceGalleryCases='+JSON.stringify(inventory.groups.flatMap(g=>g.cases.map(c=>({id:c.id,group:g.name,index:c.index,label:c.label}))))+';\n');
writeFileSync(resolve(out,'metadata.json'),JSON.stringify({format:'gallery-offline-bundle-v1',node:process.version,
  esbuild:esbuild.version,react:manifest.packages.react.version,three:manifest.packages.three.version,
  options:{target:'es2020',format:'iife',production:true,original_case_bodies:'unchanged',added_registry_exports:true,css:'pinned Tailwind compiler with static token candidates; omitted font assets'},
  provenance:manifest.provenance,inputs,outputs},null,2)+'\n');
console.log(`Built original ${inventory.case_count}-case registry / full gallery + ${inventory.groups.length} isolated groups offline; no original-game writes.`);
