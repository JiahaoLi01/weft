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
# The bitmaps the captured source loads (capture_gallery_assets.py).
assets = root / 'tests/fixtures/crpg-gallery-assets'
asset_manifest = json.loads((assets / 'manifest.json').read_text(encoding='utf-8'))
for name, record in asset_manifest['files'].items():
    path = assets / 'UI' / name
    data = path.read_bytes() if path.is_file() else b''
    if len(data) != record['bytes'] or hashlib.sha256(data).hexdigest() != record['sha256']:
        raise SystemExit('Gallery asset drift: ' + name)
metadata = root / 'examples/crpg-gallery/native/metadata.json'
if metadata.exists():
    build = json.loads(metadata.read_text(encoding='utf-8'))
    for category in ('inputs', 'outputs'):
        for name, digest in build[category].items():
            if hashlib.sha256((root / name).read_bytes()).hexdigest() != digest:
                raise SystemExit('Gallery bundle drift: ' + name)
# The nested tooltip page: its bundles (tools/build_tooltip_page.mjs) and stylesheet
# (tools/compile_gallery_cluster_styles.py), from the captured component and the page's copy.
for built, keys in (('examples/crpg-gallery/native/tooltip-metadata.json', ('inputs', 'outputs')),
                    ('examples/crpg-gallery/tooltip-style-manifest.json', ('input_sha256', 'output_sha256'))):
    record = json.loads((root / built).read_text(encoding='utf-8'))
    for category in keys:
        for name, digest in record[category].items():
            if hashlib.sha256((root / name).read_bytes()).hexdigest() != digest:
                raise SystemExit('Tooltip page drift: ' + name)
# Its Chrome reference (tools/browser_tooltip_page_reference.mjs) was recorded from these bundles, sheet, clocks and
# case.
reference = json.loads((root / 'tests/fixtures/tooltip-reference/report.json').read_text(encoding='utf-8'))
for name, digest in reference['source_files'].items():
    if hashlib.sha256((root / name).read_bytes()).hexdigest() != digest:
        raise SystemExit('Tooltip reference drift: ' + name)
print(f'PASS immutable gallery source, {len(asset_manifest["files"])} bitmaps and {len(cases)} original case records')
