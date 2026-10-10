"""Normal-build integrity check. No original-game access or network."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
fixture = root / 'tests/fixtures/crpg-gallery'
manifest = json.loads((fixture / 'manifest.json').read_text(encoding='utf-8'))
for name, digest in manifest['files'].items():
    path = fixture / 'UI' / name
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        raise SystemExit('Gallery source drift: ' + name)
inventory = json.loads((fixture / 'inventory.json').read_text(encoding='utf-8'))
source = fixture / 'UI/src/components/debug/TestGallery.tsx'
if inventory['source_sha256'] != hashlib.sha256(source.read_bytes()).hexdigest():
    raise SystemExit('Gallery inventory belongs to another source revision')
cases = [case for group in inventory['groups'] for case in group['cases']]
if len(cases) != inventory['case_count'] or len({case['id'] for case in cases}) != len(cases):
    raise SystemExit('Gallery case inventory is incomplete or ambiguous')
for case in cases:
    # JS offsets are UTF-16, so the generated inventory also stores raw UTF-8 bytes.
    data = source.read_bytes()[case['utf8_start']:case['utf8_end']]
    if hashlib.sha256(data).hexdigest() != case['case_source_sha256']:
        raise SystemExit('Gallery case drift: ' + case['id'])
metadata = root / 'examples/crpg-gallery/native/metadata.json'
if metadata.exists():
    build = json.loads(metadata.read_text(encoding='utf-8'))
    for category in ('inputs', 'outputs'):
        for name, digest in build[category].items():
            if hashlib.sha256((root / name).read_bytes()).hexdigest() != digest:
                raise SystemExit('Gallery bundle drift: ' + name)
print(f'PASS immutable gallery source and {len(cases)} original case records')
