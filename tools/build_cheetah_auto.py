"""Compose automatic geometry; never read or write the live game."""
import hashlib, json, re, shutil
from pathlib import Path
root=Path(__file__).resolve().parents[1]
sample=root/'examples/cheetah-auto';sample.mkdir(parents=True,exist_ok=True)
old=(root/'examples/cheetah-style/profile.css').read_text(encoding='utf-8')
css=re.sub(r'\.alert[^{}]*\{[^}]*\}\s*','',old)
css+='''
/* Authored bounded automatic-layout adapter; original React is unchanged. */
.alert-bar {position:absolute;left:26px;top:16px;display:flex;gap:8px}
.alert-chip {position:relative;display:block}
.alert-chip > button {display:flex;align-items:center;gap:6px;padding:5px 12px 5px 6px;box-sizing:content-box;background-color:rgba(16,18,17,.78);color:var(--ops-soft);font-size:11px;line-height:22px}
.alert-chip > button i {width:22px;height:22px;line-height:22px;font-size:13px;text-align:center;background-color:rgba(230,201,139,.16);color:var(--ops-gold)}
.alert-chip > button b {font-size:14px;line-height:22px;color:var(--ops-text)}
.alert-chip > button span {font-size:11px;line-height:22px;color:inherit}
.alert-chip > button[aria-expanded="true"] {color:var(--ops-text)}
.alert-list {position:absolute;top:calc(100% + 6px);left:0;width:340px;padding:8px 12px;box-sizing:content-box;list-style:none;background-color:rgba(16,18,17,.9)}
.alert-list li {padding:1px 0;list-style:none;font-size:11px;line-height:14px}
.alert-list button {width:100%;font-size:11px;line-height:14px;color:var(--ops-text);background-color:transparent}
.alert-list .quiet {font-size:11px;line-height:14px;color:var(--ops-dim)}
'''
css=re.sub(r'(?m)^\.alert', '#gallery .alert', css)
(sample/'profile.css').write_text(css,encoding='utf-8')
html=(root/'examples/cheetah-style/index.html').read_text(encoding='utf-8').replace('原始主题变量','自动布局')
(sample/'index.html').write_text(html,encoding='utf-8')
copies={'examples/cheetah-alert/native/app.js':'app.js','examples/cheetah-alert/native/BUNDLED-LICENSES.txt':'BUNDLED-LICENSES.txt','third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf':'Roboto-Regular.ttf','third_party/noto-cjk/source/NotoSansCJKsc-Regular.otf':'NotoSansCJKsc-Regular.otf','tests/fixtures/casecheetah/UI/public/art/ink-panel-v2.png':'art/ink-panel-v2.png'}
for src,dst in copies.items():
    target=sample/dst;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(root/src,target)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
inputs=['tools/build_cheetah_auto.py','examples/cheetah-style/profile.css','examples/cheetah-style/index.html',*copies]
(sample/'metadata.json').write_text(json.dumps({'fixture':'casecheetah-auto-v1','inputs':{p:sha(root/p) for p in inputs},'outputs':{p.relative_to(root).as_posix():sha(p) for p in sample.rglob('*') if p.is_file() and p.name!='metadata.json'},'note':'Unchanged captured React/AlertBar/store; authored automatic geometry; no live game IO'},indent=2)+'\n',encoding='utf-8')
print('PASS prepared automatic-layout preview')
