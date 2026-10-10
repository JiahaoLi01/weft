"""Save provenance with samples extracted by WIC from an actual Chrome PNG."""
from pathlib import Path
import argparse
import hashlib
import json
from html.parser import HTMLParser

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('samples', type=Path)
    parser.add_argument('png', type=Path)
    parser.add_argument('--dom', type=Path, required=True)
    parser.add_argument('--browser-version', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    class Page(HTMLParser):
        flags = {}
        def handle_starttag(self, tag, attrs):
            if tag == 'html': self.flags = dict(attrs)
    page = Page(); page.feed(args.dom.read_text(encoding='utf-8'))
    if page.flags.get('data-done') != 'true' or page.flags.get('data-font-loaded') != 'true' or 'data-error' in page.flags:
        raise SystemExit('Browser did not complete the scene with its actual Ahem font')
    report = json.loads(args.samples.read_text(encoding='utf-8'))
    if report['source'] != 'actual Chrome PNG capture' or len(report['samples']) != 10:
        raise SystemExit('Expected the native WIC extractor output, not invented reference pixels')
    report.update(browser_file_version=args.browser_version, captured_date='2026-10-07',
                  browser_flags=['--force-color-profile=srgb', '--force-device-scale-factor=1',
                                 '--window-size=256,128', '--disable-gpu'],
                  png_sha256=sha(args.png),
                  dom_sha256=sha(args.dom), font_loaded=True,
                  html_sha256=sha(ROOT / 'tests/fixtures/gpu_color_reference.html'),
                  font_sha256=sha(ROOT / 'third_party/ahem/Ahem.ttf'),
                  purpose='Independent browser comparison; transparency color differences are intentional')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8', newline='\n')
    print(f'Saved {len(report["samples"])} actual browser color samples and provenance')


if __name__ == '__main__':
    main()
