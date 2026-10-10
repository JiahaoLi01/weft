"""Record final local build/report hashes without touching the reference game."""
from pathlib import Path
from datetime import datetime, timezone
import hashlib
import json
import re
import shutil

root=Path(__file__).resolve().parents[1]
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
sources=['CMakeLists.txt','src/layout/positioned_page.cpp','src/layout/positioned_page.hpp',
         'tests/positioned_page_tests.cpp','probes/cheetah_probe.hpp','probes/cheetah_probe.cpp','probes/d3d11_probe.cpp','src/render/backends/d3d11/backend.cpp',
         'tests/fixtures/casecheetah/manifest.json','tests/fixtures/cheetah_browser_reference.json',
         'examples/cheetah-nine-slice/native/metadata.json','tools/compare_cheetah_reports.py','tools/verify_cheetah_fixture.py']
for name,build,config,log,count in [('debug','windows-msvc','Debug','ctest-final.log',28),
                                  ('release','windows-msvc','Release','ctest-final.log',28),
                                  ('gc-stress','windows-gc-stress','Debug','ctest-focused.log',4)]:
    folder=root/'artifacts/local'/('cheetah-'+name)
    binary=root/'build'/build/'bin'/config/'weft_d3d11_probe.exe'
    warp=root/'build'/build/'artifacts'/('cheetah-'+config)/'warp'
    shutil.copytree(warp,folder/'warp',dirs_exist_ok=True)
    text=(folder/log).read_text(encoding='utf-8')
    assert f'0 tests failed out of {count}' in text
    duration=re.search(r'Total Test time \(real\) =\s+([\d.]+)',text).group(1)
    reports={}
    for driver in (('warp',) if name=='gc-stress' else ('warp','hardware')):
        p=folder/driver/'report.json'
        assert p.stat().st_mtime>=binary.stat().st_mtime, 'Report predates final binary'
        r=json.loads(p.read_text(encoding='utf-8'))
        assert r['passed'] and len(r['frames'])==3 and r['cpu_pixel_samples']==6612 and r['cpu_pixel_max_difference']<=3
        assert r['native_textures_retired']==211 and r['debug_layer'] and r['debug_warning_error_count']==0
        assert not r['full_game_verified'] and not r['chinese_font_verified'] and not r['mouse_geometry_hit_verified']
        if driver=='hardware':
            # Re-run pure comparison after final native output was refreshed.
            import subprocess,sys
            subprocess.run([sys.executable,str(root/'tools/compare_cheetah_reports.py'),str(root/'tests/fixtures/cheetah_browser_reference.json'),str(p),'--output',str(folder/driver/'browser-comparison.json')],check=True)
        reports[driver]={'adapter':r['adapter'],'cpu_pixel_samples':r['cpu_pixel_samples'],'cpu_pixel_max_difference':r['cpu_pixel_max_difference'],
                         'retired':r['native_textures_retired'],'browser':json.loads((folder/driver/'browser-comparison.json').read_text())}
    outputs={p.relative_to(root).as_posix():sha(p) for p in folder.rglob('*') if p.is_file() and p.name!='run-metadata.json'}
    data={'fixture':'casecheetah-nine-slice-v1','recorded_utc':datetime.now(timezone.utc).isoformat(),'configuration':name,
          'tests_passed':count,'ctest_seconds':float(duration),'test_scope':'new workload only' if name=='gc-stress' else 'full suite',
          'binary_sha256':sha(binary),'source_sha256':{p:sha(root/p) for p in sources},'output_sha256':outputs,'reports':reports,
          'full_game_verified':False,'mobile_verified':False,'performance_measured':False,'source_game_writes':False}
    (folder/'run-metadata.json').write_text(json.dumps(data,indent=2)+'\n',encoding='utf-8')
    print(f'{name}: {count}/{count}, {duration}s, final binary/source/report hashes recorded')
