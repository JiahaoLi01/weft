"""Independent saved Chromium geometry, including split inline rects and scroll."""
import argparse,json
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('reference',type=Path);p.add_argument('native',type=Path);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
b=json.loads(a.reference.read_text(encoding='utf-8'));n=json.loads(a.native.read_text(encoding='utf-8'))
assert b['fixture']==n['fixture']=='flow-layout-v1';assert all(all(f) for f in b['fonts_loaded']);assert len(b['frames'])==len(n['frames'])==3
fields=[];fragments=[];worst=None
def compare(bv,nv,where,group):
 global worst
 d=abs(bv-nv);group.append(d)
 if worst is None or d>worst[0]:worst=[d,*where,bv,nv]
for frame,(bf,nf) in enumerate(zip(b['frames'],n['frames'],strict=True)):
 assert len(bf)==len(nf)==9
 for scene,(bs,ns) in enumerate(zip(bf,nf,strict=True)):
  for bb,nb in zip(bs,ns,strict=True):
   assert bb['id']==nb['id']
   for k in ('x','y','width','height','clientWidth','clientHeight','scrollWidth','scrollHeight','scrollLeft','scrollTop'):
    compare(bb[k],nb[k],[frame,scene,bb['id'],k],fields)
   for j,(br,nr) in enumerate(zip(bb['rects'],nb['rects'],strict=True)):
    for k,(bv,nv) in enumerate(zip(br,nr,strict=True)):compare(bv,nv,[frame,scene,bb['id'],'rects',j,k],fragments)
# The new profile has rounded OpenType Win inline metrics at logical DPR 1;
# Chromium measured at 1.005 also quantizes scroll offsets and font advances.
# No allowance for different line/fragment counts or dropped/inserted elements.
r={'fixture':b['fixture'],'scenes':27,'geometry_fields':len(fields),'fragment_fields':len(fragments),'max_geometry_delta_css_px':max(fields),'max_fragment_delta_css_px':max(fragments),'tolerance_css_px':1,'worst':worst,'passed':max(fields+fragments)<=1}
a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(r,indent=2)+'\n',encoding='utf-8');assert r['passed'],r;print('PASS flow Chromium differential:',r)
