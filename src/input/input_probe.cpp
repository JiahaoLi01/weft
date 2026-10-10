#include "caseelephant/input_probe.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace caseelephant {
ProbeViewport ProbeViewport::fit(float pw,float ph,float lw,float lh) {
    if(!std::isfinite(pw) || !std::isfinite(ph) || !std::isfinite(lw) || !std::isfinite(lh) ||
       pw<0 || ph<0 || lw<=0 || lh<=0) throw std::invalid_argument("CE_VIEWPORT: finite positive logical extent");
    const float s=std::min(pw/lw,ph/lh);
    return {(pw-lw*s)/2,(ph-lh*s)/2,lw*s,lh*s,s};
}
std::array<double,2> ProbeViewport::logical(double px,double py) const {
    if(scale<=0 || !std::isfinite(px) || !std::isfinite(py)) return {-1,-1};
    return {(px-x)/scale,(py-y)/scale};
}
ProbeViewport ProbeViewport::through_surface(float pw,float ph,float sw,float sh,float lw,float lh) {
    const auto raster=fit(sw,sh,lw,lh),display=fit(pw,ph,sw,sh);
    return {display.x+raster.x*display.scale,display.y+raster.y*display.scale,
        raster.width*display.scale,raster.height*display.scale,raster.scale*display.scale};
}
std::optional<std::size_t> hit_page_probe(const ProbePage& page,double x,double y) {
    if(page.hits.empty() || !std::isfinite(x) || !std::isfinite(y)) return {};
    if(!page.hit_regions.empty()) {
        auto inside=[&](const std::array<float,4>& r){return r[2]>0 && r[3]>0 && x>=r[0] && y>=r[1] && x<double(r[0])+r[2] && y<double(r[1])+r[3];};
        for(auto i=page.hit_regions.size();i-->0;) {
            const auto& r=page.hit_regions[i];
            if(r.hit>=page.hits.size())throw std::invalid_argument("CE_HIT_PROFILE: region identity index");
            if(!page.hits[r.hit].enabled || !inside(r.rect) || !inside(r.clip))continue;
            bool covered=true;
            for(const auto& c:r.rounded_clips)if(!contains_rounded_probe(c,float(x),float(y))){covered=false;break;}
            if(covered)return r.hit;
        }
        return {};
    }
    const auto inside=[&](const ProbePage::Hit& h) {
        return h.width>0 && h.height>0 && x>=h.x && y>=h.y && x<h.x+h.width && y<h.y+h.height;
    };
    if(!inside(page.hits.front())) return {}; // Surface viewport clips overflow.
    for(std::size_t i=page.hits.size();i-->0;) if(page.hits[i].enabled && inside(page.hits[i])) return i;
    return {};
}
bool ProbeMouseRouter::route(ProbeMouseAction action,ProbeMouseEvent input) {
    (void)script_.epoch(); // Even an empty hit or value-only callback stays on its owner.
    if(action==ProbeMouseAction::Cancel) { cancel(); return false; }
    const auto page=layout_();
    const auto hit=hit_page_probe(page,input.client_x,input.client_y);
    if(action==ProbeMouseAction::Down) {
        cancel();
        if(!hit) return false;
        const auto& target=page.hits[*hit]; pressed_=target.identity;
        input.type=ProbeMouseType::Down; input.buttons=1;
        try {
            const auto result=script_.dispatch_mouse(target.identity,input);
            if(result==ProbeDispatchResult::StaleTarget) cancel();
            return result!=ProbeDispatchResult::StaleTarget;
        } catch(...) { cancel();throw; }
    }
    if(action==ProbeMouseAction::Move) {
        if(!hit) return false;
        input.type=ProbeMouseType::Move;
        return script_.dispatch_mouse(page.hits[*hit].identity,input)!=ProbeDispatchResult::StaleTarget;
    }
    if(action!=ProbeMouseAction::Up) throw std::invalid_argument("CE_MOUSE_PROFILE: unsupported action");
    const auto pressed=pressed_; cancel(); // Exception or reentrant callback cannot retain press.
    if(!hit) return false;
    const auto released=page.hits[*hit].identity;
    input.type=ProbeMouseType::Up; input.buttons=0;
    script_.dispatch_mouse(released,input);
    if(!pressed) return false;
    const auto current=layout_();
    auto find=[&](ProbeNodeIdentity id)->std::optional<std::size_t> {
        for(std::size_t i=0;i<current.hits.size();++i) if(current.hits[i].identity==id) return i;
        return {};
    };
    auto a=find(*pressed),b=find(released);
    if(!a || !b || current.hits[*a].disabled_button || current.hits[*b].disabled_button) return false;
    // Different down/up targets click their nearest common inclusive ancestor.
    const auto absent=std::numeric_limits<std::size_t>::max();
    for(auto ancestor=*b;ancestor!=absent;ancestor=current.hits[ancestor].parent) {
        for(auto down=*a;down!=absent;down=current.hits[down].parent) if(down==ancestor) {
            input.type=ProbeMouseType::Click;
            return script_.dispatch_mouse(current.hits[ancestor].identity,input)!=ProbeDispatchResult::StaleTarget;
        }
    }
    return false;
}
ProbeDispatchResult ProbeMouseRouter::wheel(const ProbeWheelEvent& input) {
    (void)script_.epoch(); // Owner/fault checks also apply when no region is hit.
    for(double v:{input.delta_x,input.delta_y,input.client_x,input.client_y,input.screen_x,input.screen_y,input.time_ms})
        if(!std::isfinite(v))throw std::invalid_argument("CE_WHEEL_PROFILE: finite input values");
    if(input.time_ms<0 || std::abs(input.delta_x)>100000 || std::abs(input.delta_y)>100000)
        throw std::invalid_argument("CE_WHEEL_PROFILE: bounded logical pixel deltas/time");
    const auto page=layout_();const auto hit=hit_page_probe(page,input.client_x,input.client_y);
    return hit?script_.dispatch_wheel(page.hits[*hit].identity,input):ProbeDispatchResult::StaleTarget;
}
}
