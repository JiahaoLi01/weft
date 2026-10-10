"""Fixed Chromium elementFromPoint oracle, including explicitly bounded edges."""
import json, sys
from pathlib import Path

reference = json.loads(Path(sys.argv[1]).read_text(encoding='utf-8'))
native = json.loads(Path(sys.argv[2]).read_text(encoding='utf-8'))
assert reference['fixture'] == 'flow-input-browser-v1' and native['fixture'] == 'flow-input-v1'
assert reference['boundaryToleranceCssPx'] == 2
assert native['passed'] and len(native['frames']) == len(reference['frames']) == 3
exact = 0
boundaries = []
for frame, (expected, actual) in enumerate(zip(reference['frames'], native['frames'])):
    assert expected['fonts'] and len(expected['points']) == len(actual) == 1050
    for point, (browser, result) in enumerate(zip(expected['points'], actual)):
        assert abs(browser['x'] - result['x']) < 1e-5 and abs(browser['y'] - result['y']) < 1e-5
        if browser['target'] == result['target']:
            exact += 1
        else:
            assert result['target'] in browser['nearby'], (frame, point, browser, result)
            boundaries.append({'frame': frame, 'point': point, 'x': result['x'], 'y': result['y'],
                               'browser': browser['target'], 'native': result['target']})
# Keep a hard budget as well as the spatial bound; don't silently grow tolerance.
assert exact >= 3130 and len(boundaries) <= 20, (exact, boundaries)
report = {'points': 3150, 'exact': exact, 'boundary_differences': boundaries,
          'boundary_tolerance_css_px_linf': 2, 'passed': True}
if len(sys.argv) > 3:
    Path(sys.argv[3]).write_text(json.dumps(report, indent=2), encoding='utf-8')
print(f'PASS flow hit reference: {exact}/3150 exact; {len(boundaries)} within 2 CSS px per axis')
