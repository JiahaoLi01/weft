(function(){
  let frame=0,seed=0x731942;
  const random=n=>{seed=(Math.imul(seed,1664525)+1013904223)>>>0;return seed%n;};
  const el=(tag,id,style='',text='')=>{const n=document.createElement(tag);n.id=id;if(style)n.setAttribute('style',style);if(text)n.appendChild(document.createTextNode(text));return n;};
  const text=(n,s)=>n.appendChild(document.createTextNode(s));
  const gallery=el('div','gallery');document.body.appendChild(gallery);
  const title=el('div','title');text(title,'CaseElephant / ');title.appendChild(el('span','accent','','行内排版'));gallery.appendChild(title);
  gallery.appendChild(el('div','subtitle','','HTML · Unicode wrapping · Scroll · Rounded clips'));
  const viewport=el('div','viewport'),content=el('div','content');viewport.appendChild(content);gallery.appendChild(viewport);
  const intro=el('div','intro');text(intro,'库存：');const emphasis=el('span','emphasis','','仓库与运输');intro.appendChild(emphasis);text(intro,'已连接。 中文标点（不会留在行首）与 English words 共用行盒。 ');
  const nested=el('span','nested','','A\u00a0B / cafe\u0301');intro.appendChild(nested);text(intro,' 也会保留不可断空格和组合字符。');content.appendChild(intro);
  const inner=el('div','inner');inner.appendChild(el('div','stripe'));inner.appendChild(el('div','inner-label','','Nested elliptical clipping'));content.appendChild(inner);
  for(let i=0;i<12;i++)content.appendChild(el('div','record-'+i,'','0'+i+' / 港口仓库 · Unicode 与 mixed inline text'));
  const footer=el('div','footer','','拖动/滚动列表，或点击下一步改变宽度与内容。');gallery.appendChild(footer);
  const controls=el('div','controls'),advance=el('button','advance','','下一步 / Reflow'),reset=el('button','reset','','回到顶部');controls.appendChild(advance);controls.appendChild(reset);gallery.appendChild(controls);
  const scenes=el('div','scenes');document.body.appendChild(scenes);
  for(let i=0;i<8;i++){
    const root=el('div','scene-'+i,`display:block;position:relative;width:${125+random(160)}px;padding:2px 3px;font-family:ProbeRoboto, ProbeCjk;font-size:16px;line-height:24px;font-weight:400;font-style:normal`);
    text(root,'  AV ');const outer=el('span','outer-'+i,'color:#b65b3a','仓库（运输）');outer.appendChild(el('span','small-'+i,'font-size:12px;line-height:18px',' / stock '));root.appendChild(outer);
    text(root,' A\u00a0B / cafe\u0301 ');root.appendChild(el('span','tail-'+i,'','中文标点，不能拆开。 next words'));
    if(i%2){const block=el('div','block-'+i,'display:block;height:7px');root.appendChild(block);text(root,'anonymous block after a block child');}
    scenes.appendChild(root);
  }
  function advanceFrame(){if(frame>=2)return;frame++;
    if(frame===1){intro.style.width='344px';emphasis.style.fontSize='20px';viewport.scrollTop=63;viewport.scrollLeft=29;}
    else if(frame===2){for(let i=11;i>=0;i--){const n=document.getElementById('record-'+i);content.removeChild(n);}intro.textContent='内容缩短，滚动偏移自动夹到新的有效范围。';viewport.style.borderRadius='48px 12px / 22px 36px';}
    for(let i=0;i<8;i++){
      const root=document.getElementById('scene-'+i);root.style.width=(115+random(145))+'px';
      document.getElementById('tail-'+i).textContent=frame===1?'扩展的中文段落，more words / Unicode 换行测试。':'中文标点，不能拆开。 next words';
      const outer=document.getElementById('outer-'+i);if(frame===1)outer.appendChild(el('span','added-'+i,'color:#44846b',' 变化 '));else outer.removeChild(document.getElementById('added-'+i));
    }
    advance.setAttribute('data-frame',String(frame));
    if(frame===2){advance.textContent='已完成 / Done';advance.setAttribute('disabled','');}
  }
  advance.addEventListener('click',advanceFrame);
  reset.addEventListener('click',()=>{viewport.scrollTop=0;viewport.scrollLeft=0;reset.setAttribute('data-reset','1');});
  Object.assign(globalThis,{flowLayoutFixture:{advance:advanceFrame}});
})();
