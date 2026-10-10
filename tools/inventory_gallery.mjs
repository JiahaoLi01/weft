import {readFileSync,writeFileSync} from 'node:fs';
import {resolve} from 'node:path';
import {createHash} from 'node:crypto';
import {createRequire} from 'node:module';
const repo=resolve(import.meta.dirname,'..'),require=createRequire(resolve(repo,'third_party/gallery-runtime/package.json'));
const ts=require('typescript'),fixture=resolve(repo,'tests/fixtures/crpg-gallery');
const path=resolve(fixture,'UI/src/components/debug/TestGallery.tsx'),text=readFileSync(path,'utf8');
const source=ts.createSourceFile(path,text,ts.ScriptTarget.Latest,true,ts.ScriptKind.TSX);
if(source.parseDiagnostics.length)throw new Error('Original gallery did not parse');
const groups=[];
const requirements={CSS_TESTS:['CSS cascade/layout/paint','CSS transitions/keyframes','hover/click'],D3_TESTS:['D3 SVG DOM','SVG path/text/transform paint','rAF/timers'],D3_INTERACTIVE_TESTS:['SVG geometry','pointer/drag/wheel','D3 transitions','tooltip layout'],KLINE_TESTS:['SVG paint','pointer/crosshair/tooltip'],WEBGL_TESTS:['Canvas2D or WebGL2','rAF','canvas texture composition'],FRAMER_TESTS:['computed style','rAF','transform/opacity','layout measurement','presence/gesture'],SPRING_TESTS:['rAF','computed style','animated values'],ICON_TESTS:['SVG stroke/path paint','timers'],ANIME_TESTS:['rAF','computed style','SVG path geometry','drag/layout'],LOTTIE_TESTS:['XHR/asset provider','SVG paint','rAF','remote animation JSON']};
for(const statement of source.statements)if(ts.isVariableStatement(statement))for(const declaration of statement.declarationList.declarations){
  const name=declaration.name.getText(source);if(!name.endsWith('_TESTS') || !declaration.initializer || !ts.isArrayLiteralExpression(declaration.initializer))continue;
  const cases=declaration.initializer.elements.map((element,index)=>{
    if(!ts.isObjectLiteralExpression(element))throw new Error('Unknown test registration form: '+name);
    const label=element.properties.find(p=>ts.isPropertyAssignment(p) && p.name.getText(source)==='label');
    if(!label || !ts.isStringLiteral(label.initializer))throw new Error('Missing literal case label');
    const start=element.getStart(source),end=element.end,bytes=Buffer.from(text.slice(start,end));
    return {id:name.toLowerCase().replace('_tests','')+'-'+String(index).padStart(2,'0'),label:label.initializer.text,index,
      source_line:source.getLineAndCharacterOfPosition(start).line+1,source_start:start,source_end:end,
      utf8_start:Buffer.byteLength(text.slice(0,start)),utf8_end:Buffer.byteLength(text.slice(0,end)),case_source_sha256:createHash('sha256').update(bytes).digest('hex'),
      native_status:'not_run',browser_status:'not_run',requirements:requirements[name] ?? ['unclassified']};
  });groups.push({name,count:cases.length,cases});
}
const result={format:'gallery-original-case-inventory-v1',source:'UI/src/components/debug/TestGallery.tsx',source_sha256:createHash('sha256').update(text).digest('hex'),case_count:groups.reduce((n,g)=>n+g.count,0),
  levels:{execution:'Original case executes without unsupported calls/errors',pixels:'Our renderer produces independently checked pixels',interaction:'Original controls/animations perform expected behavior',performance:'Comparable fixed conditions and tail metrics; separate from correctness'},
  note:'Registration inventory, not evidence of passing cases. Original labels including historical failure marks are preserved.',groups};
writeFileSync(resolve(fixture,'inventory.json'),JSON.stringify(result,null,2)+'\n');
console.log(groups.map(g=>`${g.name}: ${g.count}`).join('\n'));console.log('Original registered cases: '+result.case_count);
