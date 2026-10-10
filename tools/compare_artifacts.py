"""Compare two artifact trees produced by the CTest runs (refactor evidence).

    python tools/compare_artifacts.py <baseline_dir> <current_dir> [--suites a,b,...] [--exclude a,b,...] [--show N]

Images (.png, .rgba) must match byte for byte. JSON reports are compared value by value after dropping
fields that measure wall-clock time, host scheduling or the machine (see VOLATILE); every other value,
including pixel samples, geometry, counts and retirement totals, must be identical. Prints one line per
suite and exits nonzero on any difference or missing file.

Real-time window suites depend on wall-clock pacing and thread scheduling (wheel coalescing, frames presented
inside a time window) and differ between two runs of the same binary; exclude them and rely on their own
assertions. Suite names carry the configuration suffix, e.g. gallery-preview-Release.
"""
import argparse
import json
import pathlib
import re
import sys

# Field names whose values depend on timing or on the host machine, not on rendering semantics.
VOLATILE = re.compile(
    r"(^|_)(ms|us|ns|seconds|sec|time|times|timestamp|elapsed|duration|fps|hz|clock|wall|cpu|wait|waits|"
    r"latency|age|p50|p95|p99|max_ms|mean_ms|polls|adapter|driver_version|pid|memory|working_set|private_bytes|"
    r"started|finished|date|host_time|samples_ms|run_id)($|_)", re.IGNORECASE)


def volatile(key):
    return bool(VOLATILE.search(key))


def strip(value):
    if isinstance(value, dict):
        return {k: strip(v) for k, v in value.items() if not volatile(k)}
    if isinstance(value, list):
        return [strip(v) for v in value]
    return value


def first_difference(a, b, path=""):
    if type(a) is not type(b):
        return f"{path}: type {type(a).__name__} != {type(b).__name__}"
    if isinstance(a, dict):
        for key in sorted(set(a) | set(b)):
            if key not in a or key not in b:
                return f"{path}/{key}: only in {'current' if key not in a else 'baseline'}"
            found = first_difference(a[key], b[key], f"{path}/{key}")
            if found:
                return found
        return None
    if isinstance(a, list):
        if len(a) != len(b):
            return f"{path}: length {len(a)} != {len(b)}"
        for i, (x, y) in enumerate(zip(a, b)):
            found = first_difference(x, y, f"{path}[{i}]")
            if found:
                return found
        return None
    return None if a == b else f"{path}: {a!r} != {b!r}"


def load_json(path):
    text = path.read_text(encoding="utf-8-sig")
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return [json.loads(line) for line in text.splitlines() if line.strip()]


def compare_suite(base, current):
    problems = []
    files = 0
    for path in sorted(p for p in base.rglob("*") if p.is_file()):
        relative = path.relative_to(base)
        other = current / relative
        if path.suffix not in (".png", ".rgba", ".json", ".jsonl"):
            continue
        files += 1
        if not other.is_file():
            problems.append(f"missing {relative}")
            continue
        if path.suffix in (".png", ".rgba"):
            if path.read_bytes() != other.read_bytes():
                problems.append(f"image differs {relative}")
            continue
        difference = first_difference(strip(load_json(path)), strip(load_json(other)))
        if difference:
            problems.append(f"json differs {relative} {difference[:300]}")
    return files, problems


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("baseline", type=pathlib.Path)
    parser.add_argument("current", type=pathlib.Path)
    parser.add_argument("--suites", default="")
    parser.add_argument("--exclude", default="")
    parser.add_argument("--show", type=int, default=5)
    args = parser.parse_args()
    wanted = set(filter(None, args.suites.split(",")))
    excluded = set(filter(None, args.exclude.split(",")))
    failed = 0
    total = 0
    for suite in sorted(p for p in args.baseline.iterdir() if p.is_dir()):
        if (wanted and suite.name not in wanted) or suite.name in excluded:
            continue
        files, problems = compare_suite(suite, args.current / suite.name)
        total += files
        status = "same" if not problems else f"DIFFERENT ({len(problems)})"
        print(f"{suite.name:40s} {files:4d} files  {status}")
        for problem in problems[:args.show]:
            print("    " + problem)
        failed += bool(problems)
    print(f"{total} files compared; {failed} suites differ")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
