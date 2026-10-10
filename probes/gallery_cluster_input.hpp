#pragma once
#include "gallery_scene_session.hpp"
#include <iostream>

inline void gallery_cluster_prepare(GallerySceneSession& s, const std::string& id) {
    s.vm.eval("globalThis.__ceClusterSpec=" + scene_read(CE_SOURCE_DIR "/examples/crpg-gallery/cluster-cases.json") +
              ";globalThis.__ceClusterInputs=__ceClusterSpec.inputs['" + id + "']||[]");
}

inline unsigned gallery_cluster_inputs(GallerySceneSession& s, unsigned frame, std::ostream* trace = nullptr) {
    s.vm.eval("globalThis.__ceReady=__ceClusterInputs.filter(i=>i.frame===" + std::to_string(frame) + ")");
    const auto count = unsigned(std::stoul(s.vm.eval("__ceReady.length")));
    for (unsigned index = 0; index < count; ++index) {
        s.vm.eval("globalThis.__ceInput=__ceReady[" + std::to_string(index) + "]");
        const auto action = s.vm.eval("__ceInput.action");
        const auto coords = s.vm.eval(
            "(()=>{let {x,y}=__ceInput;if(__ceInput.selector){let list=Array.from(document.querySelectorAll(__ceInput.selector));if(__ceInput.visible)list=list.filter(n=>{const r=n.getBoundingClientRect(),x=r.x+r.width/2,y=r.y+r.height/2;return x>innerWidth*.15&&x<innerWidth*.85&&y>innerHeight*.15&&y<innerHeight*.85;});const n=list[__ceInput.index||0];if(!n)throw Error('input anchor absent');const r=n.getBoundingClientRect(),f=__ceInput.fraction||[.5,.5];x=r.x+r.width*f[0];y=r.y+r.height*f[1];}return x+' '+y;})()");
        float x{};
        float y{};
        std::istringstream(coords) >> x >> y;
        const auto buttons = unsigned(std::stoul(s.vm.eval("__ceInput.buttons||0")));
        const auto wheel = std::stod(s.vm.eval("__ceInput.delta||0"));
        s.input_value(action == "down"    ? caseelephant::ProbeMouseType::Down
                      : action == "up"    ? caseelephant::ProbeMouseType::Up
                      : action == "click" ? caseelephant::ProbeMouseType::Click
                                          : caseelephant::ProbeMouseType::Move,
                      x, y, buttons, wheel);
        s.step(s.time);
        s.step(s.time);
        if (s.vm.stats().listener_exceptions) {
            throw std::runtime_error(s.vm.stats().last_listener_error);
        }
        if (trace) {
            *trace << " {input:" << frame << "," << action << "," << x << "," << y << "}" << std::flush;
        }
    }
    return count;
}

inline constexpr std::array<unsigned, 7> gallery_cluster_capture_frames{24, 63, 90, 120, 180, 240, 300};
