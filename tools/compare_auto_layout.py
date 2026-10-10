import argparse,json
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('reference',type=Path);p.add_argument('native',type=Path);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
b=json.loads(a.reference.read_text(encoding='utf-8'));n=json.loads(a.native.read_text(encoding='utf-8'));assert b['fixture']==n['fixture']=='auto-layout-v1'
assert len(b['frames'])==len(n['frames'])==3
errors=[];worst=None
for i,(bf,nf) in enumerate(zip(b['frames'],n['frames'],strict=True)):
 for j,(bs,ns) in enumerate(zip(bf,nf,strict=True)):
  for bb,nb in zip(bs,ns,strict=True):
   assert bb['id']==nb['id']
   for k in ('x','y','width','height'):
    d=abs(bb[k]-nb[k]);errors.append(d)
    if worst is None or d>worst[0]:worst=(d,i,j,bb['id'],k,bb[k],nb[k])
r={'fixture':b['fixture'],'scenes':48,'geometry_fields':len(errors),'max_delta_css_px':max(errors),'tolerance_css_px':.3,'worst':worst,'passed':max(errors)<=.3}
a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(r,indent=2)+'\n',encoding='utf-8');assert r['passed'],r;print('PASS automatic layout differential:',r)
