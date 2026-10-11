"""Seal actual completed runs, not inferred support or stale build outputs."""
from pathlib import Path
from datetime import datetime, timezone
import hashlib
import json
import re
import shutil
import subprocess
import sys

root=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
sources=['CMakeLists.txt','src/text/font.hpp','src/text/font.cpp','tests/font_fallback_tests.cpp',
 'src/layout/document.hpp','src/layout/document.cpp','tests/document_cases_tests.cpp',
 'probes/d3d11_probe.cpp','probes/cheetah_probe.hpp','probes/cheetah_alert_probe.hpp','src/render/backends/d3d11/quad.hlsl',
 'third_party/manifest.json','tests/fixtures/casecheetah/manifest.json','tests/fixtures/cheetah_alert_browser_reference.json',
 'examples/cheetah-alert/native/metadata.json','tools/compare_cheetah_alert.py','tools/verify_cheetah_alert.py',
 'tools/record_cheetah_alert_evidence.py']
for name,build,config,log,count in [('debug','windows-msvc','Debug','ctest-final.log',32),
 ('release','windows-msvc','Release','ctest-final.log',32),('gc-stress','windows-gc-stress','Debug','ctest-focused.log',5)]:
    folder=root/'artifacts/local'/('cheetah-alert-'+name)
    binary=root/'build'/build/'bin'/config/'weft_d3d11_probe.exe'
    shutil.copytree(root/'build'/build/'artifacts'/('cheetah-alert-'+config)/'warp',folder/'warp',dirs_exist_ok=True)
    text=(folder/log).read_text(encoding='utf-8');assert f'0 tests failed out of {count}' in text
    duration=float(re.search(r'Total Test time \(real\) =\s+([\d.]+)',text).group(1))
    reports={}
    for driver in (('warp',) if name=='gc-stress' else ('warp','hardware')):
        p=folder/driver/'report.json';assert p.stat().st_mtime>=binary.stat().st_mtime,'Report predates binary'
        r=json.loads(p.read_text(encoding='utf-8'))
        assert r['passed'] and len(r['frames'])==8 and r['fallback_glyph_count']==568
        assert r['native_textures_retired']==788 and r['cpu_pixel_samples']==217065 and r['cpu_pixel_max_difference']<=3
        assert r['debug_layer'] and r['debug_warning_error_count']==0
        assert all(r[k] for k in ('font_id_cache_collision_verified','post_vm_reset_render_verified','stale_list_target_verified','actual_completion_query','original_store_and_bridge_verified'))
        assert not r['real_unreal_commands_executed'] and not r['full_game_verified'] and not r['mobile_verified']
        subprocess.run([sys.executable,str(root/'tools/compare_cheetah_alert.py'),str(root/'tests/fixtures/cheetah_alert_browser_reference.json'),str(p),'--output',str(folder/driver/'browser-comparison.json')],check=True)
        reports[driver]={'adapter':r['adapter'],'cpu_pixel_samples':r['cpu_pixel_samples'],'cpu_pixel_max_difference':r['cpu_pixel_max_difference'],'retired':r['native_textures_retired'],
          'browser':json.loads((folder/driver/'browser-comparison.json').read_text(encoding='utf-8'))}
    outputs={p.relative_to(root).as_posix():sha(p) for p in folder.rglob('*') if p.is_file() and p.name!='run-metadata.json'}
    data={'fixture':'casecheetah-alert-v1','recorded_utc':datetime.now(timezone.utc).isoformat(),'configuration':name,
     'tests_passed':count,'ctest_seconds':duration,'test_scope':'new workload/font/positioned only' if name=='gc-stress' else 'full suite',
     'binary_sha256':sha(binary),'source_sha256':{p:sha(root/p) for p in sources},'output_sha256':outputs,'reports':reports,
     'post_notice_fixture_checks':json.loads((folder/'notice-verification.json').read_text(encoding='utf-8')),
     'full_game_verified':False,'mobile_verified':False,'performance_measured':False,'source_game_writes':False}
    (folder/'run-metadata.json').write_text(json.dumps(data,indent=2)+'\n',encoding='utf-8')
    print(f'{name}: {count}/{count}, {duration:.2f}s, final source/binary/output hashes recorded')
