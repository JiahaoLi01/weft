"""Offline provenance validation: exact captured CSS, assets and browser oracles."""
import hashlib
import json
from pathlib import Path
root=Path(__file__).resolve().parents[1]
hash_file=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
meta=json.loads((root/'examples/cheetah-style/metadata.json').read_text(encoding='utf-8'))
generated={'app.js','BUNDLED-LICENSES.txt','Roboto-Regular.ttf','NotoSansCJKsc-Regular.otf','ink-panel-v2.png'}
for group in ('inputs','outputs'):
    for name,digest in meta[group].items():
        path=root/name
        # Native tests use the original pinned bundle/assets directly. Browser
        # copies are optional on a fresh checkout; verify every existing copy.
        if group=='outputs' and path.name in generated and not path.exists():continue
        assert hash_file(path)==digest,name
css=(root/'examples/cheetah-style/profile.css').read_text(encoding='utf-8')
original=(root/'tests/fixtures/casecheetah/UI/src/operations.css').read_text(encoding='utf-8')
for rule in meta['original_css'].values():assert rule in css and rule in original
for name in ('style_browser_reference.json','cheetah_style_browser_reference.json'):
    data=json.loads((root/'tests/fixtures'/name).read_text(encoding='utf-8'))
    for path,digest in data['source_hashes'].items():assert hash_file(root/path)==digest,(name,path)
    assert len(data['frames'])==(3 if name.startswith('style_') else 8)
if (root/'examples/cheetah-style/app.js').exists():
    assert (root/'examples/cheetah-style/app.js').read_bytes()==(root/'examples/cheetah-alert/native/app.js').read_bytes()
print('PASS fixed style CSS / unchanged component bundle / assets / browser provenance')
