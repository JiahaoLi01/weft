import React from 'react';
import {createRoot} from 'react-dom/client';
import {__ceGalleryGroups, __ceOriginalGallery} from 'ce-gallery-original';

// The build only appends exports to the captured original module. All original
// case/component function bodies and imported library sources stay unchanged.
const groups=__ceGalleryGroups as Record<string,Array<{label:string,render:()=>React.ReactNode}>>;
let root:ReturnType<typeof createRoot>|undefined;
let errors:string[]=[];
let active:{group:string,index:number,label:string}|null=null;
function OriginalCase({group,index}:{group:string,index:number}){
  return <>{groups[group][index].render()}</>;
}
function record(error:unknown){
  if(errors.length<64)errors.push((String(error)+(error instanceof Error && error.stack ? '\n'+error.stack : '')).slice(0,8192));
}
function mount(group:string,index:number){
  if(!groups[group]?.[index])throw new Error('CE_GALLERY_CASE: unknown registration');
  unmount();errors=[];
  active={group,index,label:groups[group][index].label};
  root=createRoot(document.getElementById('root')!,{onCaughtError:record,onUncaughtError:record,onRecoverableError:record});
  root.render(<OriginalCase group={group} index={index}/>);
}
function unmount(){root?.unmount();root=undefined;active=null;}
(globalThis as any).__ceGallery=Object.freeze({
  groups:()=>Object.fromEntries(Object.entries(groups).map(([name,cases])=>[name,cases.map(c=>c.label)])),
  mount,unmount,
  original(){unmount();errors=[];root=createRoot(document.getElementById('root')!,{onUncaughtError:record});root.render(<__ceOriginalGallery/>);},
  state:()=>({active,errors:errors.slice(),bridge:(globalThis as any).__ceGalleryBridge.slice()})
});
