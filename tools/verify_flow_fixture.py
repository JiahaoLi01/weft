"""Offline provenance of new shared DOM/CSS and unchanged captured React."""
import hashlib,json
from pathlib import Path
root=Path(__file__).resolve().parents[1]
sha=lambda p:hashlib.sha256((root/p).read_bytes()).hexdigest()
m=json.loads((root/'examples/cheetah-flow/metadata.json').read_text(encoding='utf-8'))
for group in ('inputs','outputs'):
 for p,h in m[group].items():
  if group=='outputs' and p.endswith('.png') and not (root/p).exists():continue
  assert sha(p)==h,p
for name in ('flow_layout_browser_reference.json','cheetah_flow_browser_reference.json'):
 b=json.loads((root/'tests/fixtures'/name).read_text(encoding='utf-8'))
 for p,h in b['sources'].items():assert sha(p)==h,(name,p)
 assert len(b['frames'])==(3 if name.startswith('flow_layout') else 8)
 if name.startswith('flow_layout'):assert all(all(f) for f in b['fonts_loaded'])
 else:assert all(f['fonts_loaded'] for f in b['frames'])
print('PASS flow fixtures and Chromium source hashes')
