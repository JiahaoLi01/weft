"""Seal the executed flow hit/HWND stage while preserving prior stage seals."""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


sources = ['CMakeLists.txt', 'CMakePresets.json', 'README.md', 'third_party/manifest.json',
           'tests/fixtures/casecheetah/manifest.json', 'examples/cheetah-alert/native/app.js',
           'examples/cheetah-flow/profile.css', 'tests/fixtures/flow_input_browser_reference.json',
           'tools/compare_flow_input.py', 'tools/verify_flow_input_fixture.py',
           'tools/record_flow_input_evidence.py']
sources += sorted(p.relative_to(ROOT).as_posix()
                  for directory in ('src', 'include', 'probes', 'shaders', 'docs', 'tests/fixtures/flow-input')
                  for p in (ROOT / directory).rglob('*') if p.is_file())
sources += sorted(p.relative_to(ROOT).as_posix() for p in (ROOT / 'tests').glob('*.cpp'))

# The user's source is mutable. Compare only; never refresh our captured fixture.
manifest = json.loads((ROOT / 'tests/fixtures/casecheetah/manifest.json').read_text(encoding='utf-8'))
source = Path(manifest['source_root'])
changed, missing, changing = [], [], []
for name, expected in manifest['files'].items():
    path = source / name
    if not path.is_file():
        missing.append(name)
        continue
    observed = sha(path)
    if observed != sha(path):
        changing.append(name)
    if observed != expected:
        changed.append(name)
folder = ROOT / 'artifacts/local/flow-input-debug'
folder.mkdir(parents=True, exist_ok=True)
drift = dict(recorded_utc=datetime.now(timezone.utc).isoformat(), source_root=str(source),
             compared_files=len(manifest['files']), changed=changed, missing=missing,
             changed_during_read=changing, source_written=False, snapshot_refreshed=False)
(folder / 'source-drift.json').write_text(json.dumps(drift, ensure_ascii=False, indent=2), encoding='utf-8')

for name, build, config, count, suffix in [
        ('debug', 'windows-msvc', 'Debug', 54, 'debug'),
        ('release', 'windows-msvc', 'Release', 54, 'release'),
        ('gc-stress', 'windows-gc-stress', 'Debug', 7, 'gc')]:
    folder = ROOT / 'artifacts/local' / ('flow-input-' + name)
    folder.mkdir(parents=True, exist_ok=True)
    binary_dir = ROOT / 'build' / build / 'bin' / config
    binaries = {n: binary_dir / n for n in ('ce_d3d11_probe.exe', 'ce_flow_input_tests.exe',
                                          'ce_input_probe_tests.exe', 'ce_script_dom_tests.exe')}
    log = ROOT / 'artifacts/local' / ('flow-input-ctest-' + suffix + '.log')
    text = log.read_text(encoding='utf-8')
    assert f'0 tests failed out of {count}' in text
    assert log.stat().st_mtime >= max(p.stat().st_mtime for p in binaries.values())
    shutil.copyfile(log, folder / 'ctest.log')
    artifacts = ROOT / 'build' / build / 'artifacts'
    shutil.copytree(artifacts / ('flow-input-' + config), folder, dirs_exist_ok=True)
    gpu_folder = ROOT / 'artifacts/local' / ('cheetah-flow-window-' + name)
    shutil.copytree(artifacts / ('cheetah-flow-window-' + config) / 'warp', gpu_folder / 'warp', dirs_exist_ok=True)
    subprocess.run([sys.executable, str(ROOT / 'tools/compare_flow_input.py'),
                    str(ROOT / 'tests/fixtures/flow_input_browser_reference.json'), str(folder / 'native.json'),
                    str(folder / 'comparison.json')], check=True)
    reports = {}
    for driver in (('warp',) if name == 'gc-stress' else ('warp', 'hardware')):
        path = gpu_folder / driver / 'report.json'
        assert path.stat().st_mtime >= binaries['ce_d3d11_probe.exe'].stat().st_mtime
        r = json.loads(path.read_text(encoding='utf-8'))
        assert r['fixture'] == 'casecheetah-flow-window-v1'
        assert all(r[k] for k in ('passed', 'hidden_message_fixture', 'actual_completion_query',
                                  'message_receipt_viewport_verified', 'signed_screen_coordinates_verified'))
        assert (r['frames'], r['completed_serial'], r['native_textures_retired']) == (20, 40, 2389)
        assert r['wheel_messages'] == 12 and r['mouse_messages'] == 18 and r['resize_calls'] == 3
        assert r['cpu_pixel_samples'] == 38640 and r['cpu_pixel_max_difference'] <= 3
        assert r['debug_warning_error_count'] == 0 and not r['physical_wheel_verified'] and not r['mobile_verified']
        reports[driver] = {k: r[k] for k in ('adapter', 'frames', 'cpu_pixel_samples',
                                           'cpu_pixel_max_difference', 'native_textures_retired',
                                           'completed_serial', 'wheel_messages', 'mouse_messages')}
    outputs = {p.relative_to(ROOT).as_posix(): sha(p) for directory in (folder, gpu_folder)
               for p in directory.rglob('*') if p.is_file() and p.name != 'run-metadata.json'}
    screenshot = ROOT / 'artifacts/local/flow-input-reference/frame-2.png'
    outputs[screenshot.relative_to(ROOT).as_posix()] = sha(screenshot)
    command = ("ctest --test-dir build/windows-gc-stress -C Debug -R 'flow_input|flow_window|mouse_input|script_dom_lifecycle|page_react' --output-on-failure"
               if name == 'gc-stress' else 'ctest --preset windows-' + name + ' --output-on-failure')
    result = dict(recorded_utc=datetime.now(timezone.utc).isoformat(), configuration=name,
                  tests_passed=count, ctest_command=command,
                  ctest_seconds=float(re.search(r'Total Test time \(real\) =\s+([\d.]+)', text).group(1)),
                  test_scope='new input/HWND, legacy mouse, DOM lifecycle and eight-cycle React GC' if name == 'gc-stress' else 'full suite',
                  binaries={n: sha(p) for n, p in binaries.items()},
                  sources={p: sha(ROOT / p) for p in sources}, outputs=outputs, gpu=reports,
                  browser_hit_points=3150, browser_hit_exact=3130,
                  browser_hit_boundary_differences=20, boundary_tolerance_css_px_linf=2,
                  native_random_mutation_checks=128, random_seed='0xCE802026',
                  physical_mouse_verified=False, physical_wheel_verified=False,
                  touch_verified=False, full_game_verified=False, mobile_verified=False,
                  performance_measured=False)
    (folder / 'run-metadata.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(name, count, result['ctest_seconds'], reports)
print('Read-only source comparison:', len(changed), 'changed,', len(missing), 'missing')
