"""The logo's Chrome references (tests/fixtures/logo-reference) are of the logo as it is: every file the captures came
from (examples/logo, the capture tool, its clock) hashes as recorded, with LF line ends as Git stores them, and every
capture is there as recorded. Recapture with node tools/browser_logo_reference.mjs after changing the logo."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
fixture = root / 'tests' / 'fixtures' / 'logo-reference'
report = json.loads((fixture / 'report.json').read_text(encoding='utf-8'))
checked = 0
for name, expected in report['source_files'].items():
    path = (root / name).resolve()
    if not path.is_relative_to(root):
        raise ValueError('report path escapes the repository: ' + name)
    if hashlib.sha256(path.read_bytes().replace(b'\r\n', b'\n')).hexdigest() != expected:
        raise ValueError(f'{name} changed since the logo references were captured: '
                         'run node tools/browser_logo_reference.mjs')
    checked += 1
cases = json.loads((root / 'examples' / 'logo' / 'reference-cases.json').read_text(encoding='utf-8'))
captured = {(c['variant'], c['frame']) for c in report['captures']}
for variant in cases['variants']:
    for frame in cases['frames']:
        if (variant, frame) not in captured:
            raise ValueError(f'no capture of {variant} at frame {frame}')
for capture in report['captures']:
    if hashlib.sha256((fixture / capture['file']).read_bytes()).hexdigest() != capture['sha256']:
        raise ValueError('logo reference changed: ' + capture['file'])
    checked += 1
print(f'PASS logo references from {report["browser"]}: {checked} SHA256 values')
