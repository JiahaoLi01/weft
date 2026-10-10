"""Independent real-browser/native React behavior and CSS geometry comparison."""
import argparse
import hashlib
import json
import math
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('browser', type=Path)
p.add_argument('native', type=Path)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
b, n = (json.loads(v.read_text(encoding='utf-8')) for v in (a.browser, a.native))
root = Path(__file__).resolve().parents[1]
for name, expected in b['source_sha256'].items():
    assert hashlib.sha256((root / name).read_bytes()).hexdigest() == expected, f'stale browser input: {name}'
assert b['fixture'] == n['fixture'] == 'caseelephant-react-vite-v1'
assert b['react'] == n['react'] == '19.3.0' and b['vite'] == n['vite'] == '8.3.3'
assert b['font_loaded'] and b['keyed_identity_preserved'] and not b['errors']
assert n['passed'] and n['actual_completion_query'] and n['keyed_identity_preserved'] and n['snapshots_survive_vm_reset']
assert n['native_event_dispatch_verified'] and n['root_unmount_verified']
assert n['debug_layer'] and n['debug_warning_error_count'] == 0 and n['native_textures_retired'] == 522
assert len(b['frames']) == len(n['frames']) == 3
maximum = 0.0
fields = 0
for i, (bf, nf) in enumerate(zip(b['frames'], n['frames'])):
    assert bf['state'] == nf['state'], f'frame {i} React state mismatch'
    assert nf['direct_rtt_max_difference'] <= 2
    bb, nb = bf['layout']['boxes'], nf['layout']['boxes']
    assert len(bb) == len(nb) == 25
    assert [v['id'] for v in bb] == [v['id'] for v in nb], f'frame {i} DOM/layout order'
    for expected, actual in zip(bb, nb):
        for field in ('x', 'y', 'width', 'height'):
            v, w = expected[field], actual[field]
            assert math.isfinite(v) and math.isfinite(w), 'non-finite geometry'
            difference = abs(v - w)
            assert difference <= 0.1, f'frame {i} {actual["id"]}.{field}: browser {v}, native {w}'
            maximum = max(maximum, difference)
            fields += 1
result = {'passed': True, 'frames': 3, 'boxes_per_frame': 25, 'geometry_fields': fields,
          'max_geometry_difference_css_px': maximum, 'tolerance_css_px': 0.1, 'browser_dpr': b['dpr'],
          'pixel_browser_parity_verified': False}
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
print(f'PASS actual browser/native React state and {fields} geometry fields; max difference {maximum:.6f} CSS px (browser DPR {b["dpr"]:.6f})')
