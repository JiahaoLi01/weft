#define NOMINMAX
#include "../probes/gallery_cluster_input.hpp"
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <cstring>
#include "scene_test_image.hpp"
#include "cluster_visual_compare.hpp"
int main(int argc,char** argv){CoInitializeEx(nullptr,COINIT_MULTITHREADED);unsigned failed{},captures{},passed{};
    const auto out=std::filesystem::path(argc>1?argv[1]:"artifacts/local/gallery-cluster/native");std::filesystem::create_directories(out);
    const char* groups[]{"D3_INTERACTIVE_TESTS","KLINE_TESTS","FRAMER_TESTS","SPRING_TESTS","ICON_TESTS","ANIME_TESTS"};const char* prefixes[]{"d3_interactive","kline","framer","spring","icon","anime"};const unsigned counts[]{4,1,6,3,2,14};const float widths[]{1596,1596,518.6666667f,518.6666667f,788,387},heights[]{1500,800,320,288,288,288};
    std::ofstream report(out/"native-captures.jsonl");
    for(unsigned g=0;g<6;++g)for(unsigned i=0;i<counts[g];++i){const auto id=std::string(prefixes[g])+"-"+(i<10?"0":"")+std::to_string(i);if(argc>2 && id!=argv[2])continue;std::cout<<id<<std::flush;try{
        GallerySceneSession session(groups[g],i,widths[g],heights[g],"cluster-utilities.css");gallery_cluster_prepare(session,id);
        bool oracle_rejected{};std::size_t changed{};std::vector<std::uint8_t> previous;
        for(unsigned n=0;n<=300;++n){const auto time=n*(1000./60);session.step(time);
            gallery_cluster_inputs(session,n,&std::cout);
            if(n!=24&&n!=63&&n!=90&&n!=120&&n!=180&&n!=240&&n!=300)continue;
            const auto image=session.bitmap();png(out/(id+"-"+std::to_string(n)+".png"),image);auto ref=scene_reference(std::filesystem::path(CE_SOURCE_DIR)/"tests/fixtures/gallery-cluster-reference"/(id+"-"+std::to_string(n)+".png"));auto d=scene_difference(image,ref);const auto mean=*std::max_element(d.mean.begin(),d.mean.end());
            // Audit the HTML boxes compared by the independent verifier.
            // SVG pixels, CTM and hit semantics have separate counterexamples.
            {std::ofstream geometry(out/(id+"-"+std::to_string(n)+"-geometry.json"));geometry<<session.vm.eval("JSON.stringify(Array.from(document.getElementById('root').querySelectorAll('div,button'),n=>({tag:n.localName,in_defs:(()=>{for(let p=n;p;p=p.parentElement)if(p.localName==='defs')return true;return false})(),rect:(r=>[r.x,r.y,r.width,r.height])(n.getBoundingClientRect())})))");}
            if(!previous.empty())for(std::size_t p=0;p<image.rgba.size();p+=4)changed+=std::memcmp(image.rgba.data()+p,previous.data()+p,4)!=0;previous=image.rgba;
            if(!oracle_rejected && d.foreground>=200){auto blank=image;for(std::size_t p=0;p<blank.rgba.size();p+=4){blank.rgba[p]=5;blank.rgba[p+1]=4;blank.rgba[p+2]=3;blank.rgba[p+3]=255;}oracle_rejected=!scene_difference(blank,ref).passes();}
            report<<"{\"id\":\""<<id<<"\",\"frame\":"<<n<<",\"mean\":"<<mean<<",\"iou\":"<<d.soft_iou<<",\"foreground\":"<<d.foreground<<",\"state\":"<<session.vm.eval("JSON.stringify(__ceGallery.state())")<<",\"dom\":"<<session.vm.eval("JSON.stringify(Array.from(document.getElementById('root').querySelectorAll('*'),n=>({tag:n.localName,style:n.getAttribute('style'),text:n.children.length?'':n.textContent})))")<<"}\n";
            std::cout<<" ["<<n<<":"<<mean<<","<<d.soft_iou<<"]"<<std::flush;++captures;
            // Diagnostic mode retains every failure/capture during integration.
            if(!cluster_image_match(image,ref))++failed;
        }
        require(oracle_rejected,"blank content was not rejected");session.unmount();require(session.vm.stats().live_nodes==3,"node leak");++passed;std::cout<<" done changed="<<changed<<"\n";
    }catch(const std::exception& e){++failed;std::cout<<" FAIL "<<e.what()<<'\n';}}
    std::cout<<"cases="<<passed<<" captures="<<captures<<" failures="<<failed<<'\n';CoUninitialize();return failed?1:0;
}
