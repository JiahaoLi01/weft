"""The text phase reference (tools/browser_text_phase_reference.mjs, tests/fixtures/text-phase-reference) is of its
inputs as they are (their SHA-256 with LF line ends, as a checkout may give them CRLF), with a screenshot of the page's
size at each scale."""
import hashlib
import json
import struct
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
fixture = root / 'tests/fixtures/text-phase-reference'
reference = json.loads((fixture / 'reference.json').read_text(encoding='utf-8'))
for path, digest in reference['inputs'].items():
    data = (root / path).read_bytes().replace(b'\r\n', b'\n')
    if hashlib.sha256(data).hexdigest() != digest:
        sys.exit(f'stale text phase reference: {path} changed since the capture '
                 '(node tools/browser_text_phase_reference.mjs)')
for scale in reference['scales']:
    png = (fixture / f'chrome-{scale}x.png').read_bytes()
    assert png[:8] == b'\x89PNG\r\n\x1a\n', f'chrome-{scale}x.png is no PNG'
    size = struct.unpack('>II', png[16:24])
    assert size == (reference['width'] * scale, reference['height'] * scale), (scale, size)
print(f'PASS text phase fixture: {len(reference["inputs"])} inputs, scales {reference["scales"]} from '
      f'{reference["browser"]}')
