"""Offline pin checks, including optional browser copies and saved oracles."""
import hashlib,json
from pathlib import Path
root=Path(__file__).resolve().parents[1];sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
m=json.loads((root/'examples/cheetah-auto/metadata.json').read_text(encoding='utf-8'))
generated={'app.js','BUNDLED-LICENSES.txt','Roboto-Regular.ttf','NotoSansCJKsc-Regular.otf','ink-panel-v2.png'}
for group in ('inputs','outputs'):
 for name,digest in m[group].items():
  path=root/name
  if group=='outputs' and path.name in generated and not path.exists():continue
  assert sha(path)==digest,name
for name in ('auto_layout_browser_reference.json','cheetah_auto_browser_reference.json'):
 b=json.loads((root/'tests/fixtures'/name).read_text(encoding='utf-8'))
 for path,digest in b['sources'].items():assert sha(root/path)==digest,(name,path)
 assert len(b['frames'])==(3 if name.startswith('auto_layout') else 8)
 if name.startswith('cheetah'):assert all(f['fonts_loaded'] for f in b['frames'])
assert (root/'examples/cheetah-auto/app.js').read_bytes()==(root/'examples/cheetah-alert/native/app.js').read_bytes() if (root/'examples/cheetah-auto/app.js').exists() else True
print('PASS automatic-layout workload, unchanged bundle/assets and browser provenance')
