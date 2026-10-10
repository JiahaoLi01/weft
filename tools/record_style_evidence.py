"""Seal executed style/cascade/GPU evidence after final compilation and tests."""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

root=Path(__file__).resolve().parents[1]
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
sources=['CMakeLists.txt','third_party/manifest.json','tests/fixtures/casecheetah/manifest.json',
 'examples/cheetah-style/metadata.json','examples/cheetah-style/profile.css',
 'tests/fixtures/style_browser_reference.json','tests/fixtures/cheetah_style_browser_reference.json',
 'tests/style_probe_tests.cpp','tools/build_cheetah_style.py','tools/verify_style_fixture.py',
 'tools/compare_style_reports.py','tools/record_style_evidence.py']
sources+=sorted(p.relative_to(root).as_posix() for directory in ('src','include','probes','shaders') for p in (root/directory).rglob('*') if p.is_file())
for name,build,config,count,log in [('debug','windows-msvc','Debug',37,'ctest-full.log'),
 ('release','windows-msvc','Release',37,'ctest-full.log'),('gc-stress','windows-gc-stress','Debug',7,'ctest-focused.log')]:
    folder=root/'artifacts/local'/('style-'+name);gpu_folder=root/'artifacts/local'/('cheetah-style-'+name)
    binary=root/'build'/build/'bin'/config/'ce_d3d11_probe.exe'
    style_binary=binary.with_name('ce_style_probe_tests.exe')
    text=(folder/log).read_text(encoding='utf-8');assert f'0 tests failed out of {count}' in text
    assert (folder/log).stat().st_mtime>=max(binary.stat().st_mtime,style_binary.stat().st_mtime)
    duration=float(re.search(r'Total Test time \(real\) =\s+([\d.]+)',text).group(1))
    shutil.copytree(root/'build'/build/'artifacts'/('cheetah-style-'+config)/'warp',gpu_folder/'warp',dirs_exist_ok=True)
    for p in (root/'build'/build/'artifacts'/('style-'+config)).glob('*.json'):shutil.copyfile(p,folder/p.name)
    reports={}
    for driver in (('warp',) if name=='gc-stress' else ('warp','hardware')):
        p=gpu_folder/driver/'report.json';assert p.stat().st_mtime>=binary.stat().st_mtime,'Report predates binary'
        r=json.loads(p.read_text(encoding='utf-8'));assert r['passed'] and len(r['frames'])==8
        assert r['native_textures_retired']==788 and r['cpu_pixel_samples']==217065 and r['cpu_pixel_max_difference']<=3
        assert r['debug_layer'] and r['debug_warning_error_count']==0
        assert all(r[k] for k in ('original_theme_css_verified','original_expanded_rule_verified','font_id_cache_collision_verified','post_vm_reset_render_verified','stale_list_target_verified','actual_completion_query'))
        assert not r['full_game_verified'] and not r['mobile_verified'] and not r['real_unreal_commands_executed']
        subprocess.run([sys.executable,str(root/'tools/compare_style_reports.py'),str(root/'tests/fixtures/cheetah_style_browser_reference.json'),str(p),'--output',str(gpu_folder/driver/'browser-comparison.json')],check=True)
        reports[driver]={'adapter':r['adapter'],'cpu_pixel_samples':r['cpu_pixel_samples'],'cpu_pixel_max_difference':r['cpu_pixel_max_difference'],'native_textures_retired':r['native_textures_retired'],
                         'browser':json.loads((gpu_folder/driver/'browser-comparison.json').read_text(encoding='utf-8'))}
    subprocess.run([sys.executable,str(root/'tools/compare_style_reports.py'),str(root/'tests/fixtures/style_browser_reference.json'),str(folder/'native.json'),'--output',str(folder/'browser-comparison.json')],check=True)
    outputs={p.relative_to(root).as_posix():sha(p) for d in (folder,gpu_folder) for p in d.rglob('*') if p.is_file() and p.name!='run-metadata.json'}
    result={'fixture':'casecheetah-style-v1','recorded_utc':datetime.now(timezone.utc).isoformat(),'configuration':name,
            'tests_passed':count,'ctest_seconds':duration,'test_scope':'style/positioned and eight-cycle React GC' if name=='gc-stress' else 'full suite',
            'binary_sha256':sha(binary),'style_test_binary_sha256':sha(style_binary),'source_sha256':{p:sha(root/p) for p in sources},
            'output_sha256':outputs,'reports':reports,'source_game_writes':False,'full_game_verified':False,'mobile_verified':False,'performance_measured':False}
    (folder/'run-metadata.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(f'{name}: {count}/{count}, {duration:.2f}s, source/binary/output hashes sealed')
