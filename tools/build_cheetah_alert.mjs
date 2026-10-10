// Build only our fixture; never reads the live game or installs into it.
import { readFileSync, writeFileSync, mkdirSync, copyFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';
const repo = resolve(import.meta.dirname, '..'), sample = resolve(repo, 'examples/cheetah-alert');
const source = resolve(repo, 'tests/fixtures/casecheetah');
const rules = readFileSync(resolve(source,'UI/src/shell/shell.css'),'utf8').split(/\r?\n/).slice(4,9);
if(rules.length !== 5 || !rules[0].startsWith('.nine-slice {')) throw new Error('NineSlice source drift');
writeFileSync(resolve(sample,'src/profile.css'),rules.join('\n')+'\n'+readFileSync(resolve(sample,'src/layout.css'),'utf8'));
mkdirSync(resolve(sample,'public/art'),{recursive:true});
copyFileSync(resolve(source,'UI/public/art/ink-panel-v2.png'),resolve(sample,'public/art/ink-panel-v2.png'));
for(const [from,to] of [['third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf','Roboto-Regular.ttf'],['third_party/noto-cjk/source/NotoSansCJKsc-Regular.otf','NotoSansCJKsc-Regular.otf']]) copyFileSync(resolve(repo,from),resolve(sample,'public',to));
const notices = readFileSync(resolve(repo,'third_party/react-runtime/BUNDLED-LICENSES.txt'),'utf8')+'\nZustand 5.0.15\n'+readFileSync(resolve(repo,'third_party/zustand/source/LICENSE'),'utf8')+'\nNoto Sans CJK SC Sans2.004\n'+readFileSync(resolve(repo,'third_party/noto-cjk/source/LICENSE'),'utf8')+'\nRoboto Regular 2.138\n'+readFileSync(resolve(repo,'third_party/probe-font-notices/NOTICE.txt'),'utf8')+'\n'+readFileSync(resolve(repo,'third_party/probe-font-notices/LICENSE-Apache-2.0.txt'),'utf8');
writeFileSync(resolve(sample,'public/BUNDLED-LICENSES.txt'),notices);
const vite = resolve(repo,'examples/react-vite/node_modules/vite/bin/vite.js');
for(const mode of ['native','browser']) {
  const r=spawnSync(process.execPath,[vite,'build','--config',resolve(sample,'vite.config.mjs'),'--mode',mode],{cwd:repo,stdio:'inherit'});
  if(r.status !== 0) process.exit(r.status ?? 1);
}
mkdirSync(resolve(sample,'native'),{recursive:true});
copyFileSync(resolve(sample,'dist/native/app.js'),resolve(sample,'native/app.js'));
writeFileSync(resolve(sample,'native/BUNDLED-LICENSES.txt'),notices);
const lock=JSON.parse(readFileSync(resolve(repo,'third_party/manifest.json'),'utf8'));
const inputs=['tests/fixtures/casecheetah/manifest.json',...Object.keys(JSON.parse(readFileSync(resolve(source,'manifest.json'),'utf8')).files).map(p=>'tests/fixtures/casecheetah/'+p),
 'examples/react-vite/package-lock.json','examples/cheetah-alert/vite.config.mjs','examples/cheetah-alert/index.html','examples/cheetah-alert/src/main.tsx','examples/cheetah-alert/src/layout.css','examples/cheetah-alert/src/profile.css','tools/build_cheetah_alert.mjs',
 'third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf','third_party/probe-font-notices/NOTICE.txt','third_party/probe-font-notices/LICENSE-Apache-2.0.txt',
 ...lock.dependencies.filter(d=>['zustand','noto-cjk'].includes(d.name)).flatMap(d=>Object.keys(d.files).map(p=>d.source_dir+'/'+p))];
const outputs=['examples/cheetah-alert/native/app.js','examples/cheetah-alert/native/BUNDLED-LICENSES.txt'];
const hash=p=>createHash('sha256').update(readFileSync(resolve(repo,p))).digest('hex');
writeFileSync(resolve(sample,'native/metadata.json'),JSON.stringify({fixture:'casecheetah-alert-v1',react:'19.3.0',vite:'8.3.3',zustand:'5.0.15',noto:'Sans2.004',note:'Unmodified captured AlertBar/store/focus/bridge/NineSlice; authored preview CSS and data; local bridge captures only',inputs:Object.fromEntries(inputs.map(p=>[p,hash(p)])),outputs:Object.fromEntries(outputs.map(p=>[p,hash(p)]))},null,2)+'\n');
