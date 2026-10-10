"""Offline bundle/input/font provenance; live CaseCheetah is never accessed."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
meta = json.loads((root/'examples/cheetah-alert/native/metadata.json').read_text(encoding='utf-8'))
assert meta['fixture'] == 'casecheetah-alert-v1'
assert (meta['react'],meta['vite'],meta['zustand'],meta['noto']) == ('19.3.0','8.3.3','5.0.15','Sans2.004')
for group in ('inputs','outputs'):
    for path,wanted in meta[group].items():
        assert hashlib.sha256((root/path).read_bytes()).hexdigest() == wanted, path
notices=(root/'examples/cheetah-alert/native/BUNDLED-LICENSES.txt').read_bytes()
for license_path in ('third_party/react-runtime/BUNDLED-LICENSES.txt','third_party/zustand/source/LICENSE',
                     'third_party/noto-cjk/source/LICENSE','third_party/probe-font-notices/NOTICE.txt',
                     'third_party/probe-font-notices/LICENSE-Apache-2.0.txt'):
    assert (root/license_path).read_bytes() in notices, 'missing notice: '+license_path
manifest = json.loads((root/'tests/fixtures/casecheetah/manifest.json').read_text(encoding='utf-8'))
for path,wanted in manifest['files'].items():
    assert hashlib.sha256((root/'tests/fixtures/casecheetah'/path).read_bytes()).hexdigest() == wanted, path
shell = (root/'tests/fixtures/casecheetah/UI/src/shell/shell.css').read_text(encoding='utf-8').splitlines()[4:9]
assert (root/'examples/cheetah-alert/src/profile.css').read_text(encoding='utf-8') == '\n'.join(shell)+'\n'+(root/'examples/cheetah-alert/src/layout.css').read_text(encoding='utf-8')
golden = json.loads((root/'tests/fixtures/cheetah_alert_browser_reference.json').read_text(encoding='utf-8'))
for path,wanted in golden['captured_source_hashes'].items():
    assert hashlib.sha256((root/path).read_bytes()).hexdigest() == wanted, 'stale browser reference: '+path
print(f'PASS AlertBar fixed source/bundle/notices: {len(meta["inputs"])} inputs, {len(manifest["files"])} captured game files')
