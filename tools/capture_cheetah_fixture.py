"""Capture a consistent, read-only reference; never build or write in the game.

An existing snapshot is deliberately immutable. Capture to a new destination
when comparing a later revision of the game instead of silently refreshing it.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
from datetime import datetime, timezone

p = argparse.ArgumentParser()
p.add_argument('--source', type=Path, default=Path('F:/Projects/CaseCheetah'))
p.add_argument('--destination', type=Path, default=Path(__file__).resolve().parents[1] / 'tests/fixtures/casecheetah')
a = p.parse_args()
source = a.source.resolve()
destination = a.destination.resolve()
if destination.exists():
    raise SystemExit('Snapshot exists; use a new destination for a later game revision')
paths = sorted((source / 'UI/src').rglob('*'))
paths = [x for x in paths if x.is_file()]
paths += [source / 'UI' / x for x in ('index.html', 'package.json', 'package-lock.json', 'vite.config.ts')]
paths += [source / 'UI/public/art' / x for x in ('ink-panel-v2.png', 'ink-dock-left-v1.png', 'ink-dock-right-v1.png')]
payloads = {x.relative_to(source).as_posix(): x.read_bytes() for x in paths}
if any(len(x) > 8 * 1024 * 1024 for x in payloads.values()):
    raise SystemExit('Reference file exceeds snapshot bound')
for relative, data in payloads.items():
    if (source / relative).read_bytes() != data:
        raise SystemExit('Reference changed during capture; retry without writing a snapshot')
hashes = {k: hashlib.sha256(v).hexdigest() for k, v in payloads.items()}
css = {k: v.decode('utf-8') for k, v in payloads.items() if k.endswith('.css')}
properties = sorted(set(re.findall(r'(?:[;{]\s*)([-\w]+)\s*:', '\n'.join(css.values()))))
lock = json.loads(payloads['UI/package-lock.json'])
runtime = {k: lock['packages']['node_modules/' + k]['version'] for k in ('react', 'react-dom', 'zustand', 'd3', 'vite')}
manifest = {
    'format': 'casecheetah-readonly-snapshot-v1', 'source_root': str(source),
    'captured_utc': datetime.now(timezone.utc).isoformat(), 'source_git_revision': None,
    'consistency': 'all captured bytes reread before publishing snapshot',
    'files': hashes, 'source_runtime_versions': runtime,
    'inventory': {'source_files': len(payloads), 'public_png_count': len(list((source / 'UI/public').rglob('*.png'))),
                  'css_properties_heuristic': properties},
    'native_workload': 'Unmodified NineSlice.tsx + exact five shell.css rules + three original PNGs',
    'full_game_supported': False,
}
for relative, data in payloads.items():
    target = destination / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
(destination / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(f'Captured {len(payloads)} files / {sum(map(len, payloads.values()))} bytes; source left read-only')
