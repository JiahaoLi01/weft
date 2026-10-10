// Freeze installed, lock-version-checked packages. No install scripts/network.
import {readFileSync,writeFileSync,mkdirSync,readdirSync,existsSync,symlinkSync} from 'node:fs';
import {resolve,dirname,relative,join} from 'node:path';
import {createHash} from 'node:crypto';
const original=resolve(process.argv[2] ?? ''),repo=resolve(import.meta.dirname,'..');
if(!process.argv[2])throw new Error('usage: node tools/capture_gallery_runtime.mjs <original UI/node_modules>');
const root=resolve(repo,'third_party/gallery-runtime'),out=resolve(root,'source');
if(existsSync(resolve(root,'manifest.json')))throw new Error('Runtime is immutable; use another capture for a later revision');
const lock=JSON.parse(readFileSync(resolve(repo,'tests/fixtures/crpg-gallery/UI/package-lock.json'),'utf8')).packages;
const roots=['react','react-dom','d3','three','framer-motion','@react-spring/web','animejs','lottie-web','lucide-react','typescript','tailwindcss','esbuild'];
const records={},files={},payloads=new Map();let bytes=0;
const hash=data=>createHash('sha256').update(data).digest('hex');
function locate(name,from){
  for(let cursor=from;;cursor=dirname(cursor)){
    const p=resolve(cursor,'node_modules',name);if(existsSync(resolve(p,'package.json')))return p;
    if(cursor===dirname(cursor))throw new Error('Dependency not installed: '+name);
  }
}
function visit(path){
  const key=relative(original,path).replaceAll('\\','/');if(key.startsWith('..'))throw new Error('Package outside original node_modules: '+path);
  if(records[key])return key;
  const pkg=JSON.parse(readFileSync(resolve(path,'package.json'),'utf8')),pinned=lock['node_modules/'+key];
  if(!pinned || pkg.version!==pinned.version)throw new Error('Installed version differs from captured lock: '+key);
  const folder=pkg.name.replace(/[^\w.-]/g,'_')+'-'+pkg.version+'-'+hash(key).slice(0,8);
  const record=records[key]={name:pkg.name,version:pkg.version,source_dir:'source/'+folder,resolved:pinned.resolved,lock_integrity:pinned.integrity,license:pkg.license,dependencies:{},licenses:[]};
  function walk(dir){for(const entry of readdirSync(dir,{withFileTypes:true})){
    if(entry.name==='node_modules' || entry.isSymbolicLink() || entry.name.endsWith('.map'))continue;
    const p=join(dir,entry.name);if(entry.isDirectory()){walk(p);continue;}if(!entry.isFile())continue;
    const data=readFileSync(p);if(data.length>32*1024*1024 || (bytes+=data.length)>160*1024*1024)throw new Error('Runtime byte budget exceeded');
    const name=relative(path,p).replaceAll('\\','/'),target=record.source_dir+'/'+name;
    payloads.set(target,{original:p,data});files[target]=hash(data);
    if(/^(licen[cs]e|copying|notice)(\.|$)/i.test(entry.name))record.licenses.push(target);
  }}walk(path);
  for(const [name] of Object.entries(pkg.dependencies ?? {}))record.dependencies[name]=visit(locate(name,path));
  for(const [name] of Object.entries(pkg.optionalDependencies ?? {})){let dep;try{dep=locate(name,path)}catch{continue;}record.dependencies[name]=visit(dep);}
  return key;
}
const imports=Object.fromEntries(roots.map(name=>[name,visit(resolve(original,name))]));
// The platform-specific esbuild binary package declares MIT in package.json;
// its license text is distributed with the matching esbuild wrapper package.
for(const p of Object.values(records))if(!p.licenses.length && p.name.startsWith('@esbuild/') && p.license==='MIT'){
  const wrapper=records[imports.esbuild];
  if(wrapper.version!==p.version || !wrapper.licenses.length)throw new Error('Missing esbuild binary license');
  p.licenses=[...wrapper.licenses];p.license_supplied_by=imports.esbuild;
}
for(const [name,{original:p,data}] of payloads)if(!readFileSync(p).equals(data))throw new Error('Installed source changed during capture: '+name);
for(const [name,{data}] of payloads){const p=resolve(root,name);mkdirSync(dirname(p),{recursive:true});writeFileSync(p,data);}
function link(target,path){if(existsSync(path))return;mkdirSync(dirname(path),{recursive:true});symlinkSync(target,path,process.platform==='win32'?'junction':'dir');}
for(const record of Object.values(records))for(const [name,key] of Object.entries(record.dependencies))link(resolve(root,records[key].source_dir),resolve(root,record.source_dir,'node_modules',name));
for(const [key,p] of Object.entries(records))if(key===p.name)link(resolve(root,p.source_dir),resolve(root,'node_modules',p.name));
for(const [name,key] of Object.entries(imports))link(resolve(root,records[key].source_dir),resolve(root,'node_modules',name));
const manifest={format:'gallery-installed-runtime-v1',original_node_modules:original,node:process.version,platform:process.platform,packages:records,imports,files,
  provenance:'Installed package versions checked against the immutable npm lock; all retained bytes reread and SHA256 pinned. npm tarball integrity is recorded, not independently verified.',
  omitted:['source maps','existing node_modules directories','symlinks'],normal_build:'Consumes frozen bundles; no network, install scripts or original game access.'};
writeFileSync(resolve(root,'manifest.json'),JSON.stringify(manifest,null,2)+'\n');
console.log(`Frozen ${Object.keys(records).length} packages / ${payloads.size} files / ${bytes} bytes; source and licenses retained.`);
