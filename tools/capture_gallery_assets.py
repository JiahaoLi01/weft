"""Copy named binary assets of the original UI (such as public/cloud.png) into an immutable fixture, without writing to
the game. Text sources are captured by capture_gallery_fixture.py; this snapshot holds the bitmaps they load."""
import argparse
import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--source', type=Path, required=True, help='Original UI directory')
p.add_argument('--destination', type=Path,
               default=Path(__file__).resolve().parents[1] / 'tests/fixtures/crpg-gallery-assets')
p.add_argument('files', nargs='+', help='Paths relative to the original UI directory')
a = p.parse_args()
source, destination = a.source.resolve(), a.destination.resolve()
if destination.exists():
    raise SystemExit('Snapshot exists; use a new destination for a newer revision')
relative = sorted(set(Path(f).as_posix() for f in a.files))
for name in relative:
    path = (source / name).resolve()
    if source not in path.parents or not path.is_file():
        raise SystemExit(f'Not a file inside the original UI: {name}')
payloads = {name: (source / name).read_bytes() for name in relative}
if any(len(x) > 16 * 1024 * 1024 for x in payloads.values()) or sum(map(len, payloads.values())) > 32 * 1024 * 1024:
    raise SystemExit('Assets exceed capture byte budget')
for name, data in payloads.items():
    if (source / name).read_bytes() != data:
        raise SystemExit('Asset changed during capture; no snapshot published')
manifest = {'format': 'crpg-gallery-assets-readonly-v1', 'source_root': str(source),
            'captured_utc': datetime.now(timezone.utc).isoformat(),
            'consistency': 'all bytes reread before publishing',
            'files': {k: {'sha256': hashlib.sha256(v).hexdigest(), 'bytes': len(v)} for k, v in payloads.items()},
            'note': 'Original bitmaps, unmodified, as the captured UI source loads them.'}
for name, data in payloads.items():
    target = destination / 'UI' / name
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
(destination / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(f'Captured {len(payloads)} assets / {sum(map(len, payloads.values()))} bytes; original UI left read-only')
