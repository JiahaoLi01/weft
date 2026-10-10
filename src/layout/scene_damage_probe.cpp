#include "caseelephant/scene_probe.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace caseelephant {
ProbeSceneDamage scene_damage_probe(const ProbeSceneFrame& a,const ProbeSceneFrame& b,float scale){
    if(!std::isfinite(scale) || scale<=0 || scale>4)throw std::invalid_argument("CE_SCENE_DAMAGE_SCALE");
    ProbeSceneDamage out;const auto w=b.root.box[2],h=b.root.box[3];
    auto full=[&]{out.changed=out.full=true;out.rect={0,0,w,h};};
    if(a.root.identity!=b.root.identity || a.root.box[2]!=w || a.root.box[3]!=h || a.elements.empty() || b.elements.empty()){full();return out;}
    using Key=std::tuple<std::uint64_t,std::uint32_t,std::uint32_t>;
    auto key=[](ProbeNodeIdentity n){return Key{n.view_epoch,n.slot,n.generation};};
    std::map<Key,std::array<double,4>> old_boxes,new_boxes;
    for(const auto& e:a.elements)old_boxes.emplace(key(e.identity),e.rect);
    for(const auto& e:b.elements)new_boxes.emplace(key(e.identity),e.rect);
    std::array<double,4> bounds{std::numeric_limits<double>::infinity(),std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity()};
    auto add=[&](auto&& self,const ProbeSceneNode& n,const auto& boxes)->void{
        // Text may overhang its layout box; SVG/projective paint can have
        // nonlocal extents. The independent full path owns those cases.
        auto t=n.style.find("transform");
        if((n.style.contains("box-shadow") && !n.style.at("box-shadow").empty() && n.style.at("box-shadow")!="none") || !n.text.empty() || (!n.space.empty() && n.space!="http://www.w3.org/1999/xhtml") || (t!=n.style.end() && t->second.find("perspective(")!=t->second.npos)){full();return;}
        auto i=boxes.find(key(n.identity));if(i==boxes.end()){full();return;}
        auto r=i->second;for(auto v:r)if(!std::isfinite(v)){full();return;}
        bounds[0]=std::min(bounds[0],r[0]);bounds[1]=std::min(bounds[1],r[1]);bounds[2]=std::max(bounds[2],r[0]+r[2]);bounds[3]=std::max(bounds[3],r[1]+r[3]);
        for(const auto& c:n.children)self(self,c,boxes);
    };
    auto visit=[&](auto&& self,const ProbeSceneNode& x,const ProbeSceneNode& y)->void{
        if(out.full)return;
        bool local=x.identity!=y.identity || x.tag!=y.tag || x.space!=y.space || x.text!=y.text || x.attributes!=y.attributes || x.style!=y.style || x.box!=y.box || x.children.size()!=y.children.size() || bool(x.paint)!=bool(y.paint) || (x.paint && x.paint->values!=y.paint->values);
        if(!local)for(std::size_t i=0;i<x.children.size();++i)if(x.children[i].identity!=y.children[i].identity){local=true;break;}
        if(local){out.changed=true;add(add,x,old_boxes);add(add,y,new_boxes);return;}
        for(std::size_t i=0;i<x.children.size();++i)self(self,x.children[i],y.children[i]);
    };visit(visit,a.root,b.root);
    if(out.full || !out.changed)return out;
    // Integer physical pixel boundaries, including AA and flattening guards.
    out.rect={float(std::max(0.,std::floor((bounds[0]-2)*scale)/scale)),float(std::max(0.,std::floor((bounds[1]-2)*scale)/scale)),float(std::min(double(w),std::ceil((bounds[2]+2)*scale)/scale)),float(std::min(double(h),std::ceil((bounds[3]+2)*scale)/scale))};
    if(out.rect[2]<=out.rect[0] || out.rect[3]<=out.rect[1]){out.changed=false;return out;}
    if((out.rect[2]-out.rect[0])*(out.rect[3]-out.rect[1])>w*h*.65f)full();
    return out;
}
}
