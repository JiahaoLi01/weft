"""Compare portable scene text metrics with Chrome (tests/fixtures/scene_text_reference.json).

    python tools/compare_scene_text.py <reference.json> <native.json> [--output comparison.json]

Each case must be the same (family list, weight, size, text, letter-spacing, line-height) in both files. Chrome's
platform fonts for a span must equal the faces the native text used (by family name), and width, height and
baseline must agree within the tolerances below.

Synthetic bold (a bold request served by a lighter face) is not implemented: Chrome emboldens such glyphs and widens
their advances. Those cases still check face choice, height and baseline; their widths are reported, not required.
"""
import argparse
import json
import pathlib
import sys

# Width: both sides sum 16.16 HarfBuzz advances and round up to a LayoutUnit (1/64 px); one LayoutUnit of slack
# absorbs float summation order.
WIDTH_TOLERANCE_PX = 1 / 64 + 1e-6
# Line box height and baseline offset: Blink's rounded font metrics and whole-pixel half-leading, reproduced exactly.
HEIGHT_TOLERANCE_PX = 1e-6
BASELINE_TOLERANCE_PX = 1e-6


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=pathlib.Path)
    parser.add_argument("native", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    reference = json.loads(args.reference.read_text(encoding="utf-8"))
    native = json.loads(args.native.read_text(encoding="utf-8"))
    rc, nc = reference["cases"], native["cases"]
    if len(rc) != len(nc):
        raise SystemExit(f"case count differs: reference {len(rc)}, native {len(nc)}")
    worst = {"width": 0.0, "height": 0.0, "baseline": 0.0}
    checked = {"width": 0.0, "height": 0.0, "baseline": 0.0}
    synthetic_cases = 0
    synthetic_width = 0.0
    failures = []
    for i, (r, n) in enumerate(zip(rc, nc)):
        key = ("family", "weight", "size", "text", "letterSpacing", "lineHeight")
        if any(r[k] != n[k] for k in key):
            raise SystemExit(f"case {i} differs: {[r[k] for k in key]} vs {[n[k] for k in key]}")
        chrome = sorted({f["family"] for f in r["fonts"]})
        used = n["faces"]
        matched = len(chrome) == len(used) and all(any(name in names for names in used) for name in chrome)
        errors = {k: abs(r[k] - n[k]) for k in worst}
        for k, v in errors.items():
            worst[k] = max(worst[k], v)
        synthetic = n.get("syntheticBold", False)
        if synthetic:
            synthetic_cases += 1
            synthetic_width = max(synthetic_width, errors["width"])
            errors_for_check = dict(errors, width=0.0)
        else:
            errors_for_check = errors
            for k, v in errors.items():
                checked[k] = max(checked[k], v)
        if (not matched or errors_for_check["width"] > WIDTH_TOLERANCE_PX or
                errors_for_check["height"] > HEIGHT_TOLERANCE_PX or
                errors_for_check["baseline"] > BASELINE_TOLERANCE_PX):
            failures.append({"case": i, "family": r["family"], "weight": r["weight"], "size": r["size"],
                             "text": r["text"], "letterSpacing": r["letterSpacing"], "lineHeight": r["lineHeight"],
                             "chrome_fonts": chrome, "native_faces": used, "errors": errors,
                             "chrome": [r["width"], r["height"], r["baseline"]],
                             "native": [n["width"], n["height"], n["baseline"]]})
    summary = {"browser": reference["browser"], "cases": len(rc), "failures": len(failures),
               "max_errors": checked, "synthetic_bold_cases": synthetic_cases,
               "synthetic_bold_max_width_error": synthetic_width, "tolerances": {"width": WIDTH_TOLERANCE_PX, "height": HEIGHT_TOLERANCE_PX,
                                                   "baseline": BASELINE_TOLERANCE_PX}, "failed": failures}
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(summary, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    print(f"{len(rc)} text cases vs {reference['browser']}: {len(failures)} failures; max errors {checked}; "
          f"{synthetic_cases} synthetic-bold cases (width up to {synthetic_width:.3f}px, not implemented)")
    for f in failures[:12]:
        print("   ", json.dumps(f, ensure_ascii=False)[:400])
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
