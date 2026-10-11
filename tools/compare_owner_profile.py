"""Alternate two weft_d3d11_probe builds on --gallery-owner-profile and compare owner CPU ticks.

    python tools/compare_owner_profile.py <baseline.exe> <candidate.exe> <out_dir> [--rounds 3] [--high-priority]
        [cases...]

Cases are the profile filters (kline, scatter, framer, spring, anime; default kline scatter). Each
round runs both binaries back to back and swaps the order every round. The report keeps every run's
P50/P95/P99/max of the warm owner tick (fixed 60 Hz clock, no GPU) and the median of each.
--high-priority runs both binaries in the Windows high priority class, so other desktop processes disturb
the samples less; it is recorded in the report and is not the default.
"""
import json
import pathlib
import statistics
import subprocess
import sys


def run(exe, folder, case, flags=0):
    folder.mkdir(parents=True, exist_ok=True)
    subprocess.run([exe, "--gallery-owner-profile", str(folder), case], check=True, stdout=subprocess.DEVNULL,
                   creationflags=flags)
    report = json.loads((folder / "report.json").read_text(encoding="utf-8"))
    return [c["warm_ticks_ms"] for c in report["cases"]]


def main():
    args = sys.argv[1:]
    rounds = 3
    if "--rounds" in args:
        rounds = int(args[args.index("--rounds") + 1])
        del args[args.index("--rounds"):args.index("--rounds") + 2]
    high_priority = "--high-priority" in args
    if high_priority:
        args.remove("--high-priority")
    flags = subprocess.HIGH_PRIORITY_CLASS if high_priority else 0
    baseline, candidate, out_dir = args[0], args[1], pathlib.Path(args[2])
    cases = args[3:] or ["kline", "scatter"]
    results = {}
    for case in cases:
        runs = {"baseline": [], "candidate": []}
        for index in range(rounds):
            order = [("baseline", baseline), ("candidate", candidate)]
            if index % 2:
                order.reverse()
            for name, exe in order:
                ticks = run(exe, out_dir / f"{case}-{name}-{index}", case, flags)
                runs[name].extend(ticks)
                print(f"{case} round {index} {name}: " + ", ".join(f"p50 {t['p50']} p99 {t['p99']}" for t in ticks),
                      flush=True)
        summary = {}
        for name, ticks in runs.items():
            summary[name] = {key: statistics.median(float(t[key]) for t in ticks) for key in ("p50", "p95", "p99", "max")}
            summary[name]["runs"] = ticks
        results[case] = summary
        print(f"{case} median p50/p99: baseline {summary['baseline']['p50']:.3f}/{summary['baseline']['p99']:.3f} ms, "
              f"candidate {summary['candidate']['p50']:.3f}/{summary['candidate']['p99']:.3f} ms", flush=True)
    results["high_priority"] = high_priority
    (out_dir / "comparison.json").write_text(json.dumps(results, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
