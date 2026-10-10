"""Validate and summarize the opt-in native gallery owner/render traces.

No upload or third-party module is used. Run after the preview has exited.
"""
import argparse
import json
import math
from collections import defaultdict
from pathlib import Path


def distribution(values):
    values = sorted(values)
    if not values:
        return {"samples": 0}
    return {"samples": len(values), "p50": values[int((len(values) - 1) * .5)],
            "p95": values[int((len(values) - 1) * .95)],
            "p99": values[int((len(values) - 1) * .99)], "max": values[-1]}


def analyze(folder):
    events, groups, drops, sampled = [], defaultdict(list), {}, {}
    for filename, tid in [("owner-trace.json", 1), ("render-trace.json", 2)]:
        trace = json.loads((folder / filename).read_text(encoding="utf-8"))
        if trace.get("clock") != "steady_clock_us":
            raise ValueError(f"{filename}: requires the shared, unrounded clock format")
        drops[filename] = trace["dropped_events"]
        sampled[filename] = trace
        for event in trace["traceEvents"]:
            if (event["ph"] != "X" or event["tid"] != tid or
                    event["ts"] == 0 or
                    any(not math.isfinite(event[key]) or event[key] < 0 for key in ("ts", "dur"))):
                raise ValueError(f"{filename}: invalid event")
            events.append(event)
            groups[(tid, event["name"])].append(event["dur"] / 1000)
    work = json.loads((folder / "thread-work.json").read_text(encoding="utf-8"))
    if work.get("profile_run_id") is not None and any(trace.get("run_id") != work["profile_run_id"] for trace in sampled.values()):
        raise ValueError("Trace files belong to another run; enable Profiler/F8 and exit before analyzing this directory")
    if work["accepted_inputs"] != work["processed_inputs"] + work["coalesced_inputs"]:
        raise ValueError("settled input accounting mismatch")
    input_events = [event for event in events if event["name"].startswith("input scene=")]
    owner = sampled["owner-trace.json"]
    expected_inputs = owner.get("sampled_inputs", work["processed_inputs"])
    expected_publications = owner.get("sampled_publications", work["published"])
    if not drops["owner-trace.json"] and len(input_events) != expected_inputs:
        raise ValueError("processed inputs missing from trace")
    if not drops["owner-trace.json"] and len(groups[(1, "owner publication")]) != expected_publications:
        raise ValueError("publications missing from trace")
    summary = {"clock": "steady_clock_us", "dropped_events": drops,
               "sampled_inputs": expected_inputs, "sampled_publications": expected_publications,
               "complete_run_coverage": not any(drops.values()) and expected_inputs == work["processed_inputs"] and expected_publications == work["published"],
               "accepted_inputs": work["accepted_inputs"], "processed_inputs": work["processed_inputs"],
               "coalesced_inputs": work["coalesced_inputs"],
               "input_queue_age_ms": distribution([event["args"]["queue_age_ms"] for event in input_events]),
               "events": [{"thread": "owner" if tid == 1 else "render", "name": name,
                           "duration_ms": distribution(values), "total_ms": sum(values)}
                          for (tid, name), values in sorted(groups.items())]}
    events.sort(key=lambda event: event["ts"])
    (folder / "combined-trace.json").write_text(json.dumps({"traceEvents": events,
        "clock": "steady_clock_us", "dropped_events": drops}, ensure_ascii=False), encoding="utf-8")
    (folder / "profile-summary.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf-8")
    return summary


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path)
    summary = analyze(parser.parse_args().folder)
    print("Input queue age (ms):", summary["input_queue_age_ms"])
    for thread in ("owner", "render"):
        rows = [row for row in summary["events"] if row["thread"] == thread]
        for row in sorted(rows, key=lambda row: row["duration_ms"].get("p99", 0), reverse=True)[:6]:
            print(thread, row["name"], row["duration_ms"])
    coverage = "complete run" if summary["complete_run_coverage"] else "sampled window (profiling toggled or ring overwritten)"
    print(f"PASS trace clocks, {coverage}, settled input accounting; combined-trace.json written")
