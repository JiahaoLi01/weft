"""Fixed real Chromium bitmap references; offline integrity only."""
from pathlib import Path
import hashlib
import json

root = Path(__file__).resolve().parents[1]
fixture = root / 'tests/fixtures/canvas-gallery-reference'
record = json.loads((fixture / 'manifest.json').read_text(encoding='utf-8'))
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
assert record['frames_per_case'] == 64 and record['seed'] == 0xce202610
assert set(record['files']) == {f'webgl-0{i}-{f}-bitmap.png' for i in range(2) for f in (11, 39, 63)}
for name, sha in record['files'].items():
    assert digest(fixture / name) == sha, 'Canvas browser bitmap drift: ' + name
for name, sha in record['inputs'].items():
    assert digest(root / name) == sha, 'Canvas browser input drift: ' + name
for case in record['cases']:
    assert not case['errors'] and not case['blocked'] and not case['state']['errors']
    assert [c['rect'] for c in case['captures']] == [[0, 0, 800, 600], [0, 0, 400, 300], [0, 0, 800, 600]]
print('PASS 6 real Chromium Canvas bitmaps and fixed original-source/input identities')
