"""Compare current native scene rectangles to the real controlled browser."""
import json, sys
from pathlib import Path
native=Path(sys.argv[1]); reference=Path(sys.argv[2]); checks=0; worst=0.; rows=[]
for group,count in (('css',26),('d3',8)):
    for index in range(count):
        for frame in (24,63,120):
            name=f'{group}-{index:02d}-{frame}-geometry.json'
            a=json.loads((native/name).read_text(encoding='utf-8'))
            b=json.loads((reference/name).read_text(encoding='utf-8'))
            if len(a)!=len(b): raise ValueError(f'{name}: element count {len(a)} != {len(b)}')
            error=max(abs(x-y) for r,s in zip(a,b) for x,y in zip(r,s['rect']))
            if error>.3: raise ValueError(f'{name}: geometry error {error} > .3 CSS px')
            checks+=len(a)*4;worst=max(worst,error);rows.append({'capture':name,'elements':len(a),'maximum_css_px':error})
report={'format':'scene-geometry-browser-diff-v1','captures':102,'fields':checks,'maximum_css_px':worst,'tolerance_css_px':.3,'passed':True,'captures_detail':rows}
(native/'geometry-diff.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print(f'PASS 102 real browser geometry captures: {checks} fields, maximum {worst:g} CSS px')
