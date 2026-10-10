"""Compare freshly executed native values against recorded browser observations."""
import argparse
import json
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('reference',type=Path);p.add_argument('native',type=Path);p.add_argument('--output',type=Path,required=True)
a=p.parse_args();b=json.loads(a.reference.read_text(encoding='utf-8'));n=json.loads(a.native.read_text(encoding='utf-8'))
assert b['fixture']==n['fixture'];assert len(b['frames'])==len(n['frames'])
colors=[];geometry=[];scroll=[]
for frame,(bf,nf) in enumerate(zip(b['frames'],n['frames'],strict=True)):
    if b['fixture'] in ('casecheetah-style-v1','casecheetah-auto-v1','casecheetah-flow-v1'):
        assert bf['fonts_loaded'];assert bf['state']==nf['state'],frame
        assert len(bf['boxes'])==len(nf['layout']['boxes'])
        for bb,nb in zip(bf['boxes'],nf['layout']['boxes'],strict=True):
            assert bb['id']==nb['id']
            geometry.extend(abs(bb[k]-nb[k]) for k in ('x','y','width','height'))
        bc,nc=bf['style_colors'],nf['style_colors'];keys=('color','background-color')
        if b['fixture']=='casecheetah-flow-v1':scroll.append(abs(bf['scroll_top']-nf['scroll_top']))
    else:bc,nc=bf,nf;keys=('color',)
    for bb,nb in zip(bc,nc,strict=True):
        assert bb.get('id',bb.get('tag'))==nb.get('id',nb.get('tag'))
        for key in keys:colors.extend(abs(x-y) for x,y in zip(bb[key],nb[key],strict=True))
tolerance=1 if b['fixture']=='casecheetah-flow-v1' else .3 if b['fixture']=='casecheetah-auto-v1' else .2
result={'fixture':b['fixture'],'color_fields':len(colors),'color_max_delta':max(colors),'color_tolerance':.00001,
        'geometry_fields':len(geometry),'geometry_max_delta_css_px':max(geometry,default=0),'geometry_tolerance_css_px':tolerance,
        'scroll_fields':len(scroll),'scroll_max_delta_css_px':max(scroll,default=0),'scroll_tolerance_css_px':.5,
        'passed':max(colors)<=.00001 and max(geometry,default=0)<=tolerance and max(scroll,default=0)<=.5,'browser_pixel_parity_verified':False}
a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');assert result['passed'],result
print('PASS browser/native shared style:',result)
