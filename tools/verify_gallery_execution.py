"""Audit completeness and prevent a handled fallback from becoming a pass."""
import json
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
inventory = json.loads((root / 'tests/fixtures/crpg-gallery/inventory.json').read_text(encoding='utf-8'))
report = json.loads(Path(sys.argv[1]).read_text(encoding='utf-8'))
expected = {c['id']: (g['name'], c['index'], c['label']) for g in inventory['groups'] for c in g['cases']}
seen = set()
for case in report['cases']:
    record = case['registration']
    identity = record['id']
    assert identity not in seen and expected[identity] == (record['group'], record['index'], record['label'])
    seen.add(identity)
    assert case['pixels'] == case['interaction'] == case['performance'] == 'not_run'
    assert case['reset_nodes'] == 3 and case['reset_roots'] == 1
    clean = not case['failure'] and not case['raf_exception_count'] and not case['unmount_failure']
    clean = clean and case['state'] is not None and not case['state']['errors'] and case['root_children'] > 0
    assert (case['execution'] == 'no_error_observed') == clean
    # Until a real context is implemented, never accept Three's error UI or a
    # swallowed Canvas effect error as supported rendering.
    if record['group'] == 'WEBGL_TESTS':
        assert case['execution'] == 'blocked'
    if record['group'] == 'SPRING_TESTS':
        assert case['execution'] == 'no_error_observed' and case['raf_callbacks'] == 12
        assert case['first_animation_dom'] != case['last_animation_dom'], 'Original Spring did not update native DOM'
assert seen == set(expected) and report['case_count'] == len(expected)
assert not report['rendering_verified'] and not report['interaction_verified'] and not report['performance_verified']
assert report['no_error_observed'] == sum(c['execution'] == 'no_error_observed' for c in report['cases'])
assert report['full_gallery']['phase'] in ('module', 'mount', 'schedule', 'completed')
print('PASS complete original case diagnostics, reset/epoch accounting and no fallback rendering claims')
