"""Verify vendored source against the reviewed local dependency lock; no network."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    manifest = json.loads((root / "third_party/manifest.json").read_text(encoding="utf-8"))
    failures: list[str] = []
    for dependency in manifest["dependencies"]:
        source = root / dependency["source_dir"]
        expected = dependency["files"]
        actual = {p.relative_to(source).as_posix() for p in source.rglob("*") if p.is_file()}
        for extra in sorted(actual - expected.keys()):
            failures.append(f'{dependency["name"]}: unexpected file {extra}')
        for name, wanted in expected.items():
            file = source / name
            if not file.is_file():
                failures.append(f'{dependency["name"]}: missing {name}')
            elif hashlib.sha256(file.read_bytes()).hexdigest() != wanted:
                failures.append(f'{dependency["name"]}: SHA256 mismatch {name}')
        print(f'{dependency["name"]} {dependency["version"]}: {len(expected)} locked files')
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print("Dependency hashes verified; no downloads performed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
