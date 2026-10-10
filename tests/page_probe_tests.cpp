#include "caseelephant/page_probe.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool value,const char* error) { if(!value) throw std::runtime_error(error); }
std::string read(const char* name) {
    std::ifstream file(std::string(CE_SOURCE_DIR)+name,std::ios::binary);
    require(bool(file),"fixture not found"); std::ostringstream s; s<<file.rdbuf(); return s.str();
}
void pump(caseelephant::ScriptDomProbe& p,unsigned t=0) {
    for(unsigned i=0;i<32;++i) { p.eval("__ceHost.pump("+std::to_string(t)+")"); if(p.eval("__ceHost.pending()")=="0") return; }
    throw std::runtime_error("tasks did not drain");
}
}
int main() {
    try {
        caseelephant::TextProbeFont font(CE_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf");
        caseelephant::ScriptProbeConfig config{1024*1024};
#ifdef CE_REACT_GC_STRESS
        config.execution_budget_ms=30000; // GC at every allocation is deliberate instrumentation.
#endif
        caseelephant::ScriptDomProbe p(config);
        const auto bootstrap=read("/probes/web_host_bootstrap.js"),bundle=read("/examples/react-vite/native/app.js");
        p.eval(bootstrap);
        require(p.eval(R"JS(
          let n=document.createElement('div'); n.setAttribute('id','id with spaces');document.body.appendChild(n);
          n.style.width='20px'; n.style.setProperty('color','#123456'); n.style.height='30px';
          n.setAttribute('style','display:flex; width:40px; color:#abcdef;');
          let ok=document.getElementById('id with spaces')===n && n.style.width==='40px' && n.style.color==='#abcdef' && n.style.height==='';
          n.style.removeProperty('width'); ok && !n.getAttribute('style').includes('width');
        )JS")=="true","CSS declaration diverged from native style attribute / exact ID lookup");
        p.reset_document();
        for(unsigned cycle=0;cycle<8;++cycle) {
            p.eval(bootstrap); p.eval(bundle); pump(p);
            p.collect_garbage();
            const auto identity=p.node_identity("#increment");
            auto snapshot=p.snapshot_subtree("#page");
            const auto page=caseelephant::build_page_probe(snapshot,font);
            require(page.boxes.size()==25 && page.paints.size()>15,"real React page layout/paint");
            p.eval("document.getElementById('increment').dispatchEvent(new Event('click',{bubbles:true}));"); pump(p,16);
            require(p.eval("caseElephantReact.snapshot().count")=="Count: 1" && p.node_identity("#increment")==identity,"React click and identity after full GC");
            auto invalid=snapshot; invalid.attributes["style"]+=" opacity:0.5;";
            bool rejected{}; try{(void)caseelephant::build_page_probe(invalid,font);}catch(const std::invalid_argument&){rejected=true;}
            require(rejected,"unsupported paint accepted");
            invalid=snapshot; invalid.attributes["style"]="width:640px;";
            rejected=false; try{(void)caseelephant::build_page_probe(invalid,font);}catch(const std::invalid_argument&){rejected=true;}
            require(rejected,"block silently mapped to flex");
            invalid=snapshot; invalid.attributes["style"]+=" border-left-width:2px;";
            rejected=false; try{(void)caseelephant::build_page_probe(invalid,font);}catch(const std::invalid_argument&){rejected=true;}
            require(rejected,"unpainted border accepted");
            p.eval("caseElephantReact.unmount()"); pump(p,32); p.collect_garbage();
            require(p.stats().listener_exceptions==0,"React listener failed");
            p.reset_document();
            require(p.stats().live_nodes==3 && !p.is_live(identity),"React VM reset leaked or retained node identity");
            require(caseelephant::build_page_probe(snapshot,font).boxes.size()==25,"value snapshot depends on destroyed VM");
        }
        p.eval(bootstrap); p.eval("queueMicrotask(()=>{throw Error('microtask-proof');})");
        bool reported{}; try{pump(p);}catch(const std::runtime_error& e){reported=std::string(e.what()).find("microtask-proof")!=std::string::npos;}
        require(reported,"microtask exception silently dropped");
        p.reset_document(); p.eval(bootstrap);
        p.eval("globalThis.timerProof=[];setTimeout(()=>{timerProof.push(1);setTimeout(()=>timerProof.push(2),0)},0);setTimeout(()=>timerProof.push(99),10);");
        p.eval("__ceHost.pump(0)"); require(p.eval("timerProof.join(',')")=="1","nested timeout executed in same batch");
        p.eval("__ceHost.pump(0)"); require(p.eval("timerProof.join(',')")=="1,2","next timer batch missed");
        p.eval("__ceHost.pump(10)"); require(p.eval("timerProof.join(',')")=="1,2,99","timer due time ignored");
        std::cout<<"PASS native CSSOM reflection, exact ID lookup, 8 real React/GC/reset cycles, snapshot ownership, unsupported paint/format, microtask error and timer batches\n";
    } catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1; }
}
