"""Seal executed gallery/rAF stage; preserve all previous stage evidence."""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / 'artifacts/local/gallery-foundation'
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()

for checker in ('verify_gallery_fixture.py', 'verify_gallery_runtime.py'):
    subprocess.run([sys.executable, str(root / 'tools' / checker)], check=True)

inventory = json.loads((root / 'tests/fixtures/crpg-gallery/inventory.json').read_text(encoding='utf-8'))
manifest = json.loads((root / 'tests/fixtures/crpg-gallery/manifest.json').read_text(encoding='utf-8'))
original = Path(manifest['source_root'])
drift = {'source_root': str(original), 'checked': len(manifest['files']), 'changed': [], 'missing': [], 'changing_during_read': []}
for name, expected in manifest['files'].items():
    path = original / name
    if not path.is_file():
        drift['missing'].append(name)
    else:
        digest = sha(path)
        if digest != sha(path): drift['changing_during_read'].append(name)
        if digest != expected: drift['changed'].append(name)
(out / 'source-drift.json').write_text(json.dumps(drift, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')

tests, binaries = {}, {}
for config, build, scope, count in (
    ('debug', 'windows-msvc', 'full suite', 63),
    ('release', 'windows-msvc', 'full suite', 63),
    ('gc-stress', 'windows-gc-stress', 'DOM/rAF/eight-cycle React only; no 72-case bundle instrumentation', 3)):
    folder = out / config
    text = (folder / 'ctest.log').read_text(encoding='utf-8-sig')
    assert f'0 tests failed out of {count}' in text
    tests[config] = {'passed': count, 'scope': scope, 'seconds': float(re.search(r'Total Test time \(real\) =\s+([\d.]+)', text).group(1))}
    bin_dir = root / 'build' / build / 'bin' / ('Release' if config == 'release' else 'Debug')
    animation_log = folder / 'ctest-final-animation.log'
    animation_text = animation_log.read_text(encoding='utf-8-sig')
    final_count = 1 if config == 'gc-stress' else 5
    assert f'0 tests failed out of {final_count}' in animation_text
    assert animation_log.stat().st_mtime >= (bin_dir / 'ce_animation_frame_tests.exe').stat().st_mtime
    tests[config]['final_animation_scope_passed'] = final_count
    tests[config]['final_animation_scope_seconds'] = float(re.search(r'Total Test time \(real\) =\s+([\d.]+)', animation_text).group(1))
    names = ['ce_animation_frame_tests.exe', 'ce_script_dom_tests.exe', 'ce_page_probe_tests.exe']
    if config != 'gc-stress':
        names += ['ce_gallery_probe.exe', 'ce_d3d11_probe.exe']
        final = (folder / 'ctest-final-gallery.log').read_text(encoding='utf-8-sig')
        assert '0 tests failed out of 4' in final
        assert (folder / 'ctest-final-gallery.log').stat().st_mtime >= (bin_dir / 'ce_gallery_probe.exe').stat().st_mtime
        tests[config]['final_gallery_passed'] = 4
        tests[config]['final_gallery_seconds'] = float(re.search(r'Total Test time \(real\) =\s+([\d.]+)', final).group(1))
        generated = root / 'build' / build / 'artifacts' / ('gallery-' + ('Release' if config == 'release' else 'Debug')) / 'native-execution.json'
        assert generated.stat().st_mtime >= (bin_dir / 'ce_gallery_probe.exe').stat().st_mtime
        shutil.copyfile(generated, folder / 'native-execution.json')
        subprocess.run([sys.executable, str(root / 'tools/verify_gallery_execution.py'), str(folder / 'native-execution.json')], check=True)
        # CTest's preset interprets relative JUnit destinations inside its test dir.
        xml = root / 'build' / build / 'artifacts/local/gallery-foundation' / config / 'ctest.xml'
        if xml.exists(): shutil.copyfile(xml, folder / 'ctest.xml')
    binaries.update({str((bin_dir / n).relative_to(root)).replace('\\', '/'): sha(bin_dir / n) for n in names})

reference = out / 'reference/browser-observations.json'
browser = json.loads(reference.read_text(encoding='utf-8'))
assert browser['case_count'] == inventory['case_count'] == 72
assert browser['bundle_metadata_sha256'] == sha(root / 'examples/crpg-gallery/native/metadata.json')
assert not any(c['errors'] or c['observation']['state']['errors'] for c in browser['cases'])
assert sum(bool(c['blocked_resources']) for c in browser['cases']) == 4
native = json.loads((out / 'release/native-execution.json').read_text(encoding='utf-8'))
assert native['no_error_observed'] == 38

sources = ['CMakeLists.txt', 'CMakePresets.json', 'README.md', 'AGENTS.md',
           'third_party/manifest.json', 'third_party/gallery-runtime/manifest.json']
for directory in ('src', 'include', 'probes', 'shaders', 'docs', 'tools', 'examples/crpg-gallery', 'tests/fixtures/crpg-gallery'):
    sources += [p.relative_to(root).as_posix() for p in (root / directory).rglob('*') if p.is_file() and '__pycache__' not in p.parts]
sources += [p.relative_to(root).as_posix() for p in (root / 'tests').rglob('*.cpp')]
artifacts = {}
for folder in [out / n for n in ('debug', 'release', 'gc-stress', 'reference')]:
    artifacts.update({p.relative_to(root).as_posix(): sha(p) for p in folder.rglob('*') if p.is_file() and 'browser-profile' not in p.parts})
artifacts[(out / 'source-drift.json').relative_to(root).as_posix()] = sha(out / 'source-drift.json')
record = {'format': 'gallery-foundation-seal-v1', 'recorded_utc': datetime.now(timezone.utc).isoformat(),
          'tests': tests, 'binaries': binaries, 'sources': {n: sha(root / n) for n in sorted(set(sources))},
          'artifacts': artifacts, 'original_cases': 72, 'native_no_error_observed': 38, 'native_blocked': 34,
          'browser': {'version': browser['version'], 'viewport': [800, 600], 'external_assets_blocked': 4},
          'native_gallery_pixels_verified': False, 'native_gallery_interaction_verified': False,
          'gallery_performance_verified': False, 'angle_integrated': False, 'mobile_verified': False}
(out / 'run-metadata.json').write_text(json.dumps(record, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(f"SEALED {len(record['sources'])} sources / {len(artifacts)} artifacts / {len(binaries)} binaries; original checked read-only")
