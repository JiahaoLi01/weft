"""Compare native Flex geometry/text to a separately captured browser reference."""
from pathlib import Path
import argparse
import json
import math


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reference', type=Path)
    parser.add_argument('native', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    reference = json.loads(args.reference.read_text(encoding='utf-8'))
    native = json.loads(args.native.read_text(encoding='utf-8'))
    if reference['fixture'] != native['fixture'] or reference['input_sha256'] != native['input_sha256']:
        raise SystemExit('Fixture identity/hash mismatch; do not compare different inputs')
    tolerance = reference['tolerance_css_px']
    actual_cases = {c['id']:c for c in native['cases']}
    results = []
    differences = []
    checked = 0
    for case in reference['cases']:
        case_differences = []
        actual = actual_cases.pop(case['id'], None)
        if actual is None:
            case_differences.append({'error':'missing case'})
        else:
            boxes = {b['id']:b for b in actual['boxes']}
            for expected in case['boxes']:
                box = boxes.pop(expected['id'], None)
                if box is None:
                    case_differences.append({'node':expected['id'],'error':'missing node'})
                    continue
                for field in ('x','y','width','height','text_width','text_height','text_lines'):
                    if field not in expected:
                        continue
                    checked += 1
                    got = box.get(field)
                    want = expected[field]
                    if got is None or not math.isfinite(got) or abs(got-want) > (0 if field=='text_lines' else tolerance):
                        case_differences.append({'node':expected['id'],'field':field,'expected':want,'actual':got})
            for node in boxes:
                case_differences.append({'node':node,'error':'extra node'})
        results.append({'id':case['id'],'matched':not case_differences,'differences':case_differences})
        differences.extend(case_differences)
    for case in actual_cases:
        differences.append({'case':case,'error':'extra case'})
    summary = {'fixture':reference['fixture'],'input_sha256':native['input_sha256'],
               'mode':native['mode'],'tolerance_css_px':tolerance,'fields_checked':checked,
               'cases_passed':sum(c['matched'] for c in results),'cases_total':len(results),
               'matched':not differences,'cases':results}
    encoded = json.dumps(summary, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded, encoding='utf-8')
    print(f'{summary["mode"]}: {summary["cases_passed"]}/{summary["cases_total"]} cases; {checked} fields; tolerance {tolerance} CSS px')
    for case in results:
        if not case['matched']:
            print(case['id'], json.dumps(case['differences']))
    return 0 if summary['matched'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
