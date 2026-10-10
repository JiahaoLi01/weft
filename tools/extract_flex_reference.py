"""Extract the report from actual Chrome dump-dom output. No geometry is invented."""
from pathlib import Path
from html.parser import HTMLParser
import argparse
import hashlib
import json

class ReportParser(HTMLParser):
    def __init__(self):
        super().__init__()
        self.collecting = False
        self.pieces = []
    def handle_starttag(self, tag, attrs):
        if tag == 'pre' and dict(attrs).get('id') == 'report':
            self.collecting = True
    def handle_endtag(self, tag):
        if tag == 'pre':
            self.collecting = False
    def handle_data(self, data):
        if self.collecting:
            self.pieces.append(data)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('html', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    page = ReportParser()
    page.feed(args.html.read_text(encoding='utf-8'))
    report = json.loads(''.join(page.pieces))
    if 'error' in report:
        raise SystemExit(report['error'])
    source = root / 'tests/fixtures/flex_layout_cases.json'
    inputs = json.loads(source.read_bytes())
    if not report.get('font_loaded') or [c['id'] for c in report['cases']] != [c['id'] for c in inputs['cases']]:
        raise SystemExit('Missing font or incorrect case list; browser capture is invalid')
    if report.get('input_sha256') != hashlib.sha256(source.read_bytes()).hexdigest():
        raise SystemExit('Browser input hash differs from current fixture; recapture the browser reference')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8',newline='\n')
    print(f'Captured {len(report["cases"])} independent browser cases: {report["browser"]}')

if __name__ == '__main__':
    main()
