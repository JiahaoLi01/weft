"""Seal the executed Canvas stage without replacing earlier stage evidence."""
from datetime import datetime, timezone
from pathlib import Path
import hashlib
import json
import re
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / 'artifacts/local/gallery-canvas'
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
for name in ('verify_gallery_fixture.py', 'verify_gallery_runtime.py', 'verify_canvas_fixture.py'):
    subprocess.run([sys.executable, str(root / 'tools' / name)], check=True)

tests, binaries = {}, {}
for config, build, count in (('debug', 'windows-msvc', 67), ('release', 'windows-msvc', 67), ('gc-stress', 'windows-gc-stress', 4)):
    directory = out / config
    log = (directory / 'ctest.log').read_text(encoding='utf-8-sig')
    assert f'0 tests failed out of {count}' in log
    tests[config] = {'passed': count, 'seconds': float(re.search(r'Total Test time \(real\) =\s+([\d.]+)', log).group(1)),
                     'scope': 'full suite' if count == 67 else 'Canvas/DOM/rAF/eight-cycle React; original large gallery bundle excluded'}
    mode = 'Release' if config == 'release' else 'Debug'
    directory_bin = root / 'build' / build / 'bin' / mode
    names = ['weft_canvas_probe_tests.exe', 'weft_script_dom_tests.exe', 'weft_animation_frame_tests.exe', 'weft_page_probe_tests.exe']
    if config != 'gc-stress':
        names += ['weft_d3d11_probe.exe', 'weft_style_probe_tests.exe', 'weft_gallery_probe.exe']
        warp = root / 'build' / build / 'artifacts' / ('gallery-canvas-' + mode)
        shutil.copytree(warp, directory / 'ctest-results', dirs_exist_ok=True)
        if config == 'debug':
            final = (directory / 'ctest-final-scope.log').read_text(encoding='utf-8-sig')
            assert '0 tests failed out of 5' in final
            assert (directory / 'ctest-final-scope.log').stat().st_mtime >= (directory_bin / 'weft_d3d11_probe.exe').stat().st_mtime
            tests[config]['final_scope'] = {'passed': 5, 'seconds': float(re.search(r'Total Test time \(real\) =\s+([\d.]+)', final).group(1))}
        else:
            assert (directory / 'ctest.log').stat().st_mtime >= (directory_bin / 'weft_d3d11_probe.exe').stat().st_mtime
        for driver, path in (('warp', directory / 'ctest-results/warp'), ('hardware', directory / 'hardware')):
            report_path = path / 'report.json'
            assert report_path.stat().st_mtime >= (directory_bin / 'weft_d3d11_probe.exe').stat().st_mtime
            report = json.loads(report_path.read_text(encoding='utf-8'))
            assert report['case_count'] == 2 and report['frames_per_case'] == 64 and report['seed'] == 0xce202610
            for case in report['cases']:
                assert case['passed'] and case['cpu_pixel_max_difference'] <= 1
                assert case['actual_completed_submissions'] == case['retired_textures'] == 65
                assert case['peak_in_flight'] == 2 and case['debug_warning_error_count'] == 0
                assert case['native_input_events'] == 2 and len(case['browser_bitmap_differences']) == 3
            tests[config][driver] = {'cases': 2, 'adapter': report['cases'][0]['adapter'], 'samples': sum(c['cpu_pixel_samples'] for c in report['cases'])}
        for driver, path in (('warp', directory / 'ctest-results/window-warp'), ('hardware', directory / 'window-hardware')):
            window = json.loads((path / 'window-report.json').read_text(encoding='utf-8'))
            assert window['frames'] == window['actual_completed_submissions'] == 8 and window['surface'] == [3840, 2160]
            assert (path / 'window-report.json').stat().st_mtime >= (directory_bin / 'weft_d3d11_probe.exe').stat().st_mtime
            assert (path / 'window-4k-hud.png').exists()
        observation = root / 'build' / build / 'artifacts' / ('gallery-' + mode) / 'native-execution.json'
        shutil.copyfile(observation, directory / 'native-execution.json')
    binaries.update({(directory_bin / name).relative_to(root).as_posix(): sha(directory_bin / name) for name in names})

original = json.loads((root / 'tests/fixtures/crpg-gallery/manifest.json').read_text(encoding='utf-8'))
source_root = Path(original['source_root'])
drift = {'source_root': str(source_root), 'checked': len(original['files']), 'changed': [], 'missing': [], 'changing_during_read': []}
for name, digest in original['files'].items():
    path = source_root / name
    if not path.is_file(): drift['missing'].append(name)
    else:
        actual = sha(path)
        if actual != sha(path): drift['changing_during_read'].append(name)
        if actual != digest: drift['changed'].append(name)
(out / 'source-drift.json').write_text(json.dumps(drift, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')

sources = ['CMakeLists.txt', 'CMakePresets.json', 'README.md', 'AGENTS.md']
for folder in ('include/caseelephant', 'src', 'probes', 'tools', 'docs', 'tests/fixtures/canvas-gallery-reference'):
    sources += [p.relative_to(root).as_posix() for p in (root / folder).rglob('*') if p.is_file() and '__pycache__' not in p.parts]
sources += ['tests/canvas_probe_tests.cpp', 'tests/style_probe_tests.cpp', 'examples/crpg-gallery/canvas-profile.css',
            'examples/crpg-gallery/native/WEBGL_TESTS.js', 'examples/crpg-gallery/native/platform.js',
            'examples/crpg-gallery/native/metadata.json', 'third_party/gallery-runtime/manifest.json',
            'tests/fixtures/crpg-gallery/manifest.json', 'tests/fixtures/crpg-gallery/UI/src/components/debug/TestGallery.tsx']
artifacts = {p.relative_to(root).as_posix(): sha(p) for p in out.rglob('*') if p.is_file() and 'browser-profile' not in p.parts and p.name != 'run-metadata.json'}
browser = json.loads((out / 'reference/report.json').read_text(encoding='utf-8'))
record = {'format': 'gallery-canvas-seal-v1', 'recorded_utc': datetime.now(timezone.utc).isoformat(), 'tests': tests,
          'binaries': binaries, 'sources': {name: sha(root / name) for name in sorted(set(sources))}, 'artifacts': artifacts,
          'browser': browser['version'], 'original_gallery_cases': 72, 'new_canvas_cases_verified': 2,
          'exact_browser_pixel_parity': False, 'performance_verified': False, 'angle_integrated': False,
          'public_external_texture_implemented': False, 'mobile_verified': False,
          'environment': {'os': 'Windows 11 26200', 'cpu': 'AMD Ryzen 9 9950X3D', 'compiler': 'MSVC 19.42',
                          'raster': 'D2D SOFTWARE/WIC RGBA8 snapshot', 'backing': [800, 600], 'logical_viewport': [800, 600],
                          'hud_target': [3840, 2160], 'font_images': 'original canvas cases none; HUD fixed Roboto/Noto CJK',
                          'sampling': '64 deterministic rAF frames per case, no FPS/CPU/GPU/RSS performance benchmark'}}
(out / 'run-metadata.json').write_text(json.dumps(record, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(f"SEALED {len(record['sources'])} sources / {len(artifacts)} artifacts / {len(binaries)} binaries; original read-only drift {len(drift['changed'])}/{len(drift['missing'])}/{len(drift['changing_during_read'])}")
