// Host-authored inspection shell, using native DOM/CSS/scroll/hit testing.
// All registry entries remain visible; diagnostic cards are NEVER pixel passes.
(()=>{
  const groups=[
    ['CSS_TESTS','CSS',4,288,12],['D3_TESTS','D3',4,384,16],
    ['D3_INTERACTIVE_TESTS','D3 Interactive',1,1500,16],['KLINE_TESTS','K-Line',1,800,16],
    ['WEBGL_TESTS','Canvas / WebGL',1,600,16],['FRAMER_TESTS','Framer Motion',3,320,16],
    ['SPRING_TESTS','React Spring',3,288,16],['ICON_TESTS','Icons',2,288,16],
    ['ANIME_TESTS','Anime.js',4,288,12],['LOTTIE_TESTS','Lottie',4,288,12]
  ];
  const add=(parent,id,cls,text,style)=>{
    const e=document.createElement('div');if(id)e.id=id;if(cls)e.setAttribute('class',cls);
    if(text)e.textContent=text;if(style)Object.assign(e.style,style);parent.appendChild(e);return e;
  };
  const shell=add(document.body,'shell');
  add(shell,'title','','CaseElephant / CRPG Gallery — 72 original cases');
  add(shell,'summary','','Inspecting original packages…');
  const sidebar=add(shell,'sidebar');
  const scroller=add(shell,'scroller'),content=add(scroller,'content');
  add(shell,'detail'); // The host paints inspection text separately from case layout.
  let y=0;const offsets={},cards={};
  for(const [group,name,columns,height,gap] of groups){
    offsets[group]=y;const records=__ceGalleryCases.filter(c=>c.group===group);
    add(content,'group-'+group,'heading',name+' / '+records.length+' cases',{top:y+'px'});y+=44;
    const width=(1600-gap*(columns-1))/columns;
    for(let i=0;i<records.length;i++){
      const c=records[i],card=add(content,'case-'+c.id,'card','',{
        left:(i%columns*(width+gap))+'px',top:(y+Math.floor(i/columns)*(height+40+gap))+'px',width:width+'px',height:(height+40)+'px'
      });
      add(card,'','label',c.id+'  |  '+c.label,{width:(width-16)+'px'});
      const body=add(card,'body-'+c.id,'body','',{width:(width-4)+'px',height:height+'px'});
      const live=c.group==='WEBGL_TESTS' && c.index<2;const scene=!['WEBGL_TESTS','LOTTIE_TESTS'].includes(c.group);
      if(scene){add(body,'slot-'+c.id,'live','',{left:'0px',top:'0px',width:(width-4)+'px',height:height+'px'});
      }else if(live){
        add(body,'','live-note','LIVE / original React + Canvas2D / bitmap 800×600 / native D3D11');
        add(body,'slot-'+c.id,'live','',{left:((width-4-800)/2)+'px',top:'0px',width:'800px',height:'600px'});
      }else{
        add(body,'diagnostic-'+c.id,'diagnostic','Pixels: NOT IMPLEMENTED',{width:(width-32)+'px',height:'90px'});
        add(body,'execution-'+c.id,'execution','JS: waiting for isolated observation',{width:(width-32)+'px',height:'160px'});
      }
      card.addEventListener('click',()=>{
        const r=__cePreview.results?.find(r=>r.id===c.id);
        __cePreview.detail=c.id+' '+c.label+' | '+(r?r.reason+' | JS: '+r.execution+' '+r.failure:'not observed');
      });cards[c.id]=card;
    }
    y+=Math.ceil(records.length/columns)*(height+40+gap)+40;
    const nav=add(sidebar,'nav-'+group,'nav',name+' ('+records.length+')');
    nav.addEventListener('click',()=>__cePreview.jump(group));
  }
  content.style.height=y+'px';
  const profiler=add(sidebar,'profiler-toggle','nav','Profiler: OFF (F8)');
  profiler.addEventListener('click',()=>__cePreview.setProfiler(!__cePreview.profiler));
  add(sidebar,'scope','','4K output / two frames in flight\n66 native cases / 6 pending\nFPS = shell + visible LIVE cards.\nProfiler: curves + phase timers\nHome/End, PgUp/PgDn, wheel; F5 resets timing.');
  globalThis.__cePreview={groups,offsets,cards,results:null,profiler:false,detail:'Click a case for its blocker summary; full errors are in preview-cases.json.',
    setProfiler(enabled){this.profiler=!!enabled;profiler.textContent='Profiler: '+(this.profiler?'ON':'OFF')+' (F8)';},
    jump(group){scroller.scrollTop=offsets[group]??0;for(const [key] of groups)document.getElementById('nav-'+key).setAttribute('data-active',String(key===group));},
    scroll(delta){scroller.scrollTop+=delta;},
    update(results){this.results=results;let clean=0;for(const r of results){
      clean+=r.execution==='no_error_observed';
      const d=document.getElementById('diagnostic-'+r.id),e=document.getElementById('execution-'+r.id);
      if(d)d.textContent='Pixels: NOT PASSED — '+r.reason;
      if(e)e.textContent='JS: '+r.execution+(r.failure?'\n'+r.failure.split('\n')[0].slice(0,160):' (12 frames, no input; not a rendering pass)');
    }
    document.getElementById('summary').textContent='72 registered / 64 CSS + SVG scenes + 2 Canvas native cases / 6 pending visual integration / '+clean+' JS observations without error';
  }};
})();
