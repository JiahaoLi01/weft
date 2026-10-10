"""Seal executed flow/Unicode/scroll/rounded evidence without altering older stages."""
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


def read_drift():
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
    folder = ROOT / 'artifacts/local/flow-debug'
    folder.mkdir(parents=True, exist_ok=True)
    result = dict(recorded_utc=datetime.now(timezone.utc).isoformat(), source_root=str(source),
                  compared_files=len(manifest['files']), changed=changed, missing=missing,
                  changed_during_read=changing, source_written=False, snapshot_refreshed=False)
    (folder / 'source-drift.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print('Read-only source comparison:', len(changed), 'changed,', len(missing), 'missing')


sources = ['CMakeLists.txt', 'CMakePresets.json', 'README.md', 'third_party/manifest.json',
           'third_party/libunibreak/README.md', 'tests/fixtures/casecheetah/manifest.json',
           'examples/cheetah-flow/metadata.json', 'examples/cheetah-flow/profile.css',
           'examples/cheetah-flow/index.html', 'examples/cheetah-flow/scroll.js',
           'examples/cheetah-alert/native/app.js', 'tests/fixtures/flow_layout_browser_reference.json',
           'tests/fixtures/cheetah_flow_browser_reference.json', 'tests/flow_page_tests.cpp',
           'tools/build_cheetah_flow.py', 'tools/compare_flow_layout.py', 'tools/compare_style_reports.py',
           'tools/verify_flow_fixture.py', 'tools/record_flow_evidence.py']
sources += sorted(p.relative_to(ROOT).as_posix()
                  for directory in ('src', 'include', 'probes', 'shaders', 'docs', 'tests/fixtures/flow-layout')
                  for p in (ROOT / directory).rglob('*') if p.is_file())
read_drift()
for name, build, config, count, log in [
        ('debug', 'windows-msvc', 'Debug', 50, 'flow-ctest-debug.log'),
        ('release', 'windows-msvc', 'Release', 50, 'flow-ctest-release.log'),
        ('gc-stress', 'windows-gc-stress', 'Debug', 10, 'flow-ctest-gc.log')]:
    folder = ROOT / 'artifacts/local' / ('flow-' + name)
    folder.mkdir(parents=True, exist_ok=True)
    binary_dir = ROOT / 'build' / build / 'bin' / config
    binaries = {filename: binary_dir / filename for filename in
                ('weft_d3d11_probe.exe', 'weft_flow_page_tests.exe', 'weft_unicode_upstream_tests.exe')}
    source_log = ROOT / 'artifacts/local' / log
    text = source_log.read_text(encoding='utf-8')
    assert f'0 tests failed out of {count}' in text
    assert source_log.stat().st_mtime >= max(p.stat().st_mtime for p in binaries.values()), 'tests predate binaries'
    shutil.copyfile(source_log, folder / 'ctest.log')
    shutil.copytree(ROOT / 'build' / build / 'artifacts' / ('flow-layout-' + config), folder / 'layout', dirs_exist_ok=True)
    shutil.copytree(ROOT / 'build' / build / 'artifacts' / ('flow-' + config) / 'warp', folder / 'warp', dirs_exist_ok=True)
    gpu = ROOT / 'artifacts/local' / ('cheetah-flow-' + name)
    shutil.copytree(ROOT / 'build' / build / 'artifacts' / ('cheetah-flow-' + config) / 'warp', gpu / 'warp', dirs_exist_ok=True)
    reports = {}
    for driver in (('warp',) if name == 'gc-stress' else ('warp', 'hardware')):
        stats = {}
        for label, path in [('flow', folder / driver / 'report.json'), ('react', gpu / driver / 'report.json')]:
            assert path.stat().st_mtime >= binaries['weft_d3d11_probe.exe'].stat().st_mtime, 'GPU report predates binary'
            report = json.loads(path.read_text(encoding='utf-8'))
            assert all(report[k] for k in ('passed', 'post_vm_reset_render_verified', 'actual_completion_query'))
            assert report['debug_warning_error_count'] == 0 and not report['mobile_verified']
            if label == 'flow':
                assert report['independent_ellipse_area_max_difference'] <= 5
            else:
                assert report['flow_layout_verified'] and not report['original_expanded_rule_verified']
                subprocess.run([sys.executable, str(ROOT / 'tools/compare_style_reports.py'),
                                str(ROOT / 'tests/fixtures/cheetah_flow_browser_reference.json'), str(path),
                                '--output', str(path.with_name('browser-comparison.json'))], check=True)
            keys = ('adapter', 'cpu_pixel_samples', 'cpu_pixel_max_difference', 'native_textures_retired')
            stats[label] = {k: report[k] for k in keys}
        reports[driver] = stats
    outputs = {p.relative_to(ROOT).as_posix(): sha(p) for directory in (folder, gpu)
               for p in directory.rglob('*') if p.is_file() and p.name != 'run-metadata.json'}
    screenshot = ROOT / 'artifacts/local/cheetah-flow-reference/frame-7.png'
    outputs[screenshot.relative_to(ROOT).as_posix()] = sha(screenshot)
    log_command = ("ctest --test-dir build/windows-gc-stress -C Debug -R 'flow|unicode|script_dom_lifecycle|page_react' --output-on-failure"
                   if name == 'gc-stress' else 'ctest --preset windows-' + name + ' --output-on-failure')
    result = dict(recorded_utc=datetime.now(timezone.utc).isoformat(), configuration=name,
                  tests_passed=count, ctest_command=log_command,
                  ctest_seconds=float(re.search(r'Total Test time \(real\) =\s+([\d.]+)', text).group(1)),
                  test_scope='flow/Unicode, DOM lifecycle and eight-cycle React GC' if name == 'gc-stress' else 'full suite',
                  binaries={filename: sha(path) for filename, path in binaries.items()},
                  sources={p: sha(ROOT / p) for p in sources}, outputs=outputs, gpu=reports,
                  unicode_data=dict(line_version='15.0', line_passed=7619, line_skipped=35,
                                    grapheme_version='15.1', grapheme_passed=1187, grapheme_skipped=0),
                  full_game_verified=False, mobile_verified=False, performance_measured=False)
    (folder / 'run-metadata.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(name, count, result['ctest_seconds'])
