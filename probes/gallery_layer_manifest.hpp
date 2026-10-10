#pragma once
// The inspection host explicitly opts these authored paths into source reuse.
// No class/style heuristic changes the renderer's layer policy or the bundle.
std::vector<caseelephant::ProbeNodeIdentity> gallery_layer_manifest(unsigned index,const caseelephant::ProbeSceneFrame& frame,bool native=false){
    // Authored host policy for the native pass: a single ring/bar/emoji is
    // already a cheap batched primitive or glyph image. Cache the compound
    // background/border/text groups; keep reference-backend hints separately.
    if(native && index!=9 && index!=15)return {};
    std::vector<std::vector<unsigned>> paths;
    if(index==9 || index==13 || index==24)paths={{0,0}};
    if(index==10)paths={{0,1}};
    if(index==11 || index==12 || index==14)paths={{0,0},{0,1},{0,2}};
    if(index==15)paths={{0,0},{0,1},{0,2},{0,3},{0,4}};
    if(index==22)paths={{0,1},{0,2}};
    if(index==23)paths={{0,1},{0,2},{0,3}};
    if(index==25)for(unsigned i=0;i<12;++i)paths.push_back({0,i});
    std::vector<caseelephant::ProbeNodeIdentity> ids;for(const auto& path:paths){auto n=&frame.root;for(auto p:path){if(p>=n->children.size()){n=nullptr;break;}n=&n->children[p];}if(n)ids.push_back(n->identity);}return ids;
}
