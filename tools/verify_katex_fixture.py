"""The KaTeX reference (tools/browser_katex_reference.mjs, tests/fixtures/katex-reference) is of the inputs as they are:
each input's SHA-256 (JavaScript and CSS with LF line ends, as a checkout may give them CRLF), every font beside
katex.css among them, and screenshots of the page's size and the same elements' rects at each scale."""
import hashlib
import json
import struct
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
fixture = root / 'tests/fixtures/katex-reference'
reference = json.loads((fixture / 'reference.json').read_text(encoding='utf-8'))
for path, digest in reference['inputs'].items():
    data = (root / path).read_bytes()
    if path.endswith(('.js', '.css')):
        data = data.replace(b'\r\n', b'\n')
    if hashlib.sha256(data).hexdigest() != digest:
        sys.exit(f'stale KaTeX reference: {path} changed since the capture (node tools/browser_katex_reference.mjs)')
fonts = {'third_party/katex/fonts/' + p.name for p in (root / 'third_party/katex/fonts').iterdir()}
missing = fonts - reference['inputs'].keys()
if missing:
    sys.exit('fonts the capture did not record: ' + ', '.join(sorted(missing)))
for scale in reference['scales']:
    png = (fixture / f'chrome-{scale}x.png').read_bytes()
    assert png[:8] == b'\x89PNG\r\n\x1a\n', f'chrome-{scale}x.png is no PNG'
    size = struct.unpack('>II', png[16:24])
    assert size == (reference['width'] * scale, reference['height'] * scale), (scale, size)
    rects = reference['rects'] if scale == 1 else reference['scaled_rects'][str(scale)]
    assert [r[:2] for r in rects] == [r[:2] for r in reference['rects']], f'the elements at {scale}x'
print(f'PASS KaTeX fixture: {len(reference["inputs"])} inputs, {len(reference["rects"])} rects, '
      f'scales {reference["scales"]} from {reference["browser"]}')
