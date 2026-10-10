"""Offline integrity plus independent browser/native state and geometry audit."""
import hashlib, json, math, re, struct, sys
from pathlib import Path
root=Path(__file__).resolve().parents[1]
fixture=root/'tests/fixtures/gallery-cluster-reference'
load=lambda p:json.loads(p.read_text(encoding='utf-8'))
manifest=load(fixture/'manifest.json'); checked=0
for category,base in (('reference_files',fixture),('source_files',root)):
    for name,digest in manifest[category].items():
        path=(base/name).resolve()
        if not path.is_relative_to(base):raise ValueError('escaped fixture path')
        if hashlib.sha256(path.read_bytes()).hexdigest()!=digest:raise ValueError('cluster fixture drift: '+name)
        checked+=1
style=load(root/'examples/crpg-gallery/cluster-style-manifest.json')
for category in ('input_sha256','output_sha256'):
    for name,digest in style[category].items():
        path=(root/name).resolve()
        if not path.is_relative_to(root) or hashlib.sha256(path.read_bytes()).hexdigest()!=digest:raise ValueError('cluster style drift: '+name)
        checked+=1
spec=load(root/'examples/crpg-gallery/cluster-cases.json'); report=load(fixture/'report.json')
assert manifest['case_count']==len(report['cases'])==30
assert manifest['capture_count']==sum(len(c['captures']) for c in report['cases'])==210
assert sum(len(c['input_trace']) for c in report['cases'])==sum(map(len,spec['inputs'].values()))==50
frames=[24,63,90,120,180,240,300]
for case in report['cases']:
    assert not case['errors'] and not case['blocked'] and not case['state']['errors']
    assert [c['frame'] for c in case['captures']]==frames
    assert len(case['input_trace'])==len(spec['inputs'].get(case['id'],[]))
    for capture in case['captures']:
        data=(fixture/f"{case['id']}-{capture['frame']}.png").read_bytes()
        assert data[:8]==b'\x89PNG\r\n\x1a\n'
        assert struct.unpack('>II',data[16:24])==(math.ceil(case['width']*2),math.ceil(case['height']*2))
print(f'PASS cluster integrity: {checked} SHA256 / 30 cases / 210 captures / 50 input operations')
if len(sys.argv)>1:
    out=Path(sys.argv[1]); native=[json.loads(l) for l in (out/'native-captures.jsonl').read_text(encoding='utf-8').splitlines()]
    index={(c['id'],c['frame']):c for c in native}; assert len(native)==len(index)==210
    geometry_count=0; maximum=0; bounds=[]
    for case in report['cases']:
        for capture in case['captures']:
            id=case['id']; frame=capture['frame']; actual=index[id,frame]
            assert actual['state']==capture['state'],(id,frame,'native/browser bridge state')
            reference=load(fixture/f'{id}-{frame}-dom.json')['dom']
            content=lambda a:[(n['tag'],re.sub(r'\s+',' ',n['text']).strip()) for n in a]
            assert content(actual['dom'])==content(reference),(id,frame,'DOM order/leaf text')
            a=load(out/f'{id}-{frame}-geometry.json'); b=capture['geometry'][1:]
            # Inline getBoundingClientRect currently reports the line box. Do
            # not count b/strong/span glyph bounds as full inline conformance.
            a=[n for n in a if n['tag'] in ('div','button')]
            b=[n for n in b if n['tag'] in ('div','button')]
            assert len(a)==len(b),(id,frame,'HTML element count')
            for x,y in zip(a,b):
                assert x['tag']==y['tag']
                delta=max(abs(v-w) for v,w in zip(x['rect'],y['rect']))
                # Icon cycle has one scheduler frame of spring phase offset.
                # This bound is separate from the unchanged dense pixel oracle.
                limit=3 if id=='icon-01' else .3 if id=='anime-10' else .1
                assert delta<=limit,(id,frame,delta,limit)
                maximum=max(maximum,delta);geometry_count+=4
    results={'format':'gallery-cluster-independent-audit-v1','state_pairs':210,'dom_pairs':210,'html_geometry_fields':geometry_count,'max_html_delta_px':maximum,'icon_cycle_phase_tolerance_px':3,'inline_glyph_bounds_verified':False,'original_issue':{'id':'anime-11','observation':'unchanged original throw has zero movement in browser and native; inertia verified separately using pinned Anime with valid axis transforms'}}
    (out/'independent-audit.json').write_text(json.dumps(results,indent=2)+'\n',encoding='utf-8')
    print(f'PASS native/browser: 210 exact states / 210 DOM structures / {geometry_count} HTML geometry fields')
