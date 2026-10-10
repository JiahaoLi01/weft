"""Offline source/bundle provenance check. Node/Vite are not needed by CTest."""
from pathlib import Path
import hashlib
import json

root = Path(__file__).resolve().parents[1]
meta = json.loads((root / 'examples/react-vite/native/metadata.json').read_text(encoding='utf-8'))
for group in ('inputs', 'outputs'):
    for name, expected in meta[group].items():
        actual = hashlib.sha256((root / name).read_bytes()).hexdigest()
        if actual != expected:
            raise SystemExit(f'React fixture {group} stale: {name}; rebuild with pinned npm ci/Vite and capture browser independently')
lock = json.loads((root / 'examples/react-vite/package-lock.json').read_text(encoding='utf-8'))
for name, version in [('react', '19.3.0'), ('react-dom', '19.3.0'), ('scheduler', '0.28.0'), ('vite', '8.3.3')]:
    assert lock['packages']['node_modules/' + name]['version'] == version, name
assert (root / 'examples/react-vite/native/BUNDLED-LICENSES.txt').read_bytes() == (root / 'third_party/react-runtime/BUNDLED-LICENSES.txt').read_bytes()
print('Pinned React/Vite input, native bundle and runtime notices verified offline')
