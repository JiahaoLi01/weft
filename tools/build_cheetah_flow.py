"""Prepare the captured React workload; never access the live game."""
import hashlib,json,shutil
from pathlib import Path
root=Path(__file__).resolve().parents[1];sample=root/'examples/cheetah-flow';sample.mkdir(parents=True,exist_ok=True)
css=(root/'examples/cheetah-auto/profile.css').read_text(encoding='utf-8')+'''
/* Authored flow adapter: Unicode normal wrapping and overlay scroll list. */
#gallery .alert-chip > button {border-radius:9px}
#gallery .alert-chip > button i {border-radius:6px}
#gallery .alert-list {max-height:104px;overflow:auto;border-radius:12px 6px 16px 8px}
#gallery .alert-list li, #gallery .alert-list button {white-space:normal}
#many, #reply {border-radius:8px}
'''
(sample/'profile.css').write_text(css,encoding='utf-8')
html=(root/'examples/cheetah-auto/index.html').read_text(encoding='utf-8').replace('自动布局','Inline / Unicode / Scroll / Rounded').replace("url('./", "url('../cheetah-auto/").replace('src="app.js"','src="../cheetah-auto/app.js"').replace('</body>','<script src="scroll.js"></script></body>')
html=html.replace('</style>','#gallery .alert-list{scrollbar-width:none}\n</style>')
(sample/'index.html').write_text(html,encoding='utf-8')
image='tests/fixtures/casecheetah/UI/public/art/ink-panel-v2.png';(sample/'art').mkdir(exist_ok=True);shutil.copyfile(root/image,sample/'art/ink-panel-v2.png')
sha=lambda p:hashlib.sha256((root/p).read_bytes()).hexdigest()
names=['tools/build_cheetah_flow.py','examples/cheetah-auto/profile.css','examples/cheetah-auto/index.html','examples/cheetah-alert/native/app.js','examples/cheetah-flow/scroll.js',image]
(sample/'metadata.json').write_text(json.dumps({'fixture':'casecheetah-flow-v1','inputs':{p:sha(p) for p in names},'outputs':{p.relative_to(root).as_posix():sha(p.relative_to(root)) for p in sample.rglob('*') if p.is_file() and p.name!='metadata.json'},'note':'Unmodified pinned React/AlertBar/Zustand/UE bundle; authored profile and external scroll control'},indent=2)+'\n',encoding='utf-8')
print('PASS prepared captured React flow preview')
