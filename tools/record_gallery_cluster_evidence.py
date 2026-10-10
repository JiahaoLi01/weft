"""Seal the shared gallery cluster, retaining and verifying earlier evidence."""
from pathlib import Path
from datetime import datetime, timezone
import hashlib
import json
import re
import shutil
import struct

root = Path(__file__).resolve().parents[1]
out = root / 'artifacts/local/gallery-cluster'


def load(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def test_run(path, count):
    text = path.read_text(encoding='utf-8', errors='replace')
    assert re.search(r'100% tests passed, 0 tests failed out of ' + str(count) + r'\b', text), path
    return {'count': count, 'seconds': float(re.search(r'Total Test time \(real\) =\s*([0-9.]+)', text)[1])}


def gpu_report(path):
    value = load(path / 'report.json')
    assert value['case_count'] == 30 and len(value['captures']) == 210
    assert value['input_count'] == 50
    assert value['submissions'] == value['completions'] == 630
    assert value['native_passes'] > 0 and value['fallback_passes'] > 0
    assert value['debug_warning_error_count'] == 0 and value['max_mean'] <= 3
    return value


def window_report(path):
    value = load(path / 'preview-session.json')
    assert value['case_count'] == 72 and value['native_pixel_cases'] == 66
    assert value['frames'] == value['actual_completed_submissions'] == 160
    assert value['card_geometry_checks'] == 288 and value['cached_full_packet_checks'] == 17
    assert value['cpu_pixel_samples'] == 1680 and value['cpu_pixel_max_difference'] <= 3
    assert value['peak_in_flight'] == 2 and value['debug_warning_error_count'] == 0
    thread = load(path / 'thread-work.json')
    assert thread['accepted_inputs'] == thread['processed_inputs'] == 16
    return {'session': value, 'thread': thread, 'render': load(path / 'render-work.json')}


assert not (out / 'run-metadata.json').exists(), 'Evidence is already sealed; use a new stage directory.'
prior_path = root / 'artifacts/local/render-stages/run-metadata.json'
prior = load(prior_path)
for name, expected in prior['artifacts'].items():
    assert sha(root / name) == expected, name
for name, expected in prior['sources'].items():
    assert sha(prior_path.parent / 'source-snapshot' / name) == expected, name

runs = {}
for name, config in [('release', 'Release'), ('debug', 'Debug')]:
    build = root / 'build/windows-msvc'
    destination = out / 'final' / name
    destination.mkdir(parents=True, exist_ok=True)
    test_log = root / f'artifacts/local/gallery-cluster-{name}-tests.log'
    if name == 'release':
        runs[name] = {'full': test_run(test_log, 86)}
    else:
        text = test_log.read_text(encoding='utf-8', errors='replace')
        assert '98% tests passed, 2 tests failed out of 86' in text
        assert 'kline-00 FAIL WEFT_VIEW_FAULTED: InternalError: interrupted' in text
        failures = text.split('The following tests FAILED:')[1]
        assert re.findall(r'\d+ - (\w+) \(', failures) == ['scene_cluster_browser_contract', 'scene_cluster_original']
        runs[name] = {'full_attempt': {'count': 86, 'passed': 84, 'failure': 'K-Line all-SVG diagnostic geometry reads exceeded per-eval 30s limit; dependent audit incomplete'}, 'accepted_test_count': 86}
    scope_log = root / f'artifacts/local/gallery-cluster-final-geometry-{name}.log'
    runs[name]['final_geometry_scope'] = test_run(scope_log, 2)
    shutil.copy2(scope_log, destination / 'final-geometry-scope.log')
    shutil.copy2(test_log, destination / 'ctest.log')
    for folder in ['gallery-cluster', 'gallery-preview', 'scene-gallery', 'scene-native', 'scene-surface-retirement']:
        shutil.copytree(build / 'artifacts' / f'{folder}-{config}', destination / f'{folder}-ctest', dirs_exist_ok=True)
    native = destination / 'gallery-cluster-ctest/native'
    rows = [json.loads(row) for row in (native / 'native-captures.jsonl').read_text(encoding='utf-8').splitlines()]
    assert len(rows) == 210 and len({row['id'] for row in rows}) == 30
    assert {row['frame'] for row in rows} == {24, 63, 90, 120, 180, 240, 300}
    assert all(not row['state']['errors'] for row in rows)
    audit = load(native / 'independent-audit.json')
    assert audit['state_pairs'] == audit['dom_pairs'] == 210 and audit['html_geometry_fields'] == 4408
    runs[name]['native'] = {'cases': 30, 'captures': len(rows), 'max_mean': max(row['mean'] for row in rows), 'independent_audit': audit}
    runs[name]['cluster_warp'] = gpu_report(destination / 'gallery-cluster-ctest/warp')
    runs[name]['window_warp'] = window_report(destination / 'gallery-preview-ctest/warp')
    old = load(destination / 'scene-native-ctest/warp/report.json')
    assert old['captures'] == 102 and old['frames'] == old['retired_imports'] == 4117
    assert old['worst_channel_mean'] <= 3 and old['minimum_soft_iou'] >= .75 and old['debug_warnings'] == 0
    runs[name]['previous_34_scene_warp'] = old
    binary_path = destination / 'bin'
    binary_path.mkdir(exist_ok=True)
    for binary in ['weft_d3d11_probe.exe', 'weft_scene_cluster_tests.exe', 'weft_scene_cluster_contract_tests.exe', 'weft_script_dom_tests.exe', 'weft_owner_executor_tests.exe', 'weft_animation_frame_tests.exe', 'weft_scene_gallery_tests.exe']:
        shutil.copy2(build / 'bin' / config / binary, binary_path / binary)
    data = (binary_path / 'weft_d3d11_probe.exe').read_bytes()
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    assert struct.unpack_from('<Q', data, pe + 24 + 72)[0] == 8388608
    runs[name]['exe_stack_reserve_bytes'] = 8388608
    hardware_suffix = '' if name == 'release' else '-debug'
    runs[name]['cluster_hardware'] = gpu_report(out / ('gpu-hardware' + hardware_suffix))
    runs[name]['window_hardware'] = window_report(out / ('window' + hardware_suffix))
    retirement = load(destination / 'scene-surface-retirement-ctest/warp/report.json')
    assert retirement['fixed_submissions'] == retirement['fixed_completions'] == 206 and retirement['peak_live_slots'] <= 6 and retirement['peak_imported_bytes'] <= 536870912
    runs[name]['surface_retirement_warp'] = retirement
    scroll = load(out / ('scroll-hardware' + hardware_suffix) / 'preview-session.json')
    assert scroll['frames'] == scroll['actual_completed_submissions'] == 120 and scroll['captures'] == 2 and scroll['scroll_regression'] and scroll['scene_gpu_hidden_retirements'] > 0 and scroll['scene_gpu_peak_imported_bytes'] <= 536870912 and scroll['debug_warning_error_count'] == 0
    runs[name]['continuous_scroll_hardware'] = scroll

gc = out / 'final/gc'
gc.mkdir(parents=True, exist_ok=True)
gc_log = root / 'artifacts/local/gallery-cluster-gc-tests.log'
runs['gc'] = {'scope': test_run(gc_log, 5), 'full_original_301_frame_suite_repeated': False}
shutil.copy2(gc_log, gc / 'ctest.log')
(gc / 'bin').mkdir(exist_ok=True)
for binary in ['weft_script_dom_tests.exe', 'weft_scene_cluster_contract_tests.exe', 'weft_animation_frame_tests.exe', 'weft_owner_executor_tests.exe', 'weft_scene_contract_tests.exe']:
    shutil.copy2(root / 'build/windows-gc-stress/bin/Debug' / binary, gc / 'bin' / binary)

benchmarks = {}
for scenario in ['d3-interactive', 'kline', 'framer', 'spring', 'icons', 'anime', 'css-middle']:
    path = out / 'performance' / scenario
    benchmark = load(path / 'benchmark.json')
    session = load(path / 'preview-session.json')
    assert benchmark['frames'] == session['actual_completed_submissions'] == 240
    assert benchmark['warm_frames'] == 224 and not benchmark['capture_readback']
    assert session['cpu_tick_ms']['samples'] == session['gpu_producer_and_ui_elapsed_ms']['samples'] == 224
    assert session['debug_warning_error_count'] == 0
    thread = load(path / 'thread-work.json')
    assert thread['owner_updates'] == 240 and thread['accepted_inputs'] == thread['processed_inputs'] == 0
    benchmarks[scenario] = {'benchmark': benchmark, 'session': session, 'thread': thread, 'render': load(path / 'render-work.json')}

original_manifest = load(root / 'tests/fixtures/crpg-gallery/manifest.json')
original = Path(original_manifest['source_root'])
drift = {'checked': len(original_manifest['files']), 'source_root': str(original), 'changed': [], 'missing': []}
for name, expected in original_manifest['files'].items():
    path = original / name
    if not path.is_file():
        drift['missing'].append(name)
    elif sha(path) != expected:
        drift['changed'].append(name)

source_names = ['AGENTS.md', 'README.md', 'CMakeLists.txt', 'CMakePresets.json']
for folder in ['include/caseelephant', 'src', 'probes', 'docs', 'tests', 'examples/crpg-gallery', 'shaders', 'tools']:
    source_names += [path.relative_to(root).as_posix() for path in (root / folder).rglob('*') if path.is_file() and 'node_modules' not in path.parts and '__pycache__' not in path.parts]
source_hashes = {name: sha(root / name) for name in sorted(set(source_names))}
for name in source_hashes:
    path = out / 'source-snapshot' / name
    path.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(root / name, path)
logs = out / 'logs'
logs.mkdir(exist_ok=True)
for path in (root / 'artifacts/local').glob('gallery-cluster*.log'):
    shutil.copy2(path, logs / path.name)
shutil.copy2(root / 'build/windows-msvc/CMakeCache.txt', out / 'final/CMakeCache.txt')
shutil.copy2(root / 'build/windows-gc-stress/CMakeCache.txt', gc / 'CMakeCache.txt')
artifact_hashes = {}
for path in out.rglob('*'):
    if not path.is_file() or path.name == 'run-metadata.json' or any(part in {'.chrome-profile', 'chrome-profile', 'browser-profile', 'node_modules', '__pycache__'} for part in path.parts):
        continue
    artifact_hashes[path.relative_to(root).as_posix()] = sha(path)
record = {
    'format': 'gallery-shared-cluster-seal-v1', 'recorded_utc': datetime.now(timezone.utc).isoformat(),
    'tests': runs, 'benchmarks': benchmarks, 'sources': source_hashes, 'artifacts': artifact_hashes,
    'binaries': {name: value for name, value in artifact_hashes.items() if name.endswith('.exe')},
    'previous_seal': {'path': prior_path.relative_to(root).as_posix(), 'sha256': sha(prior_path), 'verified_records': len(prior['sources']) + len(prior['artifacts'])},
    'environment': {key: prior['environment'][key] for key in ['os', 'cpu', 'gpu', 'gpu_driver', 'compiler', 'performance_configuration']} | {
        'surface': [3840, 2160], 'logical_shell': [1920, 1080], 'scene_scale': 2,
        'scene_vms': 64, 'canvas_vms': 2, 'fonts': 'fixed shell Roboto/Noto CJK/Mplus; scene OS Microsoft YaHei UI/Arial/Consolas/Emoji; per-case browser font reports and fixture hashes retained',
        'sampling': 'one serial run per group first viewport; 240 deterministic owner frames, 16 warmup, 224 samples; actual completion drain; no readbacks or injected input; default process priority/affinity; no concurrent build/tests',
        'gpu_elapsed_limit': 'producer+UI query elapsed includes CPU queue gaps/owner wait; excludes HUD/Present; not pure GPU busy time',
        'nodes': '438 live shell nodes; 1061 full DOM element entries across the new 30 scenes at frame24 (380 D3 Interactive / 444 K-Line / 14 Framer / 12 Spring / 69 Icons / 142 Anime), excluding root/text; final geometry audit captures only div/button boxes it compares',
    },
    'source_drift': drift, 'native_cases': 66, 'catalogue_cases': 72, 'pending_cases': 6,
    'original_full_gallery_rendered': False, 'ultralight_comparable': False, 'mobile_verified': False,
    'limits': ['bounded Windows scene/DOM/platform profile, not full browser or whole-library API support', 'original Anime Throw remains stationary in browser/native; pinned-library valid-transform inertia countertest passes', '2 MiB VM stack for new scene profiles; probe EXEs reserve 8 MiB host stack, production host headroom needs integration verification', 'hidden new rAF paused while timer pumped; benchmark contains no injected interactions', '2 WebGL and 4 Lottie cases pending', 'mobile configuration/compilation/link/simulator/device function/performance all unverified', 'full game integration/public owner C ABI/input-to-display latency/long run/total VRAM/fair Ultralight comparison unverified'],
    'exploration': 'earlier failure/debugger logs and targeted attempts retained separately; only final full logs and final hardware reports count as acceptance',
}
(out / 'run-metadata.json').write_text(json.dumps(record, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(f'SEALED {len(source_hashes)} sources / {len(artifact_hashes)} artifacts / {len(record["binaries"])} binaries; original drift {len(drift["changed"])}/{len(drift["missing"])}')
