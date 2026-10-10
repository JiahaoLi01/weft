"""Seal completed gallery optimization evidence without replacing earlier seals."""
from pathlib import Path
from datetime import datetime, timezone
import hashlib
import json
import re
import shutil

root = Path(__file__).resolve().parents[1]
out = root / 'artifacts/local/gallery-performance'

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def load(path):
    return json.loads(path.read_text(encoding='utf-8'))

def tests(path, count):
    text = path.read_text(encoding='utf-8-sig')
    assert f'100% tests passed, 0 tests failed out of {count}' in text, path
    return {'passed': count, 'seconds': float(re.search(r'Total Test time \(real\) =\s+([\d.]+)', text).group(1))}

def gpu_scene(path):
    record = load(path / 'report.json')
    assert len(record['cases']) == 34 and record['captures'] == 102 and record['frames'] == 4117
    assert record['worst_channel_mean'] <= 3 and record['minimum_soft_iou'] >= .75
    assert record['retired_imports'] == 4117 and record['peak_in_flight'] == 2
    assert record['alpha_conversion_max_difference'] <= 3 and record['crop_reveal_verified']
    assert record['debug_warnings'] == 0
    return record

def window(path):
    record = load(path / 'preview-session.json')
    assert record['frames'] == record['actual_completed_submissions'] == 160
    assert record['native_pixel_cases'] == 36 and record['case_count'] == 72
    assert record['peak_in_flight'] == 2 and record['debug_warning_error_count'] == 0
    assert record['cached_full_packet_checks'] == 17 and record['card_geometry_checks'] == 288
    assert record['cpu_pixel_samples'] == 1680 and record['cpu_pixel_max_difference'] <= 3
    assert record['scene_gpu_pool_slots'] <= 102 and record['scene_gpu_pool_reuses'] > 0
    assert record['scene_gpu_paints'] == record['scene_gpu_imports_retired']
    return record

runs, binaries = {}, {}
for name, config in (('release', 'Release'), ('debug', 'Debug'), ('gc', 'Debug')):
    build = root / 'build' / ('windows-gc-stress' if name == 'gc' else 'windows-msvc')
    target = out / 'final' / name
    target.mkdir(parents=True, exist_ok=True)
    if name != 'gc':
        runs[name] = tests(out / f'final-{name}-ctest.log', 74)
    else:
        initial = (out / 'initial-concurrent-gc-ctest.log').read_text(encoding='utf-8-sig')
        assert '88% tests passed, 1 tests failed out of 8' in initial
        runs[name] = {'passed_distinct_checks': 8, 'initial_parallel': {'passed': 7, 'failed': 1, 'seconds': 269.16},
                      'serial_retry': tests(out / 'final-gc-gallery-preview-ctest.log', 1),
                      'scope': 'scene contracts, 34 CPU originals/geometry, 34 GPU originals, WARP gallery window, external texture, two-frame pipeline, render cache',
                      'retained_failure': 'initial-concurrent-gc-ctest.log',
                      'unchanged_execution_budgets': True}
    for origin, dest in ((f'scene-gallery-{config}', 'scene-software'),
                         (f'scene-gpu-{config}/warp', 'scene-gpu-warp'),
                         (f'gallery-preview-{config}/warp', 'window-warp')):
        shutil.copytree(build / 'artifacts' / origin, target / dest, dirs_exist_ok=True)
    runs[name]['gpu_warp'] = gpu_scene(target / 'scene-gpu-warp')
    runs[name]['window_warp'] = window(target / 'window-warp')
    software = load(target / 'scene-software/native-report.json')
    assert len(software['cases']) == 34 and all(c['execution'] == 'passed' for c in software['cases'])
    runs[name]['software'] = software
    geometry = load(target / 'scene-software/geometry-diff.json')
    assert geometry['passed'] and geometry['fields'] == 1476 and geometry['maximum_css_px'] <= .3
    runs[name]['geometry'] = geometry
    if name != 'gc':
        runs[name]['gpu_hardware'] = gpu_scene(target / 'scene-gpu-hardware')
        runs[name]['window_hardware'] = window(target / 'window-hardware')
    for filename in ('ce_d3d11_probe.exe', 'ce_scene_contract_tests.exe', 'ce_scene_gallery_tests.exe'):
        destination = target / 'bin' / filename
        destination.parent.mkdir(exist_ok=True)
        shutil.copy2(build / 'bin' / config / filename, destination)
        binaries[destination.relative_to(root).as_posix()] = sha(destination)

benchmarks = {}
for scenario in ('css-top', 'css-middle', 'd3'):
    pair = {}
    for phase in ('baseline-repeat', 'optimized'):
        path = out / phase / scenario
        record = load(path / 'benchmark.json')
        session = load(path / 'preview-session.json')
        assert record['frames'] == session['actual_completed_submissions'] == 240
        assert record['warm_frames'] == 224 and not record['capture_readback']
        assert session['cpu_tick_ms']['samples'] == session['gpu_producer_and_ui_elapsed_ms' if phase == 'optimized' else 'gpu_ui_ms']['samples'] == 224
        assert session['debug_warning_error_count'] == 0
        pair[phase] = {'benchmark': record, 'session': session}
    pair['throughput_ratio'] = pair['optimized']['benchmark']['warm_throughput_fps'] / pair['baseline-repeat']['benchmark']['warm_throughput_fps']
    benchmarks[scenario] = pair

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
artifacts = {p.relative_to(root).as_posix(): sha(p) for directory in ('final', 'baseline-repeat', 'optimized', 'source-snapshot', 'baseline')
             for p in (out / directory).rglob('*') if p.is_file()}
artifacts.update({p.relative_to(root).as_posix(): sha(p) for p in out.glob('*.log')})
record = {'format': 'gallery-performance-seal-v1', 'recorded_utc': datetime.now(timezone.utc).isoformat(),
          'tests': runs, 'benchmarks': benchmarks, 'sources': source_hashes, 'binaries': binaries, 'artifacts': artifacts,
          'environment': {'os': 'Windows 11 26200', 'cpu': 'AMD Ryzen 9 9950X3D', 'gpu': 'NVIDIA GeForce RTX 5090',
                          'gpu_driver': '32.0.16.1062', 'compiler': 'MSVC 19.42 / C++20', 'performance_configuration': 'Release + D3D11 debug layer',
                          'surface': [3840, 2160], 'logical_shell': [1920, 1080], 'scene_scale': 2,
                          'css_body': [387, 288], 'd3_body': [384, 384], 'canvas_backing': [800, 600],
                          'fonts': 'same fixed shell fonts + OS system-ui / Arial / Consolas / Emoji; original reference manifest retains hashes',
                          'nodes_across_34_scenes_at_mount': 560, 'live_vms': 34,
                          'sampling': '240 fixed 60Hz clock frames, 16 warmup, 224 warm stage samples; after all correctness jobs, sequential scenarios, no PNG/readback',
                          'baseline_sampling_detail': 'profiled pre-optimization binary; baseline-repeat retains original 223-interval throughput formula, optimized uses exact 224-interval formula; both include final real-query drain',
                          'shader': 'same final shader file for repeat before/after; new D2D conversion material is unused by pre-optimization binary',
                          'gpu_elapsed_limit': 'baseline UI draw only; optimized producer+UI elapsed range includes owner CPU scheduling gaps; both exclude HUD/Present',
                          'presentation': 'hidden native 4K window; throughput of actual completed submissions, not a visible long-duration user test'},
          'native_cases': 36, 'catalogue_cases': 72, 'pending_cases': 36,
          'original_full_gallery_rendered': False, 'ultralight_comparable': False, 'mobile_verified': False,
          'source_drift': drift,
          'excluded_exploration': 'iteration-* and alpha-debug; failed initial concurrent GC run is retained separately'}
(out / 'run-metadata.json').write_text(json.dumps(record, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(f'SEALED {len(source_hashes)} sources / {len(artifacts)} artifacts / {len(binaries)} binaries; original drift {len(drift["changed"])}/{len(drift["missing"])}')
