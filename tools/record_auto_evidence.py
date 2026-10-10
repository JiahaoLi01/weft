"""Seal only fresh automatic-layout runs; preserve previous stage evidence."""
from datetime import datetime,timezone
import hashlib,json,re,shutil,subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parents[1];sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
sources=['CMakeLists.txt','third_party/manifest.json','tests/fixtures/casecheetah/manifest.json','examples/cheetah-auto/metadata.json','examples/cheetah-auto/profile.css','tests/fixtures/auto_layout_browser_reference.json','tests/fixtures/cheetah_auto_browser_reference.json','tests/auto_page_tests.cpp','tools/build_cheetah_auto.py','tools/compare_auto_layout.py','tools/compare_style_reports.py','tools/verify_auto_fixture.py','tools/record_auto_evidence.py']
sources+=sorted(p.relative_to(root).as_posix() for d in ('src','include','probes','shaders','tests/fixtures/auto-layout') for p in (root/d).rglob('*') if p.is_file())
for name,build,config,count,log in [('debug','windows-msvc','Debug',42,'auto-ctest-debug.log'),('release','windows-msvc','Release',42,'auto-ctest-release.log'),('gc-stress','windows-gc-stress','Debug',6,'auto-ctest-gc.log')]:
 folder=root/'artifacts/local'/('auto-'+name);folder.mkdir(parents=True,exist_ok=True)
 binary=root/'build'/build/'bin'/config/'ce_d3d11_probe.exe';test=binary.with_name('ce_auto_page_tests.exe')
 source_log=root/'artifacts/local'/log;text=source_log.read_text(encoding='utf-8');assert f'0 tests failed out of {count}' in text
 assert source_log.stat().st_mtime>=max(binary.stat().st_mtime,test.stat().st_mtime),'tests predate binaries'
 shutil.copyfile(source_log,folder/'ctest.log')
 gpu=root/'artifacts/local'/('cheetah-auto-'+name)
 shutil.copytree(root/'build'/build/'artifacts'/('cheetah-auto-'+config)/'warp',gpu/'warp',dirs_exist_ok=True)
 shutil.copytree(root/'build'/build/'artifacts'/('auto-layout-'+config),folder/'layout',dirs_exist_ok=True)
 reports={}
 for driver in (('warp',) if name=='gc-stress' else ('warp','hardware')):
  p=gpu/driver/'report.json';assert p.stat().st_mtime>=binary.stat().st_mtime
  r=json.loads(p.read_text(encoding='utf-8'));assert r['passed'] and r['automatic_layout_verified'] and r['post_vm_reset_render_verified'] and r['actual_completion_query'] and r['debug_warning_error_count']==0
  assert not r['original_expanded_rule_verified'] and not r['mobile_verified']
  subprocess.run([sys.executable,str(root/'tools/compare_style_reports.py'),str(root/'tests/fixtures/cheetah_auto_browser_reference.json'),str(p),'--output',str(gpu/driver/'browser-comparison.json')],check=True)
  reports[driver]={k:r[k] for k in ('adapter','cpu_pixel_samples','cpu_pixel_max_difference','native_textures_retired','fallback_glyph_count')}
 outputs={p.relative_to(root).as_posix():sha(p) for d in (folder,gpu) for p in d.rglob('*') if p.is_file() and p.name!='run-metadata.json'}
 result={'recorded_utc':datetime.now(timezone.utc).isoformat(),'configuration':name,'tests_passed':count,'ctest_seconds':float(re.search(r'Total Test time \(real\) =\s+([\d.]+)',text).group(1)),'test_scope':'automatic layout and eight-cycle React GC' if name=='gc-stress' else 'full suite','binary_sha256':sha(binary),'layout_test_binary_sha256':sha(test),'sources':{p:sha(root/p) for p in sources},'outputs':outputs,'gpu':reports,'full_game_verified':False,'mobile_verified':False,'performance_measured':False}
 (folder/'run-metadata.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(name,result['tests_passed'],result['ctest_seconds'])
