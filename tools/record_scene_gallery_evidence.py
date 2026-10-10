"""Seal only completed CSS/D3 evidence; preserve earlier gallery seals."""
from pathlib import Path
from datetime import datetime, timezone
import hashlib
import json
import re
import shutil

root = Path(__file__).resolve().parents[1]
out = root / 'artifacts/local/gallery-css-d3'
def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()
def load(p):
    return json.loads(p.read_text(encoding='utf-8'))
def tests(log, count):
    text = log.read_text(encoding='utf-8-sig')
    assert f'100% tests passed, 0 tests failed out of {count}' in text, log
    return {'passed': count, 'seconds': float(re.search(r'Total Test time \(real\) =\s+([\d.]+)', text).group(1))}
def scene(folder):
    r = load(folder / 'native-report.json')
    assert len(r['cases']) == 34 and all(c['execution'] == 'passed' and c['captures'] == 3 for c in r['cases'])
    assert max(c['worst_channel_mean'] for c in r['cases']) <= 3
    assert min(c['minimum_soft_iou'] for c in r['cases']) >= .75
    g = load(folder / 'geometry-diff.json')
    assert g['passed'] and g['captures'] == 102 and g['fields'] == 1476 and g['maximum_css_px'] <= .3
    return {'native': r, 'geometry': g}
def window(folder, gc=False):
    r = load(folder / 'preview-session.json')
    assert r['frames'] == r['actual_completed_submissions'] == 160
    assert r['case_count'] == 72 and r['native_pixel_cases'] == 36 and r['peak_in_flight'] == 2
    assert r['cpu_pixel_samples'] == 1680 and r['cpu_pixel_max_difference'] <= 3
    assert r['cached_full_packet_checks'] == 17 and r['card_geometry_checks'] == 288
    assert r['native_canvas_input_events'] >= 2 and r['minimized_opportunities'] > 0
    assert r['debug_warning_error_count'] == 0 and r['successful_visible_present_fps'] > 0
    assert not r['original_full_gallery_rendered'] and not r['ultralight_comparable']
    cases = load(folder / 'preview-cases.json')['cases']
    assert len(cases) == len({c['id'] for c in cases}) == 72
    assert sum(c['pixels'] == 'native_scene_profile' for c in cases) == 34
    assert sum(c['pixels'] == 'native_canvas_profile' for c in cases) == 2
    if gc:
        assert all(c['execution'] == 'not_run_gc_instrumentation' for c in cases)
    else:
        assert sum(c['execution'] == 'no_error_observed' for c in cases) == 45
    return r

runs, binaries = {}, {}
for name, config in (('release', 'Release'), ('debug', 'Debug'), ('gc', 'Debug')):
    build = root / 'build' / ('windows-gc-stress' if name == 'gc' else 'windows-msvc')
    target = out / name
    target.mkdir(exist_ok=True)
    gc = name == 'gc'
    runs[name] = tests(target / ('final-ctest.log' if gc else 'full-ctest.log'), 4 if gc else 73)
    if name == 'release': runs[name]['final_window'] = tests(target / 'final-window-ctest.log', 1)
    if gc: runs[name]['final_shared_renderer_scope'] = tests(target / 'final-wording-scope.log', 1)
    for origin, dest in ((f'scene-gallery-{config}', 'native'), (f'gallery-preview-{config}/warp', 'warp')):
        shutil.copytree(build / 'artifacts' / origin, target / dest, dirs_exist_ok=True)
    runs[name]['scene'] = scene(target / 'native')
    runs[name]['warp'] = window(target / 'warp', gc)
    if not gc: runs[name]['hardware'] = window(target / 'hardware')
    else: runs[name]['scope'] = '34 original scenes/121 frames/102 references/1476 geometry, scene contracts, 160-frame WARP window; 72 isolated module audit not repeated; only pending-count wording fixed afterward, final binary shared Canvas window rerun'
    for filename in ('ce_d3d11_probe.exe', 'ce_scene_gallery_tests.exe', 'ce_scene_contract_tests.exe'):
        dst = target / 'bin' / filename
        dst.parent.mkdir(exist_ok=True)
        shutil.copy2(build / 'bin' / config / filename, dst)
        binaries[dst.relative_to(root).as_posix()] = sha(dst)

manifest = load(root / 'tests/fixtures/crpg-gallery/manifest.json')
original = Path(manifest['source_root'])
drift = {'checked': len(manifest['files']), 'source_root': str(original), 'changed': [], 'missing': [], 'changing_during_read': []}
for name, expected in manifest['files'].items():
    p = original / name
    if not p.is_file(): drift['missing'].append(name)
    else:
        actual = sha(p)
        if actual != sha(p): drift['changing_during_read'].append(name)
        if actual != expected: drift['changed'].append(name)
(out / 'source-drift.json').write_text(json.dumps(drift, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')

sources = ['AGENTS.md', 'README.md', 'CMakeLists.txt', 'CMakePresets.json',
           'tools/compile_gallery_scene_styles.py', 'tools/browser_scene_gallery_reference.mjs',
           'tools/compare_scene_geometry.py', 'tools/verify_scene_fixture.py',
           'tools/record_scene_gallery_evidence.py', 'tools/gallery_runtime.mjs']
for folder in ('include/caseelephant', 'src', 'probes', 'docs', 'tests', 'examples/crpg-gallery'):
    sources += [p.relative_to(root).as_posix() for p in (root / folder).rglob('*') if p.is_file() and 'node_modules' not in p.parts]
source_hashes = {name: sha(root / name) for name in sorted(set(sources))}
# Preserve the implementation/profile/test/oracle used here. Dependency source
# hashes are covered separately by the normal dependency integrity tests.
for name in source_hashes:
    dst = out / 'source-snapshot' / name
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(root / name, dst)
excluded = ['reference-font-audit', 'comparison-first.png']
artifacts = {p.relative_to(root).as_posix(): sha(p) for p in out.rglob('*')
             if p.is_file() and p.name != 'run-metadata.json'
             and not any(part in excluded for part in p.relative_to(out).parts)}
record = {'format': 'scene-gallery-seal-v1', 'recorded_utc': datetime.now(timezone.utc).isoformat(),
          'tests': runs, 'sources': source_hashes, 'binaries': binaries, 'artifacts': artifacts,
          'excluded_initial_exploration': excluded, 'retained_failed_attempt': 'gc/initial-timeout-ctest.log',
          'cases': {'new_css': 26, 'new_d3': 8, 'native_total': 36, 'catalogue': 72, 'pending': 36},
          'original_full_gallery_rendered': False, 'performance_benchmark': False,
          'ultralight_comparable': False, 'mobile_verified': False,
          'environment': {'os': 'Windows 11 26200', 'cpu': 'AMD Ryzen 9 9950X3D', 'gpu': 'NVIDIA GeForce RTX 5090 / WARP',
                          'compiler': 'MSVC 19.42', 'cpp': 20, 'surface': [3840,2160], 'logical_shell': [1920,1080],
                          'css_body': [387,288], 'd3_body': [384,384], 'scene_raster_scale': 2,
                          'canvas_backing': [800,600], 'owner_nodes_across_34_cases_at_mount': 560,
                          'sampling': '121 per original case; captures 24/63/120; 160-frame window; last window segment 16 visible CSS bodies, 16 warmup frames',
                          'fonts': 'Windows system-ui (Microsoft YaHei UI), Arial, Consolas, OS Emoji fallback; manifest includes OS font hashes',
                          'raster': 'Direct2D SOFTWARE WIC + DirectWrite',
                          'readback': 'hidden correctness screenshots/copies; verification processes may overlap; not a performance workload'},
          'source_drift': drift}
(out / 'run-metadata.json').write_text(json.dumps(record, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
print(f"SEALED {len(source_hashes)} sources / {len(artifacts)} artifacts / {len(binaries)} binaries; original drift {len(drift['changed'])}/{len(drift['missing'])}")
