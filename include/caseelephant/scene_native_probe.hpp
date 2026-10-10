#pragma once
#include "caseelephant/scene_probe.hpp"
namespace caseelephant {
// Complete, owned native pass input. No device API, DOM pointer or VM value.
struct ProbeNativePaint {
    enum Kind {Shape,Text,Layer,BeginGroup,EndGroup} kind{};
    std::array<float,6> matrix{1,0,0,1,0,0}; // a,b,c,d,tx,ty
    std::array<float,4> clip{},radii{},widths{},background{};
    std::array<std::array<float,4>,4> edges{};
    float width{},height{},opacity=1;
    bool text_mask{};
    unsigned gradient{}; // 0:none, 1:linear, 2:radial, 3:conic
    float angle{},tile_width{},tile_height{},offset_x{},offset_y{};
    struct Stop {float offset{};std::array<float,4> color{};};
    std::vector<Stop> stops;
    std::optional<ProbeSceneNode> text; // normalized glyph/layer source, derived owned value
};
struct ProbeNativeScene {std::vector<ProbeNativePaint> paints;std::string fallback;};
// Unsupported clips/SVG/projective grammar return one explicit fallback for
// the complete scene; no partially accepted pass changes stacking semantics.
ProbeNativeScene compile_native_scene_probe(const ProbeSceneFrame&,const std::vector<ProbeNodeIdentity>& layers={});
}
