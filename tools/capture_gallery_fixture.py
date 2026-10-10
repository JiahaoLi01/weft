"""Copy a consistent immutable gallery reference, without writing to the game."""
import argparse
import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--source', type=Path, required=True, help='Original UI directory')
p.add_argument('--destination', type=Path, default=Path(__file__).resolve().parents[1] / 'tests/fixtures/crpg-gallery')
a = p.parse_args()
source, destination = a.source.resolve(), a.destination.resolve()
if destination.exists():
    raise SystemExit('Snapshot exists; use a new destination for a newer revision')
paths = sorted(x for x in (source / 'src').rglob('*') if x.is_file() and x.suffix.lower() in {'.ts', '.tsx', '.js', '.jsx', '.css', '.json', '.md', '.html', '.svg'})
paths += [source / x for x in ('package.json', 'package-lock.json', 'vite.config.ts', 'index.html')]
payloads = {x.relative_to(source).as_posix(): x.read_bytes() for x in paths}
if any(len(x) > 8 * 1024 * 1024 for x in payloads.values()) or sum(map(len, payloads.values())) > 32 * 1024 * 1024:
    raise SystemExit('Reference exceeds capture byte budget')
for relative, data in payloads.items():
    if (source / relative).read_bytes() != data:
        raise SystemExit('Reference changed during capture; no snapshot published')
lock = json.loads(payloads['package-lock.json'])['packages']
names = ['react', 'react-dom', 'd3', 'three', 'framer-motion', '@react-spring/web', 'animejs', 'lottie-web', 'lucide-react', 'vite', 'typescript', '@tailwindcss/vite']
versions = {k: {'version': lock['node_modules/' + k]['version'],
                'integrity': lock['node_modules/' + k].get('integrity'),
                'license': lock['node_modules/' + k].get('license')} for k in names}
manifest = {'format': 'crpg-gallery-readonly-v1', 'source_root': str(source),
            'captured_utc': datetime.now(timezone.utc).isoformat(),
            'consistency': 'all bytes reread before publishing',
            'files': {k: hashlib.sha256(v).hexdigest() for k, v in payloads.items()},
            'runtime_versions': versions,
            'note': 'Original source is unmodified. Text-source/CSS reference; font/bitmap assets, remote Lottie JSON and game services not captured.',
            'full_gallery_passed': False}
for relative, data in payloads.items():
    target = destination / 'UI' / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
(destination / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(f'Captured {len(payloads)} files / {sum(map(len, payloads.values()))} bytes; original UI left read-only')
