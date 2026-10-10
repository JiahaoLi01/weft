"""Seal the native inspection window; keep earlier gallery/Canvas seals intact."""
from datetime import datetime, timezone
from pathlib import Path
import hashlib
import json
import re
import shutil

root = Path(__file__).resolve().parents[1]
out = root / 'artifacts/local/gallery-preview'
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
tests, binaries = {}, {}
for name, config in (('debug', 'Debug'), ('release', 'Release')):
    directory = out / name
    log = (directory / 'ctest.log').read_text(encoding='utf-8-sig')
    assert '100% tests passed, 0 tests failed out of 69' in log
    tests[name] = {'passed': 69, 'seconds': float(re.search(r'Total Test time \(real\) =\s+([\d.]+)', log).group(1))}
    final_scope = (directory / 'final-scope.log').read_text(encoding='utf-8-sig')
    assert '100% tests passed, 0 tests failed out of 3' in final_scope
    tests[name]['final_scope'] = {'passed': 3, 'seconds': float(re.search(r'Total Test time \(real\) =\s+([\d.]+)', final_scope).group(1)),
                                 'note': 'After the GC-only observation branch; normal rendering behavior unchanged'}
    shutil.copytree(root / 'build/windows-msvc/artifacts' / ('gallery-preview-' + config) / 'warp', directory / 'warp', dirs_exist_ok=True)
    for driver in ('hardware', 'warp'):
        folder = directory / driver
        report = json.loads((folder / 'preview-session.json').read_text(encoding='utf-8'))
        assert report['frames'] == report['actual_completed_submissions'] == 160
        assert report['case_count'] == 72 and report['native_pixel_cases'] == 2
        assert not report['original_full_gallery_rendered'] and not report['ultralight_comparable']
        assert report['card_geometry_checks'] == 288 and report['cached_full_packet_checks'] == 17
        assert report['cpu_pixel_samples'] == 1680 and report['cpu_pixel_max_difference'] <= 3
        assert report['native_canvas_input_events'] >= 2 and report['minimized_opportunities'] > 0
        assert report['successful_visible_present_fps'] > 0 and report['peak_in_flight'] == 2
        assert report['debug_warning_error_count'] == 0
        cases = json.loads((folder / 'preview-cases.json').read_text(encoding='utf-8'))['cases']
        assert len(cases) == len({c['id'] for c in cases}) == 72
        assert sum(c['execution'] == 'no_error_observed' for c in cases) == 40
        assert sum(c['pixels'] == 'native_canvas_profile' for c in cases) == 2
        tests[name][driver] = report
    snapshot = directory / 'bin'
    snapshot.mkdir(exist_ok=True)
    for filename in ('weft_d3d11_probe.exe', 'weft_animation_frame_tests.exe'):
        src = root / 'build/windows-msvc/bin' / config / filename
        target = snapshot / filename
        shutil.copy2(src, target)
        binaries[target.relative_to(root).as_posix()] = sha(target)

gc_log = (out / 'gc/ctest.log').read_text(encoding='utf-8-sig')
assert '100% tests passed, 0 tests failed out of 3' in gc_log
tests['gc'] = {'passed': 3, 'seconds': float(re.search(r'Total Test time \(real\) =\s+([\d.]+)', gc_log).group(1)),
               'scope': 'animation_frame_contract, native gallery window and original Canvas window'}
shutil.copytree(root / 'build/windows-gc-stress/artifacts/gallery-preview-Debug/warp', out / 'gc/warp', dirs_exist_ok=True)
gc_cases = json.loads((out / 'gc/warp/preview-cases.json').read_text(encoding='utf-8'))['cases']
assert len(gc_cases) == 72 and all(c['execution'] == 'not_run_gc_instrumentation' for c in gc_cases)
for filename in ('weft_d3d11_probe.exe', 'weft_animation_frame_tests.exe'):
    target = out / 'gc/bin' / filename
    target.parent.mkdir(exist_ok=True)
    shutil.copy2(root / 'build/windows-gc-stress/bin/Debug' / filename, target)
    binaries[target.relative_to(root).as_posix()] = sha(target)

manifest = json.loads((root / 'tests/fixtures/crpg-gallery/manifest.json').read_text(encoding='utf-8'))
original = Path(manifest['source_root'])
drift = {'checked': len(manifest['files']), 'source_root': str(original), 'changed': [], 'missing': [], 'changing_during_read': []}
for name, expected in manifest['files'].items():
    p = original / name
    if not p.is_file():
        drift['missing'].append(name)
    else:
        actual = sha(p)
        if actual != sha(p): drift['changing_during_read'].append(name)
        if actual != expected: drift['changed'].append(name)
(out / 'source-drift.json').write_text(json.dumps(drift, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')

sources = ['AGENTS.md', 'README.md', 'CMakeLists.txt', 'CMakePresets.json', 'tests/animation_frame_tests.cpp',
           'tools/record_gallery_preview_evidence.py', 'tests/fixtures/crpg-gallery/manifest.json',
           'third_party/harfbuzz/source/test/api/fonts/Mplus1p-Regular.ttf']
for folder in ('include/caseelephant', 'src', 'probes', 'docs', 'examples/crpg-gallery'):
    sources += [p.relative_to(root).as_posix() for p in (root / folder).rglob('*') if p.is_file()]
artifacts = {p.relative_to(root).as_posix(): sha(p) for p in out.rglob('*') if p.is_file() and p.name != 'run-metadata.json'}
record = {'format': 'gallery-preview-seal-v1', 'recorded_utc': datetime.now(timezone.utc).isoformat(), 'tests': tests,
          'binaries': binaries, 'sources': {name: sha(root / name) for name in sorted(set(sources))}, 'artifacts': artifacts,
          'case_count': 72, 'native_pixel_cases': 2, 'new_original_pixel_cases': 0,
          'original_full_gallery_rendered': False, 'performance_benchmark': False, 'ultralight_comparable': False,
          'mobile_verified': False, 'environment': {'os': 'Windows 11 26200', 'cpu': 'AMD Ryzen 9 9950X3D',
          'compiler': 'MSVC 19.42', 'surface': [3840, 2160], 'canvas_backing': [800, 600],
          'fonts': ['Roboto', 'Noto CJK', 'fixed HarfBuzz Mplus symbol fallback'],
          'raster': 'Direct2D SOFTWARE WIC', 'frames_per_window_test': 160,
          'readback': 'correctness screenshots/copies; not a performance workload'}}
(out / 'run-metadata.json').write_text(json.dumps(record, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(f"SEALED {len(record['sources'])} sources / {len(artifacts)} artifacts / {len(binaries)} binaries; original drift {len(drift['changed'])}/{len(drift['missing'])}")
