"""Compare native geometry with independently captured browser DOM rectangles."""
import argparse
import json
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('reference',type=Path)
p.add_argument('native',type=Path)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
ref=json.loads(a.reference.read_text(encoding='utf-8'))
native=json.loads(a.native.read_text(encoding='utf-8'))
assert ref['fixture']==native['fixture']=='casecheetah-nine-slice-v1'
assert len(ref['frames'])==len(native['frames'])==3
errors=[]
for frame,(b,n) in enumerate(zip(ref['frames'],native['frames'])):
    assert b['state']==n['state'], (frame,b['state'],n['state'])
    assert len(b['boxes'])==len(n['layout']['boxes'])==44
    for expected,actual in zip(b['boxes'],n['layout']['boxes']):
        assert expected['id']==actual['id'], (expected['id'],actual['id'])
        for key in ('x','y','width','height'):
            errors.append((abs(expected[key]-actual[key]),frame,actual['id'],key))
worst=max(errors)
# Chromium quantizes the 26/30/42px slice boxes at fractional DPR; the
# 1100% image heights magnify those intermediate differences. Compare the
# ideal CSS lengths with a declared 0.2 CSS-pixel tolerance, not pixel parity.
tolerance=0.2
result={'fixture':native['fixture'],'fields':len(errors),'max_delta_css_px':worst[0],
        'tolerance_css_px':tolerance,'worst':worst[1:],'geometry_verified':worst[0]<=tolerance,
        'browser_pixel_parity_verified':False,'passed':worst[0]<=tolerance}
a.output.parent.mkdir(parents=True,exist_ok=True)
a.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
assert result['passed'],result
print(f"PASS original NineSlice browser geometry: {len(errors)} fields; max {worst[0]:.9f} CSS px")
