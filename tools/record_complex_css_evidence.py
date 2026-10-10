"""Seal completed complex CSS evidence while retaining previous stage seals."""
from pathlib import Path
from datetime import datetime, timezone
import hashlib
import json
import re
import shutil
import statistics

root = Path(__file__).resolve().parents[1]
out = root / 'artifacts/local/complex-css'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load(path):
    return json.loads(path.read_text(encoding='utf-8'))


def tests(path, count):
    text = path.read_text(encoding='utf-8-sig')
    assert f'100% tests passed, 0 tests failed out of {count}' in text, path
    return {'passed': count, 'seconds': float(re.search(r'Total Test time \(real\) =\s+([\d.]+)', text).group(1))}


def complex_css(path):
    record = load(path / 'report.json')
    assert record['comparisons'] == record['retired_imports'] == 88
    assert record['worst_channel_mean'] <= 3 and record['minimum_soft_iou'] >= .75
    assert record['angle_only_draws'] == 24 and record['angle_only_uploads'] <= 3
    assert record['gpu_projective_draws_per_painter_max'] > 10
    assert record['uniform_border_draws_per_painter_max'] > 0 and record['fallback_border_draws_per_painter_max'] > 0
    assert record['debug_warnings'] == 0 and record['software_target_switches'] == 8
    return record


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
    assert record['case_count'] == 72 and record['native_pixel_cases'] == 36
    assert record['peak_in_flight'] == 2 and record['debug_warning_error_count'] == 0
    assert record['card_geometry_checks'] == 288 and record['cached_full_packet_checks'] == 17
    assert record['cpu_pixel_samples'] == 1680 and record['cpu_pixel_max_difference'] <= 3
    assert record['scene_gpu_paints'] == record['scene_gpu_imports_retired']
    return record


runs, binaries = {}, {}
for name, config in (('release', 'Release'), ('debug', 'Debug'), ('gc', 'Debug')):
    build = root / 'build' / ('windows-gc-stress' if name == 'gc' else 'windows-msvc')
    target = out / 'final' / name
    target.mkdir(parents=True, exist_ok=True)
    runs[name] = tests(out / f'final-{name}-ctest.log', 8 if name == 'gc' else 75)
    contract_name = 'gc' if name == 'gc' else config
    runs[name]['final_test_variable_cleanup'] = tests(out / f'final-{contract_name}-contract-ctest.log', 1)
    if name == 'gc':
        runs[name]['final_angle_reuse_extension'] = tests(out / 'final-gc-cache-proof-ctest.log', 1)
        runs[name]['scope'] = 'scene contracts + original software/geometry + original GPU + complex CSS + external textures + two-frame pipeline + render cache; gallery window not rerun in GC'
    for origin, dest in ((f'scene-gallery-{config}', 'scene-software'),
                         (f'scene-gpu-{config}/warp', 'scene-gpu-warp'),
                         (f'complex-css-{config}/warp', 'complex-css-warp')):
        shutil.copytree(build / 'artifacts' / origin, target / dest, dirs_exist_ok=True)
    runs[name]['gpu_warp'] = gpu_scene(target / 'scene-gpu-warp')
    runs[name]['complex_warp'] = complex_css(target / 'complex-css-warp')
    software = load(target / 'scene-software/native-report.json')
    assert len(software['cases']) == 34 and all(c['execution'] == 'passed' for c in software['cases'])
    runs[name]['software'] = software
    geometry = load(target / 'scene-software/geometry-diff.json')
    assert geometry['passed'] and geometry['fields'] == 1476 and geometry['maximum_css_px'] <= .3
    runs[name]['geometry'] = geometry
    if name != 'gc':
        shutil.copytree(build / 'artifacts' / f'gallery-preview-{config}/warp', target / 'window-warp', dirs_exist_ok=True)
        runs[name]['window_warp'] = window(target / 'window-warp')
        runs[name]['gpu_hardware'] = gpu_scene(target / 'scene-gpu-test-hardware')
        runs[name]['complex_hardware'] = complex_css(target / 'complex-css-test-hardware')
        runs[name]['window_hardware'] = window(target / 'gallery-window-test-hardware')
    for filename in ('ce_d3d11_probe.exe', 'ce_scene_contract_tests.exe', 'ce_scene_gallery_tests.exe'):
        destination = target / 'bin' / filename
        destination.parent.mkdir(exist_ok=True)
        shutil.copy2(build / 'bin' / config / filename, destination)
        binaries[destination.relative_to(root).as_posix()] = sha(destination)

benchmarks = {}
for scenario in ('css-top', 'css-middle', 'css-end', 'd3'):
    pair = {}
    for phase in ('before-repeat', 'after'):
        path = out / phase / scenario
        record, session = load(path / 'benchmark.json'), load(path / 'preview-session.json')
        assert record['frames'] == session['actual_completed_submissions'] == 240
        assert record['warm_frames'] == 224 and not record['capture_readback']
        assert session['cpu_tick_ms']['samples'] == session['gpu_producer_and_ui_elapsed_ms']['samples'] == 224
        assert session['debug_warning_error_count'] == 0
        pair[phase] = {'benchmark': record, 'session': session}
    pair['throughput_ratio'] = pair['after']['benchmark']['warm_throughput_fps'] / pair['before-repeat']['benchmark']['warm_throughput_fps']
    benchmarks[scenario] = pair

paired = {}
for scenario in ('css-top', 'css-middle', 'css-end', 'd3'):
    records = {}
    for phase in ('before', 'after'):
        rows = []
        for number in (1, 2, 3):
            path = out / 'paired' / f'round-{number}' / phase / scenario
            benchmark, session = load(path / 'benchmark.json'), load(path / 'preview-session.json')
            assert benchmark['frames'] == session['actual_completed_submissions'] == 240
            assert benchmark['warm_frames'] == 224 and not benchmark['capture_readback']
            assert session['cpu_tick_ms']['samples'] == session['gpu_producer_and_ui_elapsed_ms']['samples'] == 224
            assert session['debug_warning_error_count'] == 0
            rows.append({'round': number, 'benchmark': benchmark, 'session': session})
        values = [r['benchmark']['warm_throughput_fps'] for r in rows]
        records[phase] = {'runs': rows, 'median_run_fps': statistics.median(values), 'fps_range': [min(values), max(values)],
                          'median_run_cpu_p50_ms': statistics.median(r['session']['cpu_tick_ms']['p50'] for r in rows)}
    records['median_throughput_ratio'] = records['after']['median_run_fps'] / records['before']['median_run_fps']
    paired[scenario] = records

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
artifacts = {p.relative_to(root).as_posix(): sha(p) for directory in ('final', 'before', 'before-repeat', 'after', 'paired', 'source-snapshot')
             for p in (out / directory).rglob('*') if p.is_file()}
artifacts.update({p.relative_to(root).as_posix(): sha(p) for p in out.glob('*.log')})
assert sha(out / 'before/source-snapshot/shaders/m0_probe.hlsl') == sha(root / 'shaders/m0_probe.hlsl')
record = {'format': 'complex-css-performance-seal-v1', 'recorded_utc': datetime.now(timezone.utc).isoformat(),
          'tests': runs, 'initial_benchmarks': benchmarks, 'paired_benchmarks': paired, 'sources': source_hashes, 'binaries': binaries, 'artifacts': artifacts,
          'environment': {'os': 'Windows 11 26200', 'cpu': 'AMD Ryzen 9 9950X3D', 'gpu': 'NVIDIA GeForce RTX 5090',
                          'gpu_driver': '32.0.16.1062', 'compiler': 'MSVC 19.42 / C++20', 'performance_configuration': 'Release + D3D11 debug layer',
                          'surface': [3840, 2160], 'logical_shell': [1920, 1080], 'scene_scale': 2,
                          'css_body': [387, 288], 'd3_body': [384, 384], 'canvas_backing': [800, 600],
                          'fonts': 'same previous fixed shell fonts and OS scene fonts; original reference manifest retains hashes',
                          'nodes_across_34_scenes_at_mount': 560, 'live_vms': 34,
                          'sampling': '240 fixed 60Hz frames, 16 warmup, 224 intervals; final query drain; after correctness tasks, sequential before-repeat and after; no PNG/readback',
                          'repeated_sampling': '3 paired rounds, before-after in rounds 1/3, after-before in round 2; per-scenario median of run throughput and reported CPU P50, not pooled-frame percentiles',
                          'gpu_elapsed_limit': 'same producer+UI elapsed range in both binaries, including CPU scheduling gaps, excluding HUD/Present GPU work',
                          'presentation': 'hidden native 4K window; actual completed submission throughput, not visible long-duration FPS',
                          'unmeasured': ['total VRAM', 'cold first frame spike', 'long-duration tail latency']},
          'baseline_provenance': 'preserved pre-optimization executable with only CSS-end viewport benchmark added; reconstructed previous sealed source-snapshot overlaid with saved baseline inputs',
          'previous_seal': {'path': 'artifacts/local/gallery-performance/run-metadata.json', 'sha256': sha(root / 'artifacts/local/gallery-performance/run-metadata.json')},
          'native_cases': 36, 'catalogue_cases': 72, 'pending_cases': 36,
          'unresolved_performance': 'D3 paired median throughput decreased, ranges overlap; initial pair increased; no core/frequency/scheduler telemetry, cannot exclude regression or conclude noise alone',
          'original_full_gallery_rendered': False, 'ultralight_comparable': False, 'mobile_verified': False,
          'source_drift': drift, 'excluded_exploration': 'iteration/ and initial before/ benchmark snapshots are not final performance claims; all initial comparison before-repeat/after records retained'}
(out / 'run-metadata.json').write_text(json.dumps(record, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(f'SEALED {len(source_hashes)} sources / {len(artifacts)} artifacts / {len(binaries)} binaries; original drift {len(drift["changed"])}/{len(drift["missing"])}')
