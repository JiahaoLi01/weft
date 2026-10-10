#pragma once
#include "caseelephant/page_probe.hpp"
#include <optional>

namespace caseelephant {
struct ProbeViewport {
    float x{},y{},width{},height{},scale{};
    static ProbeViewport fit(float physical_width,float physical_height,float logical_width=640,float logical_height=420);
    // Compose logical -> raster surface -> window, including both letterboxes.
    static ProbeViewport through_surface(float physical_width,float physical_height,
        float surface_width,float surface_height,float logical_width=640,float logical_height=420);
    // Coordinates outside the page/letterbox remain outside the logical viewport.
    std::array<double,2> logical(double physical_x,double physical_y) const;
};
std::optional<std::size_t> hit_page_probe(const ProbePage& page,double x,double y);
enum class ProbeMouseAction { Down, Up, Move, Cancel };
class ProbeMouseRouter {
public:
    ProbeMouseRouter(ScriptDomProbe& script,TextProbeFont& font,std::string root="#page")
        : script_(script),layout_([&script,&font,root=std::move(root)]{return build_page_probe(script.snapshot_subtree(root),font);}) {}
    // Owner callback must provide fresh owning values; no retained Node/JSValue.
    ProbeMouseRouter(ScriptDomProbe& script,std::function<ProbePage()> layout)
        : script_(script),layout_(std::move(layout)) {}
    // Full owner layout flush before each hit and after mouseup before click.
    bool route(ProbeMouseAction action,ProbeMouseEvent input);
    ProbeDispatchResult wheel(const ProbeWheelEvent& input);
    void cancel() { (void)script_.epoch(); pressed_.reset(); }
private:
    ScriptDomProbe& script_;
    std::function<ProbePage()> layout_;
    std::optional<ProbeNodeIdentity> pressed_;
};
}
