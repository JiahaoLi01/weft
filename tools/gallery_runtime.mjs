// Offline runtime resolution. Real source bytes are retained outside ignored
// node_modules; junctions are derived from the pinned dependency graph.
import {readFileSync,existsSync,mkdirSync,symlinkSync,realpathSync} from 'node:fs';
import {resolve,dirname} from 'node:path';
import {createHash} from 'node:crypto';
export const repo=resolve(import.meta.dirname,'..');
export const runtime=resolve(repo,'third_party/gallery-runtime');
export const manifest=JSON.parse(readFileSync(resolve(runtime,'manifest.json'),'utf8'));
export const sha=data=>createHash('sha256').update(data).digest('hex');
export function verifyRuntime(){
  for(const [name,digest] of Object.entries(manifest.files))
    if(sha(readFileSync(resolve(runtime,name)))!==digest)throw new Error('Runtime drift: '+name);
  for(const record of Object.values(manifest.packages)){
    const pkg=JSON.parse(readFileSync(resolve(runtime,record.source_dir,'package.json'),'utf8'));
    if(pkg.name!==record.name || pkg.version!==record.version || !record.licenses.length)
      throw new Error('Runtime identity/license: '+record.name);
  }
}
export function restoreRuntimeLinks(){
  const link=(target,path)=>{
    if(existsSync(path)){
      if(realpathSync(path)!==realpathSync(target))throw new Error('Foreign runtime link: '+path);
      return;
    }
    mkdirSync(dirname(path),{recursive:true});
    symlinkSync(target,path,process.platform==='win32'?'junction':'dir');
  };
  for(const p of Object.values(manifest.packages))for(const [name,key] of Object.entries(p.dependencies))
    link(resolve(runtime,manifest.packages[key].source_dir),resolve(runtime,p.source_dir,'node_modules',name));
  // Preserve original hoisted package visibility, including D3's peer imports.
  const hoisted=Object.fromEntries(Object.entries(manifest.packages).filter(([key,p])=>key===p.name).map(([key,p])=>[p.name,key]));
  for(const [name,key] of Object.entries({...hoisted,...manifest.imports}))
    link(resolve(runtime,manifest.packages[key].source_dir),resolve(runtime,'node_modules',name));
}
