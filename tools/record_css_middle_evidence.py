"""Seal CSS middle optimization; preserve all prior seals and all benchmark runs."""
from pathlib import Path
from datetime import datetime, timezone
import hashlib
import json
import re
import shutil
import statistics

root = Path(__file__).resolve().parents[1]
out = root / 'artifacts/local/css-middle-performance'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def tests(path, count):
    content = path.read_text(encoding='utf-8-sig')
    assert f'100% tests passed, 0 tests failed out of {count}' in content, path
    return {'passed': count, 'seconds': float(re.search(r'Total Test time \(real\) =\s+([\d.]+)', content).group(1))}


def complex_css(path):
    record = load(path / 'report.json')
    assert record['comparisons'] == record['retired_imports'] == 184
    assert record['border_mutations'] == 96
    assert record['border_geometry_builds'] > 256 and record['border_geometry_reuses'] > 24
    assert record['worst_channel_mean'] <= 3 and record['minimum_soft_iou'] >= .75
    assert record['angle_only_draws'] == 24 and record['angle_only_uploads'] <= 3
    assert record['debug_warnings'] == 0 and record['software_target_switches'] == 8
    return record


def scene_gpu(path):
    record = load(path / 'report.json')
    assert len(record['cases']) == 34 and record['captures'] == 102
    assert record['frames'] == record['retired_imports'] == 4117
    assert record['worst_channel_mean'] <= 3 and record['minimum_soft_iou'] >= .75
    assert record['peak_in_flight'] == 2 and record['debug_warnings'] == 0
    assert record['crop_reveal_verified'] and record['alpha_conversion_max_difference'] <= 3
    return record


def window(path):
    record = load(path / 'preview-session.json')
    assert record['frames'] == record['actual_completed_submissions'] == 160
    assert record['case_count'] == 72 and record['native_pixel_cases'] == 36
    assert record['peak_in_flight'] == 2 and record['debug_warning_error_count'] == 0
    assert record['card_geometry_checks'] == 288 and record['cached_full_packet_checks'] == 17
    assert record['cpu_pixel_samples'] == 1680 and record['cpu_pixel_max_difference'] <= 3
    assert record['scene_gpu_paints'] == record['scene_gpu_imports_retired']
    return record


runs = {}
for name, config in (('release', 'Release'), ('debug', 'Debug'), ('gc', 'Debug')):
    build = root / 'build' / ('windows-gc-stress' if name == 'gc' else 'windows-msvc')
    target = out / 'final' / name
    target.mkdir(parents=True, exist_ok=True)
    runs[name] = tests(out / f'{name}-ctest.log', 5 if name == 'gc' else 75)
    shutil.copytree(build / f'artifacts/complex-css-{config}/warp', target / 'complex-css-warp', dirs_exist_ok=True)
    runs[name]['complex_warp'] = complex_css(target / 'complex-css-warp')
    if name != 'gc':
        for source, destination in ((f'scene-gallery-{config}', 'scene-software'),
                                    (f'scene-gpu-{config}/warp', 'scene-gpu-warp'),
                                    (f'gallery-preview-{config}/warp', 'window-warp')):
            shutil.copytree(build / 'artifacts' / source, target / destination, dirs_exist_ok=True)
        runs[name]['scene_warp'] = scene_gpu(target / 'scene-gpu-warp')
        runs[name]['window_warp'] = window(target / 'window-warp')
        runs[name]['scene_hardware'] = scene_gpu(target / 'scene-gpu-test-hardware')
        runs[name]['complex_hardware'] = complex_css(target / 'complex-css-test-hardware')
        runs[name]['window_hardware'] = window(target / 'gallery-window-test-hardware')
        software = load(target / 'scene-software/native-report.json')
        assert len(software['cases']) == 34 and all(c['execution'] == 'passed' for c in software['cases'])
        geometry = load(target / 'scene-software/geometry-diff.json')
        assert geometry['passed'] and geometry['fields'] == 1476 and geometry['maximum_css_px'] <= .3
        runs[name]['software'], runs[name]['geometry'] = software, geometry
    else:
        runs[name]['scope'] = 'scene_contract + complex CSS GPU/software + external textures + two-frame pipeline + render cache; original large gallery and window GC not rerun'
        for source in ('external-texture', 'two-frame', 'render-cache'):
            shutil.copytree(build / f'artifacts/{source}-{config}/warp', target / source, dirs_exist_ok=True)
    filenames = ['ce_d3d11_probe.exe', 'ce_scene_contract_tests.exe']
    if name != 'gc':
        filenames.append('ce_scene_gallery_tests.exe')
    for filename in filenames:
        destination = target / 'bin' / filename
        destination.parent.mkdir(exist_ok=True)
        shutil.copy2(build / 'bin' / config / filename, destination)


benchmarks = {}
for condition in ('paired', 'controlled'):
    benchmarks[condition] = {}
    for scenario in ('css-middle', 'css-end'):
        record = {}
        for phase in ('before', 'after'):
            rows = []
            for number in (1, 2, 3):
                path = out / condition / f'round-{number}' / phase / scenario
                benchmark, session = load(path / 'benchmark.json'), load(path / 'preview-session.json')
                assert benchmark['frames'] == session['actual_completed_submissions'] == 240
                assert benchmark['warm_frames'] == 224 and not benchmark['capture_readback']
                assert session['cpu_tick_ms']['samples'] == session['gpu_producer_and_ui_elapsed_ms']['samples'] == 224
                assert session['debug_warning_error_count'] == 0
                row = {'round': number, 'benchmark': benchmark, 'session': session}
                if condition == 'controlled':
                    controls = load(path / 'process-controls.json')
                    assert controls['affinity_mask'] == 65535 and controls['priority'] == 'High'
                    row['process_controls'] = controls
                rows.append(row)
            values = [r['benchmark']['warm_throughput_fps'] for r in rows]
            record[phase] = {'runs': rows, 'median_run_fps': statistics.median(values), 'fps_range': [min(values), max(values)],
                             'median_run_cpu_p50_ms': statistics.median(r['session']['cpu_tick_ms']['p50'] for r in rows)}
        record['median_throughput_ratio'] = record['after']['median_run_fps'] / record['before']['median_run_fps']
        benchmarks[condition][scenario] = record

prior_path = root / 'artifacts/local/complex-css/run-metadata.json'
prior = load(prior_path)
# Verify the immutable previous artifacts. Live source paths deliberately change
# in a subsequent stage, so verify their saved snapshot instead.
for name, expected in prior['artifacts'].items():
    assert sha(root / name) == expected, name
for name, expected in prior['sources'].items():
    source = root / 'artifacts/local/complex-css/source-snapshot' / name
    assert sha(source) == expected, name
    destination = out / 'baseline/source' / name
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        assert sha(destination) == expected, name
    else:
        shutil.copy2(source, destination)
assert sha(out / 'baseline/bin/ce_d3d11_probe.exe') == sha(root / 'artifacts/local/complex-css/final/release/bin/ce_d3d11_probe.exe')

original = load(root / 'tests/fixtures/crpg-gallery/manifest.json')
source_root = Path(original['source_root'])
drift = {'checked': len(original['files']), 'source_root': str(source_root), 'changed': [], 'missing': []}
for name, expected in original['files'].items():
    path = source_root / name
    if not path.is_file():
        drift['missing'].append(name)
    elif sha(path) != expected:
        drift['changed'].append(name)

sources = ['AGENTS.md', 'README.md', 'CMakeLists.txt', 'CMakePresets.json']
for folder in ('include/caseelephant', 'src', 'probes', 'docs', 'tests', 'examples/crpg-gallery', 'shaders', 'tools'):
    sources.extend(p.relative_to(root).as_posix() for p in (root / folder).rglob('*')
                   if p.is_file() and 'node_modules' not in p.parts and '__pycache__' not in p.parts)
source_hashes = {name: sha(root / name) for name in sorted(set(sources))}
for name in source_hashes:
    destination = out / 'source-snapshot' / name
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(root / name, destination)
artifacts = {p.relative_to(root).as_posix(): sha(p) for p in out.rglob('*') if p.is_file() and p.name != 'run-metadata.json'}
binaries = {name: digest for name, digest in artifacts.items() if name.endswith('.exe')}
record = {'format': 'css-middle-performance-seal-v1', 'recorded_utc': datetime.now(timezone.utc).isoformat(),
          'tests': runs, 'benchmarks': benchmarks, 'sources': source_hashes, 'binaries': binaries, 'artifacts': artifacts,
          'environment': prior['environment'] | {
              'sampling': '240 fixed 60Hz owner frames; 16 warmup, 224 CPU/GPU intervals; completed query drain; no PNG/readback',
              'repeated_sampling': '3 alternating pairs per scene/condition; before-after in rounds 1/3, after-before in round 2; report median of run throughput, not pooled samples',
              'controlled_sampling': 'only child processes: affinity 0xFFFF (logical CPUs 0-15), High priority set during 72-case startup; same controls both versions; no runtime/application scheduling changes',
              'unmeasured': ['actual core/frequency/scheduler telemetry', 'total VRAM', 'cold first-frame spike', 'long-duration latency']},
          'baseline_provenance': 'unmodified executable and complete sealed source snapshot from immediately preceding complex CSS stage',
          'previous_seal': {'path': prior_path.relative_to(root).as_posix(), 'sha256': sha(prior_path)},
          'source_drift': drift, 'native_cases': 36, 'catalogue_cases': 72, 'pending_cases': 36,
          'original_full_gallery_rendered': False, 'ultralight_comparable': False, 'mobile_verified': False,
          'performance_limit': 'uncontrolled middle median regressed and stages changed together; controlled pairs all improved but affinity/priority condition is not default visible-window performance; no blanket FPS guarantee',
          'exploration': 'baseline/initial and iteration locate hotspots only; sanity top/D3 has one pair and is not a stability claim; every uncontrolled and controlled pair retained'}
(out / 'run-metadata.json').write_text(json.dumps(record, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(f'SEALED {len(source_hashes)} sources / {len(artifacts)} artifacts / {len(binaries)} binaries; original drift {len(drift["changed"])}/{len(drift["missing"])}')
