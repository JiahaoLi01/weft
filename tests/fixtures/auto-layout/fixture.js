// Same deterministic DOM mutations execute in Chromium and the native VM.
(function () {
  let seed=0x6e6f7761,frame=0;
  const random=n=>{seed=(Math.imul(seed,1664525)+1013904223)>>>0;return seed%n;};
  const el=(id,style,text='')=>{const e=document.createElement('div');e.id=id;e.setAttribute('style',style);if(text)e.appendChild(document.createTextNode(text));return e;};
  const host=el('scenes','');document.body.appendChild(host);
  for(let i=0;i<16;i++){
    const root=el('scene-'+i,`display:block;position:relative;width:${180+random(160)}px;padding:${random(6)}px;font-family:ProbeRoboto, ProbeCjk;font-size:16px;line-height:20px;white-space:nowrap;font-weight:400;font-style:normal`);
    root.appendChild(el('title-'+i,'display:block;padding:2px 3px','仓库 / depot '+i));
    const row=el('row-'+i,`display:flex;gap:${random(9)}px;padding:${random(5)}px;align-items:${i%2?'center':'stretch'};flex-wrap:${i%3?'nowrap':'wrap'}`);
    for(let j=0;j<3;j++)row.appendChild(el('item-'+i+'-'+j,`display:block;padding:${random(4)}px;${j===2?'flex-grow:1;min-width:0;':''}`,j===0?'A仓B':j===1?'office AV':'leaf'));
    root.appendChild(row);
    const wrap=el('wrap-'+i,'display:block;width:130px;white-space:normal;padding:2px','one two three four five six seven');root.appendChild(wrap);
    const abs=el('abs-'+i,'display:block;position:absolute;right:2px;top:1px;width:20px;height:7px');root.appendChild(abs);
    const nested=el('nested-'+i,'display:block;padding:2px 4px');nested.appendChild(el('inside-'+i,'display:block;min-height:12px','nested'));root.appendChild(nested);
    host.appendChild(root);
  }
  function advance(){frame++;
    for(let i=0;i<16;i++){
      const root=document.getElementById('scene-'+i);root.style.width=(160+random(180))+'px';
      document.getElementById('row-'+i).style.gap=random(9)+'px';
      document.getElementById('item-'+i+'-0').textContent=frame===1?'更长的仓库 / Longer':'A仓B';
      document.getElementById('item-'+i+'-1').style.display=frame===1 && i%2?'none':'block';
      const nested=document.getElementById('nested-'+i);
      if(frame===1)nested.appendChild(el('added-'+i,'display:block;height:9px'));
      else nested.removeChild(document.getElementById('added-'+i));
    }
    document.getElementById('advance').setAttribute('data-frame',String(frame));
  }
  const button=document.createElement('button');button.id='advance';button.textContent='Next mutation';button.addEventListener('click',advance);document.body.appendChild(button);
  Object.assign(globalThis,{autoLayoutFixture:{advance}});
})();
