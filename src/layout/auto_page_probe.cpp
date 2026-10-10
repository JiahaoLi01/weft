#include "caseelephant/auto_page_probe.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace caseelephant {
namespace {
using Style=std::map<std::string,std::string>;
[[noreturn]] void fail(const std::string& s){throw std::invalid_argument("CE_AUTO_PROFILE: "+s);}
std::string get(const Style& s,const std::string& k,const char* fallback=""){auto i=s.find(k);return i==s.end()?fallback:i->second;}
float number(const std::string& s){try{std::size_t i{};auto n=std::stof(s,&i);if(i==s.size() && std::isfinite(n) && std::abs(n)<=100000)return n;}catch(const std::exception&){}fail("finite number "+s);}
float pixels(const std::string& s){if(s=="0")return 0;if(s.ends_with("px"))return number(s.substr(0,s.size()-2));fail("px required: "+s);}
struct Calc {std::size_t index;std::string key;float percent,offset;};
struct Node {Style style;std::string text,id,image;std::vector<std::size_t> children;std::size_t parent;int z{};bool hidden{};};
struct Rect{float x{},y{},w{},h{};};
Rect intersect(Rect a,Rect b){float x=std::max(a.x,b.x),y=std::max(a.y,b.y);return {x,y,std::max(0.f,std::min(a.x+a.w,b.x+b.w)-x),std::max(0.f,std::min(a.y+a.h,b.y+b.h)-y)};}
struct Builder {
    TextProbeFontSet& fonts;std::vector<Node> nodes;std::vector<Calc> calcs;
    FlexProbeNode make(const ProbeDomSnapshot& dom,const ProbeComputedStyle& computed,std::size_t parent,bool hidden=false){
        static const std::set<std::string> tags{"div","button","i","b","span","ul","li","img"};
        if(dom.type!=1 || dom.namespace_uri!="http://www.w3.org/1999/xhtml" || !tags.contains(dom.local_name))fail("HTML element profile");
        Style s=computed.values;
        auto display=get(s,"display","inline");const auto position=get(s,"position","static");
        if(display=="inline" && (position=="absolute" || (parent<nodes.size() && get(nodes[parent].style,"display")=="flex")))display="block";
        s["display"]=display;
        if(display!="block" && display!="flex" && display!="none")fail("explicit block/flex required: "+display);
        if(position!="static" && position!="relative" && position!="absolute")fail("position "+position);
        hidden=hidden || display=="none";
        if(get(s,"overflow","visible")!="visible" && get(s,"overflow")!="hidden")fail("overflow");
        if(get(s,"isolation","auto")!="auto" && get(s,"isolation")!="isolate")fail("isolation");
        if(get(s,"pointer-events","auto")!="auto" && get(s,"pointer-events")!="none")fail("pointer-events");
        if(get(s,"user-select","auto")!="auto" && get(s,"user-select")!="none")fail("user-select");
        for(const auto& [k,v]:s){
            if(k.starts_with("margin") || k=="border" || k.starts_with("border-"))if(pixels(v)!=0)fail("zero margin/border subset");
            if(k.starts_with("padding") && (pixels(v)<0 || pixels(v)>4096))fail("padding bounds");
            if(display=="block" && (k=="gap" || k=="row-gap" || k=="column-gap" || k=="flex-direction" || k=="flex-wrap" || k=="align-items" || k=="align-content" || k=="justify-content"))fail("flex container property on block outside profile");
            if(parent<nodes.size() && get(nodes[parent].style,"display")=="block" && (k=="flex" || k=="flex-grow" || k=="flex-shrink" || k=="flex-basis" || k=="align-self"))fail("flex item property outside flex parent");
        }
        if((dom.local_name=="ul" || dom.local_name=="li") && get(s,"list-style")!="none")fail("explicit list marker reset required");
        std::string text;for(const auto& c:dom.children)if(c.type==3)text+=c.data;else if(c.type!=1)fail("child node type");
        std::istringstream words(text);std::string word;text.clear();while(words>>word){if(!text.empty())text+=' ';text+=word;}
        if(!text.empty() && !computed.children.empty())fail("mixed inline content");
        int z{};if(get(s,"z-index","auto")!="auto"){
            const auto n=number(s.at("z-index"));if(std::floor(n)!=n || std::abs(n)>1024)fail("z-index bound");z=int(n);
            if(position=="static" || parent>=nodes.size() || get(nodes[parent].style,"isolation")!="isolate")fail("z-index requires direct isolated positioned parent");
        }
        const auto index=nodes.size();auto id=dom.attributes.contains("id")?dom.attributes.at("id"):"box-"+std::to_string(index);
        if(position!="absolute" && parent<nodes.size() && get(s,"height").ends_with('%') && get(nodes[parent].style,"height","auto")=="auto")
            fail("flow percentage height needs explicit definite parent height");
        std::string image;if(dom.local_name=="img"){
            if(!dom.attributes.contains("src") || !dom.children.empty() || get(s,"width","auto")=="auto" || get(s,"height","auto")=="auto")fail("image requires source and dimensions");image=dom.attributes.at("src");
        }
        nodes.push_back({s,text,id,image,{},parent,z,hidden});
        FlexProbeNode out{id,s,hidden?std::string{}:text,{}};
        for(const auto* key:{"color","background-color","font-family","font-weight","font-style","text-align","pointer-events","isolation","list-style","user-select","z-index","border"})out.style.erase(key);
        for(const auto* key:{"max-width","max-height"})if(get(out.style,key)=="none")out.style.erase(key);
        out.style["display"]=hidden?"none":display;
        out.style.try_emplace("font-size",get(s,"font-size","16px"));out.style.try_emplace("line-height",get(s,"line-height","24px"));
        out.style.try_emplace("white-space",get(s,"white-space","normal"));
        if(!hidden && !text.empty()){
            if(get(s,"font-family","ProbeRoboto, ProbeCjk")!="ProbeRoboto, ProbeCjk" || get(s,"font-weight","400")!="400" || get(s,"font-style","normal")!="normal")fail("fixed normal-weight font set");
            if(get(s,"text-align","left")!="left" && get(s,"text-align")!="center" && get(s,"text-align")!="right")fail("text alignment");
            if(pixels(out.style.at("font-size"))<=0 || pixels(out.style.at("font-size"))>256 || pixels(out.style.at("line-height"))<=0 || pixels(out.style.at("line-height"))>4096)fail("text metrics bounds");
        }
        for(auto& [k,v]:out.style)if(v.starts_with("calc(")){
            if(position!="absolute" || (k!="left" && k!="top" && k!="right" && k!="bottom" && k!="width" && k!="height"))fail("calc only in absolute dimensions/insets");
            std::string a,op,b,extra;std::istringstream parts(v.substr(5,v.size()-6));parts>>a>>op>>b;
            if(parts>>extra || !v.ends_with(')') || !a.ends_with('%') || (op!="+" && op!="-"))fail("percent +/- px calc");
            calcs.push_back({index,k,number(a.substr(0,a.size()-1)),pixels(b)*(op=="-"?-1:1)});v=a;
        }
        std::size_t i{};for(const auto& c:dom.children)if(c.type==1){const auto child_index=nodes.size();auto child=make(c,computed.children[i++],index,hidden);nodes[index].children.push_back(child_index);out.children.push_back(std::move(child));}
        return out;
    }
    void paint(std::size_t index,const std::vector<FlexProbeBox>& boxes,Rect clip,ProbePositionedPage& out){
        const auto& n=nodes[index];if(n.hidden)return;const auto& box=boxes[index];Rect r{box.x,box.y,box.width,box.height};
        auto quad=[&](std::array<float,4> c,const std::string& image){auto b=intersect(r,clip);if(b.w<=0 || b.h<=0)return;
            ProbePositionedPaint p;p.paint={ProbePaintKind::Rectangle,n.id,{},b.x,b.y,b.w,b.h,0,c,{}};p.image_source=image;
            p.uv={(b.x-r.x)/r.w,(b.y-r.y)/r.h,b.w/r.w,b.h/r.h};p.clip={clip.x,clip.y,clip.w,clip.h};out.paints.push_back(std::move(p));};
        if(n.style.contains("background-color"))quad(parse_probe_css_color(n.style.at("background-color")),{});
        if(!n.image.empty())quad({1,1,1,1},n.image);
        if(get(n.style,"overflow","visible")=="hidden")clip=intersect(clip,r);
        if(!n.text.empty()){
            auto edge=[&](const char* key){return pixels(get(n.style,key,"0"));};
            const auto size=pixels(get(n.style,"font-size","16px")),line=pixels(get(n.style,"line-height","24px"));
            const auto width=std::max(0.f,r.w-edge("padding-left")-edge("padding-right"));
            const auto lines=layout_probe_text(n.text,size,width,get(n.style,"white-space","normal")=="nowrap",fonts.primary(),&fonts);
            float y=r.y+edge("padding-top")+box.baseline;
            for(const auto& l:lines){const auto align=get(n.style,"text-align","left");float dx=align=="center"?(width-l.shaped.advance)/2:align=="right"?width-l.shaped.advance:0;
                ProbePositionedPaint p;p.paint={ProbePaintKind::Text,n.id,l.text,r.x+edge("padding-left")+dx,y,0,0,size,parse_probe_css_color(get(n.style,"color","#000000")),l.shaped};
                p.clip={clip.x,clip.y,clip.w,clip.h};out.paints.push_back(std::move(p));y+=line;
            }
        }
        auto children=n.children;std::stable_sort(children.begin(),children.end(),[&](auto a,auto b){return nodes[a].z<nodes[b].z;});for(auto c:children)paint(c,boxes,clip,out);
    }
};
}
ProbePositionedPage build_auto_page_probe(const ProbeDomSnapshot& root,const ProbeComputedStyle& style,float w,float h,TextProbeFontSet& fonts){
    if(!std::isfinite(w) || !std::isfinite(h) || w<=0 || h<=0 || w>4096 || h>4096)fail("viewport bound");
    validate_style_pair(root,style);Builder builder{fonts};auto tree=builder.make(root,style,std::numeric_limits<std::size_t>::max());
    if(get(tree.style,"width","auto")=="auto"){
        const auto padding=get(tree.style,"box-sizing","content-box")=="content-box"?
            pixels(get(tree.style,"padding-left","0"))+pixels(get(tree.style,"padding-right","0")):0;
        tree.style["width"]=std::to_string(std::max(0.f,w-padding))+"px";
    }
    if(get(tree.style,"position","static")!="relative")fail("standalone scope root requires position:relative");
    for(const auto* k:{"width","height"}){auto v=get(tree.style,k);if(v.ends_with('%'))tree.style[k]=std::to_string(number(v.substr(0,v.size()-1))/100*(std::string(k)=="width"?w:h))+"px";}
    std::vector<FlexProbeNode*> inputs;auto visit=[&](auto&& self,FlexProbeNode& n)->void{inputs.push_back(&n);for(auto& c:n.children)self(self,c);};visit(visit,tree);
    auto boxes=run_flex_probe(tree,fonts);
    // Absolute calc dependencies follow containing-block ancestry. Recompute
    // from the root, bounded and convergent, with no JS callback or stale cache.
    for(unsigned pass=0;!builder.calcs.empty();++pass){
        bool changed{};for(const auto& c:builder.calcs){auto parent=builder.nodes[c.index].parent;
            while(parent<builder.nodes.size() && parent!=0 && get(builder.nodes[parent].style,"position","static")=="static")parent=builder.nodes[parent].parent;
            const auto base=parent<boxes.size()?((c.key=="top" || c.key=="bottom" || c.key=="height")?boxes[parent].height:boxes[parent].width):((c.key=="top" || c.key=="bottom" || c.key=="height")?h:w);
            const auto px=base*c.percent/100+c.offset;if((c.key=="width" || c.key=="height") && px<0)fail("negative calc size");
            const auto v=std::to_string(px)+"px";changed|=inputs[c.index]->style[c.key]!=v;inputs[c.index]->style[c.key]=v;
        }
        if(!changed)break;if(pass>=15 || (pass+1)*boxes.size()>16384)fail("absolute calc dependency work bound");boxes=run_flex_probe(tree,fonts);
    }
    for(auto& b:boxes)if(!std::isfinite(b.x) || !std::isfinite(b.y) || !std::isfinite(b.width) || !std::isfinite(b.height) || std::abs(b.x)>100000 || std::abs(b.y)>100000 || b.width>100000 || b.height>100000)fail("box bounds");
    ProbePositionedPage out;out.geometry.boxes=std::move(boxes);builder.paint(0,out.geometry.boxes,{0,0,w,h},out);return out;
}
ProbePositionedPage build_auto_page_probe(const ProbeDomSnapshot& root,std::string_view css,float w,float h,TextProbeFontSet& fonts){return build_auto_page_probe(root,compute_style_probe(root,css,auto_page_style_profile()),w,h,fonts);}
}
