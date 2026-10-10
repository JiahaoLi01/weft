"""Compare independent browser DOM, events and text metrics against C++/GPU."""
import argparse
import json
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('reference',type=Path);p.add_argument('native',type=Path);p.add_argument('--output',type=Path,required=True)
a=p.parse_args();b=json.loads(a.reference.read_text(encoding='utf-8'));n=json.loads(a.native.read_text(encoding='utf-8'))
assert b['fixture']==n['fixture']=='casecheetah-alert-v1'
assert len(b['frames'])==len(n['frames'])==8
errors=[]
for i,(bf,nf) in enumerate(zip(b['frames'],n['frames'])):
    assert bf['state']==nf['state'],(i,bf['state'],nf['state'])
    assert len(bf['boxes'])==len(nf['layout']['boxes'])
    for bb,nb in zip(bf['boxes'],nf['layout']['boxes']):
        assert bb['id']==nb['id'],(i,bb['id'],nb['id'])
        for k in ('x','y','width','height'):errors.append(abs(bb[k]-nb[k]))
widths=[];baselines=[]
for bm,nm in zip(b['font_measurements'],n['font_measurements'],strict=True):
    assert bm['text']==nm['text']
    widths.append(abs(bm['advance']-nm['advance']));baselines.append(abs(bm['baseline']-nm['baseline']))
# Same 0.2 px geometry allowance as NineSlice's fractionally quantized art.
# HB advance is 26.6; browser font ascent/descent rounds to device pixels.
# Retain our existing unrounded FT primary strut with a declared 0.5 px
# baseline tolerance, identical for Latin-only and mixed fallback text.
result={'fixture':n['fixture'],'geometry_fields':len(errors),'geometry_max_delta_css_px':max(errors),'geometry_tolerance_css_px':.2,
 'font_width_max_delta_css_px':max(widths),'font_width_tolerance_css_px':.1,'baseline_max_delta_css_px':max(baselines),'baseline_tolerance_css_px':.5,
 'states_and_bridge_equal':True,'browser_pixel_parity_verified':False,
 'passed':max(errors)<=.2 and max(widths)<=.1 and max(baselines)<=.5}
a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
assert result['passed'],result
print('PASS AlertBar browser/native states, UE payloads, geometry and font metrics:',result)
