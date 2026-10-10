"""Offline source/font provenance for the independent hit oracle."""
import hashlib, json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
reference = json.loads((root / 'tests/fixtures/flow_input_browser_reference.json').read_text(encoding='utf-8'))
for path, expected in reference['sources'].items():
    assert hashlib.sha256((root / path).read_bytes()).hexdigest() == expected, path
assert reference['boundaryToleranceCssPx'] == 2
assert len(reference['frames']) == 3
for frame in reference['frames']:
    assert frame['fonts'] and len(frame['points']) == 1050
    assert all(point['target'] in point['nearby'] for point in frame['points'])
print('PASS flow input fixture and Chromium source/font hashes')
