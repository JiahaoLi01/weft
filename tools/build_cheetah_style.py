"""Compose a new fixed style workload, preserving earlier AlertBar evidence."""
import hashlib
import json
import re
import shutil
from pathlib import Path
root=Path(__file__).resolve().parents[1]
sample=root/'examples/cheetah-style'
sample.mkdir(parents=True,exist_ok=True)
original=root/'tests/fixtures/casecheetah/UI/src/operations.css'
text=original.read_text(encoding='utf-8')
theme=re.search(r':root \{.*?\n\}',text,re.S).group()
expanded=re.search(r'\.alert-chip > button\[aria-expanded="true"\] \{[^}]*\}',text).group()
layout=(root/'examples/cheetah-alert/src/profile.css').read_text(encoding='utf-8')
overrides='''/* Authored adapter: keep explicit geometry and font resources. */
#gallery { color:var(--ops-text); }
#subtitle, #reply-status { color:var(--ops-soft); }
.alert-chip > button { background-color:rgba(16,18,17,.78); color:var(--ops-soft); }
.alert-chip > button i { color:var(--ops-gold); }
.alert-chip > button b { color:var(--ops-text); }
.alert-list { background-color:rgba(16,18,17,.9); }
.alert-list button, #selection { color:var(--ops-text); }
.alert-list .quiet { color:var(--ops-dim); }
#many, #reply { color:var(--ops-gold); }
'''
(sample/'profile.css').write_text(theme+'\n'+layout+'\n'+overrides+expanded+'\n',encoding='utf-8')
(sample/'index.html').write_text('''<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><title>CaseElephant / 原始主题变量</title>
<link rel="stylesheet" href="profile.css"><style>
@font-face{font-family:ProbeRoboto;src:url('./Roboto-Regular.ttf')}
@font-face{font-family:ProbeCjk;src:url('./NotoSansCJKsc-Regular.otf')}
html,body,#root{margin:0;width:640px;height:420px}body{background:#596864}
</style></head><body><div id="root"></div><script src="app.js"></script></body></html>''',encoding='utf-8')
copies={
 'examples/cheetah-alert/native/app.js':'app.js',
 'examples/cheetah-alert/native/BUNDLED-LICENSES.txt':'BUNDLED-LICENSES.txt',
 'third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf':'Roboto-Regular.ttf',
 'third_party/noto-cjk/source/NotoSansCJKsc-Regular.otf':'NotoSansCJKsc-Regular.otf',
 'tests/fixtures/casecheetah/UI/public/art/ink-panel-v2.png':'art/ink-panel-v2.png'}
for src,dst in copies.items():
    target=sample/dst;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(root/src,target)
inputs=['tools/build_cheetah_style.py','examples/cheetah-alert/src/profile.css','examples/cheetah-alert/native/metadata.json',
        'tests/fixtures/casecheetah/UI/src/operations.css',*copies]
hash_file=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
outputs={p.relative_to(root).as_posix():hash_file(p) for p in sample.rglob('*') if p.is_file() and p.name!='metadata.json'}
(sample/'metadata.json').write_text(json.dumps({'fixture':'casecheetah-style-v1','inputs':{p:hash_file(root/p) for p in inputs},'outputs':outputs,
 'original_css':{'theme':theme,'expanded_rule':expanded},'note':'Exact captured :root block and expanded rule; authored explicit geometry; unchanged AlertBar IIFE; no live game IO'},indent=2)+'\n',encoding='utf-8')
print('PASS built original-theme AlertBar style workload')
