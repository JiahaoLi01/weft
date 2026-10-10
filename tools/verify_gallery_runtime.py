"""Read-only, offline source and license identity check, independent of Node links."""
import hashlib
import json
import os
from pathlib import Path

root = Path(__file__).resolve().parents[1] / 'third_party/gallery-runtime'
manifest = json.loads((root / 'manifest.json').read_text(encoding='utf-8'))
actual = set()
for folder, dirs, files in os.walk(root / 'source'):
    dirs[:] = [name for name in dirs if name != 'node_modules']
    actual.update((Path(folder) / name).relative_to(root).as_posix() for name in files)
if actual != set(manifest['files']):
    raise SystemExit('Unexpected/missing retained gallery package files: ' + str(sorted(actual ^ set(manifest['files']))[:10]))
for name, digest in manifest['files'].items():
    if hashlib.sha256((root / name).read_bytes()).hexdigest() != digest:
        raise SystemExit('Gallery dependency drift: ' + name)
for package in manifest['packages'].values():
    data = json.loads((root / package['source_dir'] / 'package.json').read_text(encoding='utf-8'))
    if data['name'] != package['name'] or data['version'] != package['version'] or not package['licenses']:
        raise SystemExit('Gallery dependency identity/license missing: ' + package['name'])
    if any(name not in manifest['files'] for name in package['licenses']):
        raise SystemExit('Unpinned license: ' + package['name'])
print(f"PASS {len(manifest['packages'])} gallery packages / {len(manifest['files'])} pinned files; no install/network")
