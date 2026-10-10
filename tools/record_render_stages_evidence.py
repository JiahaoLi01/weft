"""Seal stage 3/4/5 evidence without modifying earlier seals."""
from pathlib import Path
from datetime import datetime, timezone
import hashlib, json, re, shutil, statistics
root = Path(__file__).resolve().parents[1]
out = root / 'artifacts/local/render-stages'
def load(p): return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def tests(p, n):
    text=p.read_text(encoding='utf-8', errors='replace')
    assert re.search(r'100% tests passed, 0 tests failed out of '+str(n)+r'\b', text), str(p)
    return {'count': n, 'log': str(p.relative_to(root)), 'seconds': float(re.search(r'Total Test time \(real\) =\s*([0-9.]+)',text)[1])}
prior_path=root/'artifacts/local/owner-hotpath/run-metadata.json'
prior=load(prior_path)
for name, expected in prior['artifacts'].items(): assert sha(root/name)==expected,name
for name, expected in prior['sources'].items():
    assert sha(root/'artifacts/local/owner-hotpath/source-snapshot'/name)==expected,name
    assert sha(out/'baseline/source'/name)==expected,name
assert sha(out/'baseline/bin/weft_d3d11_probe.exe')==sha(root/'artifacts/local/owner-hotpath/final/release/bin/weft_d3d11_probe.exe')
runs={}
for name,config in [('release','Release'),('debug','Debug'),('gc','Debug')]:
    build=root/'build'/('windows-gc-stress' if name=='gc' else 'windows-msvc')
    dest=out/'final'/name;dest.mkdir(parents=True,exist_ok=True)
    if name=='gc':
        record={'scope':tests(out/'final8-gc-ctest.log',10),'instrumentation_only':True}
        folders=['render-stages','owner-async','external-texture','two-frame','render-cache']
    else:
        record={'full':tests(out/('final3-release-ctest.log' if name=='release' else 'final4-debug-ctest.log'),80)}
        record['final_semantic_scope']=tests(out/f'final8-{name}-scope.log',7)
        folders=['render-stages','scene-native','owner-async','scene-gpu','gallery-preview','scene-gallery']
    for folder in folders:
        src=build/'artifacts'/f'{folder}-{config}'
        shutil.copytree(src,dest/f'{folder}-ctest',dirs_exist_ok=True)
    binaries=['weft_d3d11_probe.exe','weft_owner_executor_tests.exe','weft_owner_hotpath_tests.exe','weft_scene_contract_tests.exe','weft_script_dom_tests.exe','weft_animation_frame_tests.exe']
    if name!='gc': binaries+=['weft_scene_gallery_tests.exe']
    (dest/'bin').mkdir(exist_ok=True)
    for binary in binaries: shutil.copy2(build/'bin'/config/binary,dest/'bin'/binary)
    runs[name]=record

def render_report(p):
    v=load(p/'report.json');assert v['comparisons']==160 and v['submissions']==v['completions']==320 and v['debug_warnings']==0
    assert v['native_passes']==160 and v['native_layer_builds']>100 and v['native_image_evictions']>0 and v['layer_reuses']>100 and v['partial_paints']>0
    assert v['worst_channel_mean']<=3 and v['minimum_soft_iou']>=.75
    return v

def scene_report(p):
    v=load(p/'report.json');assert len(v['cases'])==34 and v['captures']==102 and v['frames']==v['retired_imports']==4117
    assert v['optimized'] and v['native_passes']>1000 and v['native_fallbacks']>500 and v['native_layer_reuses']>300
    assert v['worst_channel_mean']<=3 and v['minimum_soft_iou']>=.75 and v['alpha_conversion_max_difference']<=3 and v['crop_reveal_verified']
    assert v['debug_warnings']==0 and v['peak_in_flight']==2
    return v

def window_report(p):
    v=load(p/'preview-session.json');assert v['frames']==v['actual_completed_submissions']==160 and v['case_count']==72 and v['native_pixel_cases']==36
    assert v['card_geometry_checks']==288 and v['cached_full_packet_checks']==17 and v['cpu_pixel_samples']==1680 and v['cpu_pixel_max_difference']<=3
    assert v['peak_in_flight']==2 and v['debug_warning_error_count']==0
    work=load(p/'render-work.json');assert work['native_passes']>100 and work['native_layer_reuses']>100
    return {'session':v,'render':work,'thread':load(p/'thread-work.json')}

def async_report(p):
    v=load(p/'report.json');assert v['frames']==v['submissions']==v['completions']==240 and v['accepted_inputs']==v['processed_inputs']==27 and v['separate_game_render_owner_threads']
    assert v['coalesced_requests']>0 and v['reused_frames']>0 and v['debug_warnings']==0
    return v
for name in ['release','debug']:
    dest=out/'final'/name
    runs[name]['render_warp']=render_report(dest/'render-stages-ctest/warp')
    runs[name]['scene_native_warp']=scene_report(dest/'scene-native-ctest/warp')
    runs[name]['async_warp']=async_report(dest/'owner-async-ctest/warp')
    runs[name]['window_warp']=window_report(dest/'gallery-preview-ctest/warp')
    runs[name]['scene_native_hardware']=scene_report(dest/'scene-native-hardware')
    runs[name]['window_hardware']=window_report(dest/'gallery-window-hardware')
runs['release']['render_hardware']=render_report(out/'final/release/render-stages-hardware')
runs['release']['async_hardware']=async_report(out/'final/release/owner-async-hardware')
runs['gc']['render_warp']=render_report(out/'final/gc/render-stages-ctest/warp')
runs['gc']['async_warp']=async_report(out/'final/gc/owner-async-ctest/warp')
benchmarks={}
for condition in ['paired','controlled']:
    benchmarks[condition]={}
    for scenario in ['css-middle','css-end']:
        record={}
        for phase in ['before','after']:
            rows=[]
            for number in [1,2,3]:
                p=out/'sampling-v2'/condition/f'round-{number}'/phase/scenario;b=load(p/'benchmark.json');s=load(p/'preview-session.json')
                assert b['frames']==s['actual_completed_submissions']==240 and b['warm_frames']==224 and not b['capture_readback'] and s['debug_warning_error_count']==0
                assert s['cpu_tick_ms']['samples']==s['gpu_producer_and_ui_elapsed_ms']['samples']==224
                row={'round':number,'benchmark':b,'session':s}
                if phase=='after':
                    row['thread']=load(p/'thread-work.json');row['render']=load(p/'render-work.json')
                    assert row['thread']['owner_updates']==240 and row['render']['native_passes']>100
                    if scenario=='css-middle':assert row['render']['native_layer_reuses']>100
                if condition=='controlled':
                    row['controls']=load(p/'process-controls.json');assert row['controls']['affinity_mask']==65535 and row['controls']['priority']=='High'
                rows.append(row)
            fps=[r['benchmark']['warm_throughput_fps'] for r in rows]
            record[phase]={'runs':rows,'median_run_fps':statistics.median(fps),'fps_range':[min(fps),max(fps)],'median_cpu_p50':statistics.median(r['session']['cpu_tick_ms']['p50'] for r in rows)}
        record['ratio']=record['after']['median_run_fps']/record['before']['median_run_fps'];benchmarks[condition][scenario]=record
manifest=load(root/'tests/fixtures/crpg-gallery/manifest.json');original=Path(manifest['source_root'])
drift={'checked':len(manifest['files']),'source_root':str(original),'changed':[],'missing':[]}
for name,expected in manifest['files'].items():
    p=original/name
    if not p.is_file():drift['missing'].append(name)
    elif sha(p)!=expected:drift['changed'].append(name)
sources=['AGENTS.md','README.md','CMakeLists.txt','CMakePresets.json']
for folder in ['include/caseelephant','src','probes','docs','tests','examples/crpg-gallery','shaders','tools']:
    sources += [p.relative_to(root).as_posix() for p in (root/folder).rglob('*') if p.is_file() and 'node_modules' not in p.parts and '__pycache__' not in p.parts]
source_hashes={name:sha(root/name) for name in sorted(set(sources))}
for name in source_hashes:
    p=out/'source-snapshot'/name;p.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(root/name,p)
artifacts={}
for p in out.rglob('*'):
    if not p.is_file() or p.name=='run-metadata.json':continue
    relative=p.relative_to(out).as_posix()
    if relative.startswith('baseline/build/') and relative not in ['baseline/build/bin/Release/weft_d3d11_probe.exe','baseline/build/CMakeCache.txt']:continue
    if relative.startswith('baseline/source/') and relative[len('baseline/source/'):] not in prior['sources']:continue
    artifacts[p.relative_to(root).as_posix()]=sha(p)
record={'format':'render-stages-seal-v1','recorded_utc':datetime.now(timezone.utc).isoformat(),'tests':runs,'benchmarks':benchmarks,'sources':source_hashes,'artifacts':artifacts,
        'binaries':{k:v for k,v in artifacts.items() if k.endswith('.exe')},
        'previous_seal':{'path':prior_path.relative_to(root).as_posix(),'sha256':sha(prior_path),'verified_records':len(prior['sources'])+len(prior['artifacts'])},
        'baseline_provenance':'isolated MSVC Release rebuild from 650 unchanged owner-hotpath sealed sources; own WEFT_SOURCE_DIR and bootstrap; pinned dependencies/non-gallery assets unchanged; original binary retained separately',
        'baseline_build_cache_sha256':sha(out/'baseline/build/CMakeCache.txt'),'baseline_measured_executable_sha256':sha(out/'baseline/build/bin/Release/weft_d3d11_probe.exe'),
        'environment':prior['environment']|{'sampling':'240 deterministic owner frames, 16 warmup, 224 samples, three alternating before/after pairs, final query drain, no readback; default and both-child affinity 0xFFFF/High conditions; no concurrent builds/tests'},
        'source_drift':drift,'native_cases':36,'catalogue_cases':72,'pending_cases':36,'original_full_gallery_rendered':False,'ultralight_comparable':False,'mobile_verified':False,
        'limits':['internal C++ owner/native pass adapters; external C ABI threading/game integration still pending','native full-card paint and encoded-to-linear conversion retained; partial damage primarily fallback','ASCII glyph coverage/other color glyph textures use Windows reference font raster, no page SDF atlas','accounted cache bytes exclude keys/active leases/driver VRAM','GPU elapsed includes CPU queue gaps and owner wait; Canvas scope changed, not pure busy time','cold-first-frame spike, long-run clocks/power, actual game integration/input-to-display latency and mobile unmeasured'],
        'exploration':'failed XHTML dispatch/percent opacity/brush quantization/instance offset/window/capacity attempts preserved; first contended GC run failed; final quiet GC is separate; sanity top/D3 only one pair'}
(out/'run-metadata.json').write_text(json.dumps(record,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print(f'SEALED {len(source_hashes)} sources / {len(artifacts)} artifacts / {len(record["binaries"])} binaries; drift {len(drift["changed"])}/{len(drift["missing"])}')
