"""Hidden 4K CaseCheetah flow window benchmark on D3D11, on Vulkan, and on Vulkan without its validation layer, from
one Release probe. The modes alternate run by run so that drift and heat spread over all of them.

    python tools/benchmark_flow_window_apis.py <weft_d3d11_probe.exe> <output_dir> [runs] [--no-vsync]

Each run writes <output_dir>/<api>-<run>/profile.json (120 wheel frames after 8 warmup frames). Prints the median of the
runs' frame rates and of their P50, P99 and maximum per column; no averages.
"""
import json
import os
import subprocess
import sys

MODES = [('d3d11', ['--cheetah-flow-window-perf-hardware']),
         ('vulkan', ['--cheetah-flow-window-perf-vulkan']),
         ('vulkan-no-validation', ['--cheetah-flow-window-perf-vulkan', '--no-validation'])]
COLUMNS = ['total_ms', 'layout_ms', 'prepare_ms', 'encode_ms', 'submit_present_ms', 'completion_wait_ms', 'gpu_ui_ms']


def median(values):
    values = sorted(values)
    return values[len(values) // 2]


def main():
    arguments = [a for a in sys.argv[1:] if a != '--no-vsync']
    extra = ['--no-vsync'] if len(arguments) != len(sys.argv) - 1 else []
    if len(arguments) not in (2, 3):
        print(__doc__)
        return 2
    exe = os.path.abspath(arguments[0])
    out = os.path.abspath(arguments[1])
    runs = int(arguments[2]) if len(arguments) == 3 else 3
    results = {name: [] for name, _ in MODES}
    for run in range(runs):
        for name, mode in MODES:
            folder = os.path.join(out, f'{name}-{run}')
            done = subprocess.run([exe, mode[0], folder, *mode[1:], *extra], capture_output=True, text=True,
                                  encoding='utf-8', errors='replace', timeout=900)
            if done.returncode == 77:
                print(name, 'skipped:', done.stdout.strip())
                continue
            if done.returncode:
                print(name, run, 'failed:', done.stdout[-400:], done.stderr[-400:])
                return 1
            with open(os.path.join(folder, 'profile.json'), encoding='utf-8') as f:
                profile = json.load(f)
            results[name].append(profile)
            print(f'{name} run {run}: {profile["completed_frames_per_second"]:.1f} fps', flush=True)
    for name, profiles in results.items():
        if not profiles:
            continue
        rates = sorted(p['completed_frames_per_second'] for p in profiles)
        print(f'{name}: {median(rates):.1f} fps median of {len(rates)} runs ({", ".join(f"{r:.1f}" for r in rates)})')
        for column in COLUMNS:
            cells = [f'{stat} {median(p["timings"][column][stat] for p in profiles):.3f}'
                     for stat in ('p50', 'p99', 'max')]
            print(f'  {column}: ' + ', '.join(cells))
        cold = median(p['cold_first_frame_ms'] for p in profiles)
        peak = median(p['peak_working_set_bytes'] for p in profiles) / 2 ** 20
        print(f'  cold first frame {cold:.1f} ms, peak working set {peak:.1f} MiB')
    return 0


if __name__ == '__main__':
    sys.exit(main())
