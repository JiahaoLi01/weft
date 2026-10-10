"""Print the live.json results of gallery live benchmarks (--gallery-live-benchmark-hardware).

    python tools/summarize_gallery_live.py <run folder>...

Each line: present FPS and owner/tracked update rates, then P50/P99/max of the present interval, render CPU,
owner active time, tracked update interval and input-to-render latency, in milliseconds.
"""
import json
import sys
from pathlib import Path


def tails(sample):
    if not sample.get("samples"):
        return "-"
    return f"{sample['p50']:.1f}/{sample['p99']:.1f}/{sample['max']:.1f}"


def main(folders):
    if not folders:
        print(__doc__)
        return 2
    print("scenario        FPS  owner/s  updates/s | P50/P99/max ms: present | render CPU | owner | "
          "update | input->render")
    for folder in folders:
        path = Path(folder) / "live.json"
        if not path.exists():
            print(f"{Path(folder).name}: no live.json (run failed?)")
            continue
        d = json.loads(path.read_text(encoding="utf-8"))
        print(f"{d['scenario']:14s} {d['present_fps']:4.0f}  {d['owner_publications_per_s']:7.0f}  "
              f"{d['tracked_updates_per_s']:9.0f} | {tails(d['present_interval_ms'])} | {tails(d['render_cpu_ms'])} | "
              f"{tails(d['owner_active_ms'])} | {tails(d['tracked_update_interval_ms'])} | "
              f"{tails(d['input_to_render_pickup_ms'])}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
