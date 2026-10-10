"""Offline source/font provenance for the independent hit oracle."""
import hashlib, json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
reference = json.loads((root / 'tests/fixtures/flow_input_browser_reference.json').read_text(encoding='utf-8'))
# The reference keeps the hashes it was captured with. Later edits that are proven not to change
# behavior (format-only / named-constant refactors, verified by Release disassembly equivalence)
# are listed as successors of the captured hash instead of rewriting the capture record.
revisions = json.loads((root / 'tests/fixtures/flow_input_source_revisions.json').read_text(encoding='utf-8'))
for path, expected in reference['sources'].items():
    accepted = {expected} | {r['sha256'] for r in revisions.get(path, {}).get(expected, [])}
    assert hashlib.sha256((root / path).read_bytes()).hexdigest() in accepted, path
assert reference['boundaryToleranceCssPx'] == 2
assert len(reference['frames']) == 3
for frame in reference['frames']:
    assert frame['fonts'] and len(frame['points']) == 1050
    assert all(point['target'] in point['nearby'] for point in frame['points'])
print('PASS flow input fixture and Chromium source/font hashes')
