"""Seal owner hotpath evidence, keeping previous stages immutable."""
from pathlib import Path
from datetime import datetime, timezone
import hashlib
import json
import re
import shutil
import statistics

root = Path(__file__).resolve().parents[1]
out = root / 'artifacts/local/owner-hotpath'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def tests(path, count):
    value = path.read_text(encoding='utf-8-sig')
    assert f'100% tests passed, 0 tests failed out of {count}' in value, path
    return {'passed': count, 'seconds': float(re.search(r'Total Test time \(real\) =\s+([\d.]+)', value).group(1))}


def complex_css(path):
    v = load(path / 'report.json')
    assert v['comparisons'] == v['retired_imports'] == 316
    assert v['typed_frames'] == 128 and v['program_capacity_frames'] == 4
    assert v['paint_program_evictions'] > 0 and v['paint_program_reuses'] > 100 and v['typed_draws'] > 100
    assert v['worst_channel_mean'] <= 3 and v['minimum_soft_iou'] >= .75
    assert v['angle_only_draws'] == 24 and v['angle_only_uploads'] <= 3 and v['debug_warnings'] == 0
    return v


def scene_gpu(path):
    v = load(path / 'report.json')
    assert len(v['cases']) == 34 and v['captures'] == 102
    assert v['frames'] == v['retired_imports'] == 4117 and v['peak_in_flight'] == 2
    assert v['worst_channel_mean'] <= 3 and v['minimum_soft_iou'] >= .75 and v['debug_warnings'] == 0
    assert v['crop_reveal_verified'] and v['alpha_conversion_max_difference'] <= 3
    return v


def window(path):
    v = load(path / 'preview-session.json')
    assert v['frames'] == v['actual_completed_submissions'] == 160 and v['peak_in_flight'] == 2
    assert v['case_count'] == 72 and v['native_pixel_cases'] == 36 and v['debug_warning_error_count'] == 0
    assert v['card_geometry_checks'] == 288 and v['cached_full_packet_checks'] == 17
    assert v['cpu_pixel_samples'] == 1680 and v['cpu_pixel_max_difference'] <= 3
    assert v['scene_gpu_paints'] == v['scene_gpu_imports_retired']
    return {'session': v, 'owner': load(path / 'owner-work.json')}


prior_path = root / 'artifacts/local/css-middle-performance/run-metadata.json'
prior = load(prior_path)
for name, expected in prior['artifacts'].items():
    assert sha(root / name) == expected, name
for name, expected in prior['sources'].items():
    assert sha(root / 'artifacts/local/css-middle-performance/source-snapshot' / name) == expected, name
    assert sha(out / 'baseline/source' / name) == expected, name
assert sha(out / 'baseline/bin/weft_d3d11_probe.exe') == sha(root / 'artifacts/local/css-middle-performance/final/release/bin/weft_d3d11_probe.exe')

runs = {}
for name, config in [('release', 'Release'), ('debug', 'Debug'), ('gc', 'Debug')]:
    build = root / 'build' / ('windows-gc-stress' if name == 'gc' else 'windows-msvc')
    destination = out / 'final' / name
    destination.mkdir(parents=True, exist_ok=True)
    record = {'full_or_gc': tests(out / f'{name}-ctest.log', 8 if name == 'gc' else 76),
              'final_quota_scope': tests(out / f'final-{name}-scope.log', 8)}
    shutil.copytree(build / f'artifacts/complex-css-{config}/warp', destination / 'complex-css-warp', dirs_exist_ok=True)
    record['complex_warp'] = complex_css(destination / 'complex-css-warp')
    if name != 'gc':
        for source, target in [(f'scene-gallery-{config}', 'scene-software'),
                               (f'scene-gpu-{config}/warp', 'scene-gpu-warp'),
                               (f'gallery-preview-{config}/warp', 'window-warp')]:
            shutil.copytree(build / 'artifacts' / source, destination / target, dirs_exist_ok=True)
        record['scene_warp'] = scene_gpu(destination / 'scene-gpu-warp')
        record['window_warp'] = window(destination / 'window-warp')
        record['scene_hardware'] = scene_gpu(destination / 'scene-gpu-test-hardware')
        record['complex_hardware'] = complex_css(destination / 'complex-css-test-hardware')
        record['window_hardware'] = window(destination / 'gallery-window-test-hardware')
        software = load(destination / 'scene-software/native-report.json')
        geometry = load(destination / 'scene-software/geometry-diff.json')
        assert len(software['cases']) == 34 and all(c['execution'] == 'passed' for c in software['cases'])
        assert geometry['passed'] and geometry['fields'] == 1476 and geometry['maximum_css_px'] <= .3
        record['software'], record['geometry'] = software, geometry
    else:
        record['scope'] = 'DOM lifecycle, native scheduler/revisions, rAF, scene, complex GPU/software, external texture, two frame, render cache; original large gallery/window GC not rerun'
        for item in ['external-texture', 'two-frame', 'render-cache']:
            shutil.copytree(build / f'artifacts/{item}-{config}/warp', destination / item, dirs_exist_ok=True)
    names = ['weft_d3d11_probe.exe', 'weft_scene_contract_tests.exe', 'weft_owner_hotpath_tests.exe', 'weft_script_dom_tests.exe', 'weft_animation_frame_tests.exe']
    if name != 'gc':
        names += ['weft_scene_gallery_tests.exe']
    (destination / 'bin').mkdir(exist_ok=True)
    for binary in names:
        shutil.copy2(build / 'bin' / config / binary, destination / 'bin' / binary)
    runs[name] = record

benchmarks = {}
for condition in ['paired', 'controlled']:
    benchmarks[condition] = {}
    for scenario in ['css-middle', 'css-end']:
        record = {}
        for phase in ['before', 'after']:
            rows = []
            for number in [1, 2, 3]:
                path = out / condition / f'round-{number}' / phase / scenario
                b, s = load(path / 'benchmark.json'), load(path / 'preview-session.json')
                assert b['frames'] == s['actual_completed_submissions'] == 240 and b['warm_frames'] == 224
                assert not b['capture_readback'] and s['debug_warning_error_count'] == 0
                assert s['cpu_tick_ms']['samples'] == s['gpu_producer_and_ui_elapsed_ms']['samples'] == 224
                row = {'round': number, 'benchmark': b, 'session': s}
                if phase == 'after':
                    row['owner'] = load(path / 'owner-work.json')
                    assert row['owner']['snapshot_reuses'] > row['owner']['snapshots']
                    assert row['owner']['paint_program_reuses'] > row['owner']['paint_program_builds']
                if condition == 'controlled':
                    row['controls'] = load(path / 'process-controls.json')
                    assert row['controls']['affinity_mask'] == 65535 and row['controls']['priority'] == 'High'
                rows.append(row)
            values = [r['benchmark']['warm_throughput_fps'] for r in rows]
            record[phase] = {'runs': rows, 'median_run_fps': statistics.median(values), 'fps_range': [min(values), max(values)],
                             'median_cpu_p50': statistics.median(r['session']['cpu_tick_ms']['p50'] for r in rows)}
        record['ratio'] = record['after']['median_run_fps'] / record['before']['median_run_fps']
        benchmarks[condition][scenario] = record

manifest = load(root / 'tests/fixtures/crpg-gallery/manifest.json')
original_root = Path(manifest['source_root'])
drift = {'checked': len(manifest['files']), 'source_root': str(original_root), 'changed': [], 'missing': []}
for name, expected in manifest['files'].items():
    p = original_root / name
    if not p.is_file():
        drift['missing'].append(name)
    elif sha(p) != expected:
        drift['changed'].append(name)

sources = ['AGENTS.md', 'README.md', 'CMakeLists.txt', 'CMakePresets.json']
for folder in ['include/caseelephant', 'src', 'probes', 'docs', 'tests', 'examples/crpg-gallery', 'shaders', 'tools']:
    sources += [p.relative_to(root).as_posix() for p in (root / folder).rglob('*') if p.is_file() and 'node_modules' not in p.parts and '__pycache__' not in p.parts]
source_hashes = {name: sha(root / name) for name in sorted(set(sources))}
for name in source_hashes:
    p = out / 'source-snapshot' / name
    p.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(root / name, p)

artifacts = {}
for p in out.rglob('*'):
    if not p.is_file() or p.name == 'run-metadata.json':
        continue
    relative = p.relative_to(out).as_posix()
    # Build intermediates and dependency hardlinks are reproducible inputs,
    # not separate measured artifacts. The measured isolated binary is sealed.
    if relative.startswith('baseline/build/') and relative not in ['baseline/build/bin/Release/weft_d3d11_probe.exe', 'baseline/build/CMakeCache.txt']:
        continue
    if relative.startswith('baseline/source/') and relative[len('baseline/source/'):] not in prior['sources']:
        continue
    artifacts[p.relative_to(root).as_posix()] = sha(p)
record = {'format': 'owner-hotpath-seal-v1', 'recorded_utc': datetime.now(timezone.utc).isoformat(),
          'tests': runs, 'benchmarks': benchmarks, 'sources': source_hashes, 'artifacts': artifacts,
          'binaries': {k: v for k, v in artifacts.items() if k.endswith('.exe')},
          'previous_seal': {'path': prior_path.relative_to(root).as_posix(), 'sha256': sha(prior_path), 'verified_records': len(prior['sources']) + len(prior['artifacts'])},
          'baseline_provenance': 'MSVC Release rebuilt from 642 unchanged previous sealed source files; isolated WEFT_SOURCE_DIR and previous bootstrap modules; unchanged pinned dependencies/non-gallery assets hardlinked and integrity checked; previous original binary retained separately',
          'baseline_build_cache_sha256': sha(out / 'baseline/build/CMakeCache.txt'),
          'baseline_measured_executable_sha256': sha(out / 'baseline/build/bin/Release/weft_d3d11_probe.exe'),
          'environment': prior['environment'] | {'sampling': '240 fixed 60Hz owner frames, 16 warmup, 224 CPU/GPU intervals, final query drain, no readback; 3 alternating pairs per scenario/condition; no concurrent builds/tests',
                                                  'controlled_sampling': 'both child processes affinity 0xFFFF (logical CPUs 0-15), High priority set during startup; interactive window unchanged'},
          'source_drift': drift, 'native_cases': 36, 'catalogue_cases': 72, 'pending_cases': 36,
          'original_full_gallery_rendered': False, 'ultralight_comparable': False, 'mobile_verified': False,
          'limits': ['strings/full owned scenes still produced', 'single UI owner, no background UI/render threading', 'revision tracking copies full root on mutation', 'fixed short hidden throughput is not a visible game UI budget guarantee', 'GPU elapsed includes CPU submission gaps, excludes HUD/Present', 'total VRAM, cold first-frame spike, long-run latency, actual CPU frequency/scheduler telemetry unmeasured'],
          'exploration': 'all initial failed builds/tests retained; initial full Release before capacity suite retained; quota/module-owner edge fixes followed by final eight-test scopes; top/D3 sanity only one pair'}
(out / 'run-metadata.json').write_text(json.dumps(record, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(f'SEALED {len(source_hashes)} sources / {len(artifacts)} artifacts / {len(record["binaries"])} binaries; drift {len(drift["changed"])}/{len(drift["missing"])}')
