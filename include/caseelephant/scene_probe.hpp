#pragma once
#include "caseelephant/style_probe.hpp"
#include "caseelephant/scene_paint_probe.hpp"
#include <functional>
#include <memory>

#ifdef _WIN32
struct ID2D1RenderTarget;
#endif
namespace caseelephant {
// Owner-only scene formatting/animation probe. Render adapters consume these
// owned values; neither DOM pointers nor VM handles cross the boundary.
using ProbeSceneStyle=std::map<std::string,std::string>;
struct ProbeSceneTextMetrics {float width{},height{},baseline{};};
using ProbeSceneMeasure=std::function<ProbeSceneTextMetrics(std::string_view,const ProbeSceneStyle&)>;
struct ProbeSceneNode {
    ProbeNodeIdentity identity;std::string tag,space,text;
    std::map<std::string,std::string> attributes;ProbeSceneStyle style;
    std::array<float,4> box{}; // Parent-local border box, before transform.
    std::vector<ProbeSceneNode> children;
    std::optional<ProbeScenePaintValues> paint; // Derived numeric cache, not DOM/style identity.
    friend bool operator==(const ProbeSceneNode& a,const ProbeSceneNode& b){return a.identity==b.identity && a.tag==b.tag && a.space==b.space && a.text==b.text && a.attributes==b.attributes && a.style==b.style && a.box==b.box && a.children==b.children;}
};
struct ProbeSceneFrame {ProbeSceneNode root;std::vector<ProbeElementGeometry> elements;bool animated{};double time_ms{};};
// Exact dependency projection for the current geometry adapter. Paint color,
// opacity and pseudo state do not change geometry; transforms still do.
inline bool scene_geometry_own_equal_probe(const ProbeSceneNode& a,const ProbeSceneNode& b,bool identical_style=false){
    if(a.identity!=b.identity || a.tag!=b.tag || a.space!=b.space || a.text!=b.text || a.box!=b.box)return false;
    auto equal_keys=[](const auto& x,const auto& y,const auto& keys){for(const auto* k:keys){auto p=x.find(k),q=y.find(k);if((p==x.end())!=(q==y.end()) || (p!=x.end() && p->second!=q->second))return false;}return true;};
    static constexpr const char* styles[]{"transform","transform-origin","position","border-left-width","border-right-width","border-top-width","border-bottom-width","font-size","font-weight","font-family","letter-spacing","text-anchor","dominant-baseline"};
    static constexpr const char* attrs[]{"transform","width","height","viewBox","preserveAspectRatio","x","y","dy","x1","y1","x2","y2","rx","ry","r","cx","cy","d","points"};
    return (identical_style || equal_keys(a.style,b.style,styles)) && equal_keys(a.attributes,b.attributes,attrs);
}
inline bool scene_geometry_equal_probe(const ProbeSceneNode& a,const ProbeSceneNode& b){
    if(!scene_geometry_own_equal_probe(a,b) || a.children.size()!=b.children.size())return false;
    for(std::size_t i=0;i<a.children.size();++i)if(!scene_geometry_equal_probe(a.children[i],b.children[i]))return false;return true;
}
// Source reuse must compare the numeric samples too: diagnostic style strings
// may round two different samples to the same spelling. Logical DOM equality
// above intentionally excludes this derived cache.
inline bool scene_paint_source_equal_probe(const ProbeSceneNode& a,const ProbeSceneNode& b){
    if(a.identity!=b.identity || a.tag!=b.tag || a.space!=b.space || a.text!=b.text || a.attributes!=b.attributes || a.style!=b.style || a.box!=b.box || a.children.size()!=b.children.size() || bool(a.paint)!=bool(b.paint) || (a.paint && a.paint->values!=b.paint->values))return false;
    for(std::size_t i=0;i<a.children.size();++i)if(!scene_paint_source_equal_probe(a.children[i],b.children[i]))return false;
    return true;
}
// Damage is relative to an actual retained surface, never a previous candidate
// packet. Unknown paint bounds conservatively invalidate the entire surface.
struct ProbeSceneDamage {bool changed{},full{};std::array<float,4> rect{};}; // left/top/right/bottom
ProbeSceneDamage scene_damage_probe(const ProbeSceneFrame&,const ProbeSceneFrame&,float scale);
class SceneProbe {
public:
    SceneProbe(std::string stylesheet,std::string keyframes,ProbeSceneMeasure measure,bool incremental=true);
    ~SceneProbe();SceneProbe(const SceneProbe&)=delete;SceneProbe& operator=(const SceneProbe&)=delete;
    ProbeSceneFrame frame(const ProbeDomSnapshot&,float width,float height,double time_ms);
    // Owner-local retained scene; unchanged branches keep their owned values.
    // Publish a separate immutable snapshot before crossing threads. On error
    // discard the output and reset this solver before using it again.
    void update(const ProbeDomSnapshot&,ProbeSceneFrame&,float width,float height,double time_ms);
    void reset();
    void set_profiling(bool);
    struct Work {std::size_t style_evaluations{},layout_evaluations{},style_reuses{},layout_reuses{},animation_compilations{},animation_reuses{},typed_samples{},paint_compilations{},static_subtree_reuses{};double style_ms{},build_ms{},layout_ms{};bool geometry_changed{},paint_changed{};};
    Work work()const;
private:struct Impl;std::unique_ptr<Impl> impl_;
};
ProbeStyleProfile scene_style_profile();
// Windows software raster adapter, separate from the owner formatting core.
// RGBA is straight encoded sRGB, ready for the existing image upload contract.
struct ProbeSceneBitmap {unsigned width{},height{};std::vector<std::uint8_t> rgba;};
class SceneRasterProbe {
public:
    SceneRasterProbe();~SceneRasterProbe();
    SceneRasterProbe(const SceneRasterProbe&)=delete;SceneRasterProbe& operator=(const SceneRasterProbe&)=delete;
    ProbeSceneTextMetrics measure(std::string_view,const ProbeSceneStyle&);
    std::array<double,6> transform_matrix(const ProbeSceneNode&)const;
    ProbeSceneBitmap raster(const ProbeSceneFrame&,float scale=1);
    struct Work {std::size_t gpu_projective_draws{},gpu_plane_uploads{},uniform_border_draws{},fallback_border_draws{},border_geometry_builds{},border_geometry_reuses{},paint_program_builds{},paint_program_reuses{},paint_program_evictions{},typed_draws{},layer_builds{},layer_reuses{},layer_fallbacks{},layer_evictions{},partial_paints{};};
    Work work()const;
    std::vector<ProbeElementGeometry> geometry(const ProbeSceneFrame&,bool svg_descendants=false)const;
#ifdef _WIN32
    // Private Windows adapter: borrowed target for this call, owned scene only.
    void paint_d2d(const ProbeSceneFrame&,ID2D1RenderTarget*,float scale=1,std::optional<std::array<float,4>> clip=std::nullopt);
    // Explicit host opt-in; identities are weak, copied values. No promotion
    // heuristic or change to CSS stacking semantics. Unsupported sources fall
    // back to normal painting. Caches are local to the target resource domain.
    void set_layers(std::vector<ProbeNodeIdentity>);
    void paint_d2d_damage(const ProbeSceneFrame&,ID2D1RenderTarget*,float scale,
                          std::array<float,4> damage,std::optional<std::array<float,4>> clip=std::nullopt);
#endif
    ProbeNodeIdentity hit_test(const ProbeSceneFrame&,float x,float y)const;
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
