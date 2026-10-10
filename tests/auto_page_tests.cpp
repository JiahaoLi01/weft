#include "caseelephant/auto_page_probe.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace caseelephant;
namespace {
void check(bool b,const char* s){if(!b)throw std::runtime_error(s);}
void near(float a,float b){check(std::abs(a-b)<.002f,"automatic layout geometry");}
std::string read(const std::string& path){std::ifstream f(path);std::ostringstream s;s<<f.rdbuf();return s.str();}
ProbeDomSnapshot node(std::string id,std::string style,std::string text=""){
    ProbeDomSnapshot n;n.type=1;n.local_name="div";n.namespace_uri="http://www.w3.org/1999/xhtml";n.attributes={{"id",id},{"style",style}};
    if(!text.empty()){ProbeDomSnapshot t;t.type=3;t.data=text;n.children.push_back(t);}return n;
}
template<class F>void reject(F f){try{f();}catch(const std::invalid_argument& e){check(std::string(e.what()).starts_with("CE_"),"layout diagnostic");return;}throw std::runtime_error("unsupported auto layout accepted");}
}
int main(int argc,char** argv){try{
    TextProbeFontSet fonts({CE_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf",CE_SOURCE_DIR "/third_party/noto-cjk/source/NotoSansCJKsc-Regular.otf"});
    const std::string base="display:block;position:relative;width:200px;font-family:ProbeRoboto, ProbeCjk;font-size:16px;line-height:20px;white-space:nowrap";
    auto root=node("root",base+";padding:2px 3px");root.children.push_back(node("a","display:block","仓"));root.children.push_back(node("b","display:block;padding:1px 4px","ABC"));
    root.children.push_back(node("abs","display:block;position:absolute;left:0;top:0;width:100px;height:999px"));
    auto page=build_auto_page_probe(root,"",300,200,fonts);near(page.geometry.boxes[0].height,46);near(page.geometry.boxes[0].width,206);near(page.geometry.boxes[1].x,3);near(page.geometry.boxes[2].y,22);
    auto auto_width=root;auto_width.attributes["style"]+=";width:auto";near(build_auto_page_probe(auto_width,"",300,200,fonts).geometry.boxes[0].width,300);
    check(page.paints[0].paint.shaped.glyphs[0].font_id==1,"CJK intrinsic measure/fallback paint");
    auto style=compute_style_probe(root,"#a{padding:var(--pad);--pad:1px 2px 3px 4px;padding-left:9px}",auto_page_style_profile());
    check(style.children[0].values.at("padding-bottom")=="3px" && style.children[0].values.at("padding-left")=="9px","shorthand substitution/longhand cascade");
    const auto invalid=compute_style_probe(root,"#a{padding:9px;padding:var(--pad);--pad:2px invalid}",auto_page_style_profile());
    for(const auto* k:{"padding-top","padding-left","padding-right","padding-bottom"})check(invalid.children[0].values.at(k)=="0","invalid shorthand must unset every edge without recascade");
    auto percent=root;percent.children[0].attributes["style"]="display:block;height:50%";reject([&]{build_auto_page_probe(percent,"",300,200,fonts);});
    for(const auto* bad:{"margin:1px","display:grid","padding:2%","display:block;gap:2px","white-space:normal","font-weight:700","width:calc(100% - 2px)"}){
        auto n=node("bad",base+';'+bad,"仓");reject([&]{build_auto_page_probe(n,"",300,200,fonts);});
    }
    auto mixed=root;mixed.children.push_back(node("inline","display:inline","a"));reject([&]{build_auto_page_probe(mixed,"",300,200,fonts);});
    auto stale=style;stale.identity.generation++;reject([&]{build_auto_page_probe(root,stale,300,200,fonts);});
    // Owning output remains usable after destroying/mutating its input.
    root.children.clear();check(page.paints.size()==2 && page.paints[0].paint.text=="仓","borrowed layout output");
    ScriptDomProbe vm;vm.eval(read(CE_SOURCE_DIR "/probes/web_host_bootstrap.js"));vm.eval(read(CE_SOURCE_DIR "/tests/fixtures/auto-layout/fixture.js"));
    std::ostringstream out;out<<"{\"fixture\":\"auto-layout-v1\",\"frames\":[";
    for(int frame=0;frame<3;frame++){
        if(frame)vm.eval("autoLayoutFixture.advance()");if(frame)out<<',';out<<'[';
        for(int i=0;i<16;i++){
            auto dom=vm.snapshot_subtree("#scene-"+std::to_string(i));auto p=build_auto_page_probe(dom,"",640,420,fonts);
            if(i)out<<',';out<<'[';for(std::size_t j=0;j<p.geometry.boxes.size();j++){
                auto b=p.geometry.boxes[j];if(j)out<<',';out<<"{\"id\":\""<<b.id<<"\",\"x\":"<<b.x<<",\"y\":"<<b.y<<",\"width\":"<<b.width<<",\"height\":"<<b.height<<'}';
            }out<<']';
            if(i==0){vm.collect_garbage();check(p.paints.front().paint.text=="仓库 / depot 0","GC invalidated value paint");}
        }out<<']';
    }
    out<<"],\"passed\":true}";check(argc==2,"report path");const std::filesystem::path path(argv[1]);std::filesystem::create_directories(path.parent_path());std::ofstream(path)<<out.str();
    std::cout<<"PASS automatic block/flex: 48 deterministic mutation scenes, intrinsic CJK, multiline ASCII, out-of-flow height and diagnostics\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
