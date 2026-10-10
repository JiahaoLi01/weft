// Build only CaseElephant files, using its already pinned React/Vite runtime.
import { readFileSync, writeFileSync, mkdirSync, copyFileSync, readdirSync } from 'node:fs';
import { resolve, relative } from 'node:path';
import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';
const repo = resolve(import.meta.dirname, '..');
const sample = resolve(repo, 'examples/cheetah-nine-slice');
const source = resolve(repo, 'tests/fixtures/casecheetah');
const shell = readFileSync(resolve(source, 'UI/src/shell/shell.css'), 'utf8');
const lines = shell.split(/\r?\n/).slice(4,9);
if (lines.length !== 5 || !lines[0].startsWith('.nine-slice {') || !lines[4].startsWith('.nine-slice-content {')) throw new Error('Snapshot CSS section changed');
writeFileSync(resolve(sample, 'src/profile.css'), lines.join('\n') + '\n' + readFileSync(resolve(sample, 'src/layout.css'), 'utf8'));
mkdirSync(resolve(sample, 'public/art'), {recursive:true});
for (const file of readdirSync(resolve(source, 'UI/public/art'))) copyFileSync(resolve(source, 'UI/public/art',file),resolve(sample,'public/art',file));
copyFileSync(resolve(repo, 'third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf'), resolve(sample, 'public/Roboto-Regular.ttf'));
const vite = resolve(repo, 'examples/react-vite/node_modules/vite/bin/vite.js');
for (const mode of ['native','browser']) {
  const r = spawnSync(process.execPath, [vite,'build','--config',resolve(sample,'vite.config.mjs'),'--mode',mode], {cwd:repo,stdio:'inherit'});
  if(r.status !== 0) process.exit(r.status ?? 1);
}
mkdirSync(resolve(sample,'native'),{recursive:true});
copyFileSync(resolve(sample,'dist/native/app.js'),resolve(sample,'native/app.js'));
copyFileSync(resolve(repo,'third_party/react-runtime/BUNDLED-LICENSES.txt'),resolve(sample,'native/BUNDLED-LICENSES.txt'));
const hash = path => createHash('sha256').update(readFileSync(resolve(repo,path))).digest('hex');
const inputs = ['tests/fixtures/casecheetah/manifest.json',...Object.keys(JSON.parse(readFileSync(resolve(source,'manifest.json'),'utf8')).files).map(p=>'tests/fixtures/casecheetah/'+p),
  'examples/react-vite/package-lock.json','examples/cheetah-nine-slice/vite.config.mjs','examples/cheetah-nine-slice/index.html','examples/cheetah-nine-slice/src/main.tsx','examples/cheetah-nine-slice/src/layout.css','examples/cheetah-nine-slice/src/profile.css','tools/build_cheetah_fixture.mjs'];
const outputs = ['examples/cheetah-nine-slice/native/app.js','examples/cheetah-nine-slice/native/BUNDLED-LICENSES.txt'];
writeFileSync(resolve(sample,'native/metadata.json'),JSON.stringify({fixture:'casecheetah-nine-slice-v1',react:'19.3.0',vite:'8.3.3',note:'Unmodified captured TSX; isolated authored shell, using CaseElephant pinned runtime instead of game runtime 19.2.8/6.4.3',inputs:Object.fromEntries(inputs.map(p=>[p,hash(p)])),outputs:Object.fromEntries(outputs.map(p=>[p,hash(p)]))},null,2)+'\n');
console.log('Built immutable component workload without accessing the live game');
