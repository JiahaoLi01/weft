#include "caseelephant/inline_probe.hpp"
#include <hb.h>
#include <linebreak.h>
#include <graphemebreak.h>
extern "C" {
#include <eastasianwidthdef.h>
}
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>

namespace caseelephant {
namespace {
[[noreturn]] void fail(const std::string& s){throw std::invalid_argument("CE_INLINE_PROFILE: "+s);}
constexpr std::size_t byte_limit=65536, work_limit=8*1024*1024;
bool space(std::uint32_t c){return c==32 || c==9 || c==10 || c==13;}
bool east(std::uint32_t c){auto e=ub_get_char_eaw_class(c);return e==EAW_W || e==EAW_F;}
struct Unit {std::size_t begin,end,run;std::uint32_t cp;bool wrap;};
}
std::vector<ProbeUnicodePoint> probe_unicode_points(std::string_view text){
    if(text.size()>byte_limit)fail("64 KiB paragraph bound");
    const auto free=[](hb_buffer_t* b){hb_buffer_destroy(b);};
    std::unique_ptr<hb_buffer_t,decltype(free)> b(hb_buffer_create(),free);
    if(b.get()==hb_buffer_get_empty())throw std::bad_alloc();
    hb_buffer_set_replacement_codepoint(b.get(),0x110000);
    hb_buffer_add_utf8(b.get(),text.data(),int(text.size()),0,int(text.size()));
    if(!hb_buffer_allocation_successful(b.get()))throw std::bad_alloc();
    unsigned count{};const auto* points=hb_buffer_get_glyph_infos(b.get(),&count);
    std::vector<ProbeUnicodePoint> out;out.reserve(count);
    for(unsigned i=0;i<count;++i){if(points[i].codepoint>0x10ffff)fail("malformed UTF-8");
        out.push_back({points[i].codepoint,points[i].cluster,i+1<count?points[i+1].cluster:std::uint32_t(text.size())});}
    return out;
}
ProbeUnicodeBreaks probe_unicode_breaks(std::string_view text){
    const auto points=probe_unicode_points(text);static std::once_flag init;
    std::call_once(init,[]{init_linebreak();init_graphemebreak();});
    std::vector<char> line(text.size()),grapheme(text.size());
    set_linebreaks_utf8(reinterpret_cast<const utf8_t*>(text.data()),text.size(),nullptr,line.data());
    set_graphemebreaks_utf8(reinterpret_cast<const utf8_t*>(text.data()),text.size(),nullptr,grapheme.data());
    ProbeUnicodeBreaks out;
    for(const auto& p:points){const auto i=p.end-1;
        if(grapheme[i]==GRAPHEMEBREAK_BREAK)out.grapheme.push_back(p.end);
        if(line[i]==LINEBREAK_MUSTBREAK)out.mandatory.push_back(p.end);
        if((line[i]==LINEBREAK_ALLOWBREAK || line[i]==LINEBREAK_MUSTBREAK) && grapheme[i]==GRAPHEMEBREAK_BREAK)out.line.push_back(p.end);
    }
    if(!text.empty() && (out.line.empty() || out.line.back()!=text.size()))out.line.push_back(text.size());
    return out;
}
ProbeInlineLayout layout_inline_probe(const ProbeInlineInput& input,float available,TextProbeFontSet& fonts){
    if(input.runs.size()>2048 || !std::isfinite(input.font_size) || input.font_size<=0 || input.font_size>256 || !std::isfinite(input.line_height) || input.line_height<=0 || input.line_height>4096 || std::isnan(available) || available<0)fail("input bounds");
    if(input.text_align!="left" && input.text_align!="center" && input.text_align!="right")fail("text alignment");
    if(input.white_space!="normal" && input.white_space!="nowrap" && input.white_space!="pre-line")fail("white-space requires normal/nowrap/pre-line");
    // Decode every original run before whitespace processing (malformed tails
    // must not disappear during collapse). Preserve ownership of collapsed space.
    struct Source {ProbeUnicodePoint p;std::size_t run;std::string bytes;bool forced{};};
    std::vector<Source> src;std::size_t bytes{};
    for(std::size_t r=0;r<input.runs.size();++r){const auto& run=input.runs[r];
        if((bytes+=run.text.size())>byte_limit || run.ancestors.size()>64 || !std::isfinite(run.font_size) || run.font_size<=0 || run.font_size>256 || !std::isfinite(run.line_height) || run.line_height<=0 || run.line_height>4096)fail("run bounds");
        if(run.white_space!="normal" && run.white_space!="nowrap" && run.white_space!="pre-line")fail("white-space requires normal/nowrap/pre-line");
        for(float c:run.color)if(!std::isfinite(c) || c<0 || c>1)fail("color bounds");
        if(run.hard_break)src.push_back({{10,0,1},r,"\n"});
        else {const auto breaks=probe_unicode_breaks(run.text);std::set<std::size_t> forced(breaks.mandatory.begin(),breaks.mandatory.end());
            for(auto p:probe_unicode_points(run.text))src.push_back({p,r,run.text.substr(p.begin,p.end-p.begin),forced.contains(p.end) && p.value!=10 && p.value!=13});}
    }
    std::string text;std::vector<Unit> units;bool pending{};std::size_t pending_run{};
    auto append=[&](const std::string& s,std::uint32_t cp,std::size_t r){const auto begin=text.size();text+=s;
        units.push_back({begin,text.size(),r,cp,input.runs[r].white_space!="nowrap"});};
    for(std::size_t i=0;i<src.size();++i){const auto& s=src[i];const auto& run=input.runs[s.run];
        if(s.p.value==0x00ad)fail("soft hyphen substitution pending");
        if(run.hard_break || s.forced || (s.p.value==10 && run.white_space=="pre-line")){
            pending=false;
            append("\n",10,s.run);continue;
        }
        if(space(s.p.value)){
            // CSS segment break transform between wide CJK characters.
            if(s.p.value==10){auto a=i,b=i+1;while(a>0 && space(src[a-1].p.value))--a;while(b<src.size() && space(src[b].p.value))++b;
                if(a>0 && b<src.size() && east(src[a-1].p.value) && east(src[b].p.value)){pending=false;i=b-1;continue;}}
            if(!pending){pending=true;pending_run=s.run;}continue;
        }
        if(s.p.value<32 || (s.p.value>=127 && s.p.value<=159))fail("visible control-character synthesis pending");
        if(pending && !units.empty() && units.back().cp!=10)append(" ",32,pending_run);
        pending=false;append(s.bytes,s.p.value,s.run);
    }
    ProbeInlineLayout out;if(units.empty())return out;
    const auto breaks=probe_unicode_breaks(text);std::set<std::size_t> allowed(breaks.line.begin(),breaks.line.end());
    // Documented line-break:auto tailoring: a collapsible word separator before
    // ASCII solidus is an opportunity, as in Chromium ("words / more"). UAX14
    // remains untouched; GL/WJ/ZWJ/grapheme prohibitions are retained.
    for(std::size_t i=0;i+1<units.size();++i)if(units[i].cp==32 && units[i+1].cp=='/')allowed.insert(units[i].end);
    const std::set<std::size_t> graphemes(breaks.grapheme.begin(),breaks.grapheme.end());
    auto can_wrap=[&](std::size_t i){if(i+1==units.size())return units[i].wrap;
        const auto& a=input.runs[units[i].run];const auto& b=input.runs[units[i+1].run];
        if(units[i].cp==32 || units[i].run==units[i+1].run)return units[i].wrap;
        std::size_t common{};while(common<a.ancestors.size() && common<b.ancestors.size() && a.ancestors[common]==b.ancestors[common])++common;
        if(common){if(a.ancestor_wrap.size()!=a.ancestors.size())fail("ancestor wrapping metadata required");return bool(a.ancestor_wrap[common-1]);}
        return input.white_space!="nowrap";
    };
    for(std::size_t i=1;i<units.size();++i)if(units[i].run!=units[i-1].run && !graphemes.contains(units[i].begin))fail("style boundary splits a grapheme");
    std::size_t work{};
    struct Piece {std::size_t begin,end,run;float x{},width{};ProbeFallbackText shaped;};
    auto shape=[&](std::size_t first,std::size_t last){std::vector<Piece> pieces;float x{};
        for(auto i=first;i<last;){const auto r=units[i].run;auto j=i+1;
            // Paint changes do not interrupt shaping/kerning. Split glyphs by
            // source only afterwards; a ligature crossing styles is diagnosed.
            while(j<last && input.runs[units[j].run].font_size==input.runs[r].font_size)++j;
            auto start=units[i].begin,end=units[j-1].end;const auto sample=text.substr(start,end-start);
            if((work+=sample.size())>work_limit)fail("measurement work bound");
            auto shaped=fonts.shape(sample,input.runs[r].font_size);if(shaped.shaped.missing_glyph)fail("missing glyph");
            std::set<std::size_t> clusters;for(const auto& g:shaped.shaped.glyphs)clusters.insert(start+g.utf8_cluster);clusters.insert(end);
            for(auto k=i+1;k<j;++k)if(units[k].run!=units[k-1].run && !clusters.contains(units[k].begin))fail("style boundary splits a shaped cluster");
            for(auto k=i;k<j;){auto l=k+1;while(l<j && units[l].run==units[k].run)++l;
                Piece p{units[k].begin,units[l-1].end,units[k].run,x};
                for(auto g:shaped.shaped.glyphs)if(start+g.utf8_cluster>=p.begin && start+g.utf8_cluster<p.end){g.utf8_cluster=std::uint32_t(start+g.utf8_cluster-p.begin);p.shaped.shaped.glyphs.push_back(g);p.width+=g.advance_x;}
                p.shaped.shaped.advance=p.width;
                for(const auto& fr:shaped.runs)if(start+fr.utf8_start<p.end && start+fr.utf8_start+fr.utf8_length>p.begin)p.shaped.runs.push_back(fr);
                x+=p.width;pieces.push_back(std::move(p));k=l;
            }i=j;
        }return std::pair{x,std::move(pieces)};
    };
    auto trimmed=[&](std::size_t first,std::size_t last){while(last>first && units[last-1].cp==32)--last;return last;};
    auto width=[&](std::size_t first,std::size_t last){return shape(first,trimmed(first,last)).first;};
    // Intrinsics use exactly the same legal Unicode opportunities as wrapping.
    std::size_t start{},token{};for(std::size_t i=0;i<units.size();++i){const auto& u=units[i];
        if(u.cp==10){out.max_content=std::max(out.max_content,width(start,i));out.min_content=std::max(out.min_content,width(token,i));start=token=i+1;}
        else if(allowed.contains(u.end) && can_wrap(i)){out.min_content=std::max(out.min_content,width(token,i+1));token=i+1;}
    }
    out.max_content=std::max(out.max_content,width(start,units.size()));out.min_content=std::max(out.min_content,width(token,units.size()));
    auto emit=[&](std::size_t first,std::size_t last){last=trimmed(first,last);auto [w,pieces]=shape(first,last);
        const auto strut=fonts.inline_metrics(0,input.font_size);const float half=(input.line_height-strut.ascent-strut.descent)/2;
        float above=strut.ascent+half,below=strut.descent+half;
        for(const auto& p:pieces){const auto& r=input.runs[p.run];auto m=fonts.inline_metrics(0,r.font_size);float a=m.ascent,d=m.descent;
            const float lead=(r.line_height-a-d)/2;above=std::max(above,a+lead);below=std::max(below,d+lead);}
        const float line_height=above+below;const float dx=std::isfinite(available)?(input.text_align=="center"?(available-w)/2:input.text_align=="right"?available-w:0):0;
        for(auto& p:pieces){const auto& r=input.runs[p.run];auto m=fonts.inline_metrics(0,r.font_size);
            out.fragments.push_back({text.substr(p.begin,p.end-p.begin),r.ancestors,dx+p.x,out.height+above-m.ascent,p.width,m.ascent+m.descent,out.height+above,r.font_size,r.color,std::move(p.shaped.shaped)});}
        const auto baseline=out.height+above;if(out.lines==0)out.baseline=above;out.width=std::max(out.width,w);out.height+=line_height;++out.lines;
        return std::pair{dx+w,baseline};
    };
    start=0;while(start<units.size()){
        while(start<units.size() && units[start].cp==32)++start;if(start==units.size())break;
        std::size_t last_fit=start,last_test=start,end=start;bool done{};
        for(std::size_t i=start;i<units.size();++i){const auto& u=units[i];
            if(u.cp==10){auto [x,baseline]=emit(start,i);const auto& r=input.runs[u.run];if(r.hard_break){auto m=fonts.inline_metrics(0,r.font_size);out.fragments.push_back({{},r.ancestors,x,baseline-m.ascent,0,m.ascent+m.descent,baseline,r.font_size,r.color,{}});}start=i+1;done=true;break;}
            const bool can=i+1==units.size() || (allowed.contains(u.end) && can_wrap(i));
            if(!can)continue;end=i+1;const float w=width(start,end);
            if(w>available+.0001f && last_fit>start){emit(start,last_fit);start=last_fit;done=true;break;}
            if(w>available+.0001f){emit(start,end);start=end;done=true;break;}
            last_fit=end;last_test=end;
        }
        if(!done){emit(start,last_test>start?last_test:units.size());start=units.size();}
    }
    return out;
}
}
