"""Compare independent browser/native fixture JSON, without running either engine."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("native", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    reference_bytes = args.reference.read_bytes()
    native_bytes = args.native.read_bytes()
    reference = json.loads(reference_bytes)
    native = json.loads(native_bytes)
    if not isinstance(reference, dict) or not isinstance(native, dict):
        parser.error("Both reports must be JSON objects")
    fields = sorted(reference.keys() | native.keys())
    differences = [key for key in fields
                   if key not in reference or key not in native or reference[key] != native[key]]
    summary = {
        "reference": args.reference.as_posix(),
        "native": args.native.as_posix(),
        "reference_sha256": hashlib.sha256(reference_bytes).hexdigest(),
        "native_sha256": hashlib.sha256(native_bytes).hexdigest(),
        "matched": not differences,
        "fields_compared": len(fields),
        "different_fields": differences,
        "checks_passed": native.get("checks_passed"),
        "native_rendering_verified": native.get("native_rendering_verified"),
    }
    encoded = json.dumps(summary, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    return 1 if differences else 0


if __name__ == "__main__":
    raise SystemExit(main())
