"""Seal final external-texture runs, preserving prior stage evidence."""
from datetime import datetime, timezone
from pathlib import Path
import hashlib
import json
import re
import shutil

root = Path(__file__).resolve().parents[1]
out = root / 'artifacts/local/external-texture'
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
tests, binaries = {}, {}
for name, config in (('debug', 'Debug'), ('release', 'Release')):
    directory = out / name
    log_path = directory / 'ctest.log'
    log = log_path.read_text(encoding='utf-8-sig')
    assert '100% tests passed, 0 tests failed out of 68' in log
    seconds = float(re.search(r'Total Test time \(real\) =\s+([\d.]+)', log).group(1))
    binary_directory = root / 'build/windows-msvc/bin' / config
    exe = binary_directory / 'ce_d3d11_probe.exe'
    assert log_path.stat().st_mtime >= exe.stat().st_mtime
    shutil.copytree(root / 'build/windows-msvc/artifacts' / ('external-texture-' + config), directory / 'ctest-results', dirs_exist_ok=True)
    tests[name] = {'passed': 68, 'seconds': seconds, 'scope': 'full suite'}
    for driver, report_path in (('warp', directory / 'ctest-results/warp/report.json'), ('hardware', directory / 'hardware/report.json')):
        assert report_path.stat().st_mtime >= exe.stat().st_mtime
        report = json.loads(report_path.read_text(encoding='utf-8'))
        assert report['passed'] and report['frames'] == report['external_textures_retired'] == 48
        assert report['cpu_coverage_textures_retired'] == 24 and report['peak_in_flight'] == 2
        assert report['cpu_pixel_samples'] == 1572864 and report['srgb_max_difference'] <= 3
        assert report['linear_float_max_error'] <= 1 / 256
        assert report['debug_layer'] and report['debug_warning_error_count'] == 0
        assert report['producer_gpu_only'] and report['actual_completion_queries']
        assert report['source_formats'] == ['RGBA8', 'RGBA16F', 'RGBA32F']
        assert not report['angle_integrated'] and not report['mobile_verified']
        assert len(list(report_path.parent.glob('*.png'))) == 4
        tests[name][driver] = report
    snapshot = directory / 'bin'
    snapshot.mkdir(exist_ok=True)
    for binary_name in ('ce_d3d11_probe.exe', 'ce_render_backend_tests.exe', 'ce_render_backend_c_smoke.exe'):
        source = binary_directory / binary_name
        destination = snapshot / binary_name
        shutil.copy2(source, destination)
        binaries[destination.relative_to(root).as_posix()] = sha(destination)

manifest = json.loads((root / 'tests/fixtures/crpg-gallery/manifest.json').read_text(encoding='utf-8'))
source_root = Path(manifest['source_root'])
drift = {'source_root': str(source_root), 'checked': len(manifest['files']), 'changed': [], 'missing': [], 'changing_during_read': []}
for name, expected in manifest['files'].items():
    path = source_root / name
    if not path.is_file():
        drift['missing'].append(name)
    else:
        actual = sha(path)
        if actual != sha(path): drift['changing_during_read'].append(name)
        if actual != expected: drift['changed'].append(name)
(out / 'source-drift.json').write_text(json.dumps(drift, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')

sources = ['AGENTS.md', 'CMakeLists.txt', 'CMakePresets.json', 'README.md', 'tests/fixtures/crpg-gallery/manifest.json']
for folder in ('include/caseelephant', 'src', 'probes', 'shaders', 'docs'):
    sources += [p.relative_to(root).as_posix() for p in (root / folder).rglob('*') if p.is_file()]
sources += ['tests/render_backend_tests.cpp', 'tests/render_backend_c_smoke.c', 'tools/record_external_texture_evidence.py']
artifacts = {p.relative_to(root).as_posix(): sha(p) for p in out.rglob('*') if p.is_file() and p.name != 'run-metadata.json'}
record = {'format': 'external-texture-seal-v1', 'recorded_utc': datetime.now(timezone.utc).isoformat(),
          'tests': tests, 'binaries': binaries, 'sources': {name: sha(root / name) for name in sorted(set(sources))},
          'artifacts': artifacts, 'original_gallery_cases': 72, 'new_gallery_cases_verified': 0,
          'external_texture_implemented': True, 'angle_integrated': False, 'mobile_verified': False,
          'gc_stress_rerun': False, 'performance_verified': False,
          'environment': {'os': 'Windows 11 26200', 'cpu': 'AMD Ryzen 9 9950X3D', 'compiler': 'MSVC 19.42',
                          'target': [256, 128], 'draws_per_frame': 4, 'source': 'GPU-produced linear premultiplied RGBA8/16F/32F, same extent',
                          'cpu_assets': 'one 1x1 coverage texel; no fonts or source images', 'frames_per_run': 48,
                          'sampling': 'full final targets, 1572864 pixels; no CPU/GPU/RSS/launch/tail latency performance benchmark'}}
(out / 'run-metadata.json').write_text(json.dumps(record, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(f"SEALED {len(record['sources'])} sources / {len(artifacts)} artifacts / {len(binaries)} binary snapshots; read-only original drift {len(drift['changed'])}/{len(drift['missing'])}/{len(drift['changing_during_read'])}")
