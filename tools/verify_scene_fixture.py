import json,hashlib
from pathlib import Path
root=Path(__file__).resolve().parents[1]
checked=0
for manifest in ('examples/crpg-gallery/scene-style-manifest.json','tests/fixtures/scene-gallery-reference/manifest.json'):
    data=json.loads((root/manifest).read_text(encoding='utf-8'))
    for key in ('input_sha256','output_sha256'):
        for name,expected in data[key].items():
            path=(root/name).resolve()
            if not path.is_relative_to(root):raise ValueError('manifest path escapes root')
            if hashlib.sha256(path.read_bytes()).hexdigest()!=expected:raise ValueError('scene fixture changed: '+name)
            checked+=1
print(f'PASS fixed original scene styles, bundles and browser fixtures: {checked} SHA256 values')
