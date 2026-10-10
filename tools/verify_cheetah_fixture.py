"""Offline provenance; does not read or update the live CaseCheetah checkout."""
from pathlib import Path
import hashlib
import json
root=Path(__file__).resolve().parents[1]
snapshot=root/'tests/fixtures/casecheetah'
manifest=json.loads((snapshot/'manifest.json').read_text(encoding='utf-8'))
for path,expected in manifest['files'].items():
    assert hashlib.sha256((snapshot/path).read_bytes()).hexdigest()==expected,path
meta=json.loads((root/'examples/cheetah-nine-slice/native/metadata.json').read_text(encoding='utf-8'))
for group in ('inputs','outputs'):
    for path,expected in meta[group].items():
        assert hashlib.sha256((root/path).read_bytes()).hexdigest()==expected,path
shell=(snapshot/'UI/src/shell/shell.css').read_text(encoding='utf-8').splitlines()[4:9]
layout=(root/'examples/cheetah-nine-slice/src/layout.css').read_text(encoding='utf-8')
assert (root/'examples/cheetah-nine-slice/src/profile.css').read_text(encoding='utf-8')=='\n'.join(shell)+'\n'+layout
assert (root/'examples/cheetah-nine-slice/native/BUNDLED-LICENSES.txt').read_bytes()==(root/'third_party/react-runtime/BUNDLED-LICENSES.txt').read_bytes()
reference=json.loads((root/'tests/fixtures/cheetah_browser_reference.json').read_text(encoding='utf-8'))
for path,expected in reference['captured_source_hashes'].items():
    assert hashlib.sha256((root/path).read_bytes()).hexdigest()==expected, f'Browser golden stale: {path}'
print(f"PASS immutable CaseCheetah reference: {len(manifest['files'])} source/assets; exact component, CSS section, native bundle")
