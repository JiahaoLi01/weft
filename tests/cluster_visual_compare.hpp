#pragma once
#include "scene_visual_compare.hpp"
// Sparse fading points are sensitive to the >20 mask cutoff in the normal
// shape oracle. Their additional oracle uses RGB energy and 1 physical pixel
// nearest-neighbour coverage, in both directions. Dense scenes keep the
// unchanged mean <=3 / mask IoU >=.75 test.
inline bool cluster_sparse_match(const caseelephant::ProbeSceneBitmap& a,const caseelephant::ProbeSceneBitmap& b){
    require(a.width==b.width&&a.height==b.height,"sparse extent");double ae{},be{},ab{},ba{};
    constexpr int background[]{5,4,3};
    for(unsigned y=0;y<a.height;++y)for(unsigned x=0;x<a.width;++x)for(unsigned c=0;c<3;++c){const auto i=(std::size_t(y)*a.width+x)*4+c;const auto av=std::abs(int(a.rgba[i])-background[c]),bv=std::abs(int(b.rgba[i])-background[c]);int near_a{},near_b{};for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){auto xx=int(x)+dx,yy=int(y)+dy;if(xx<0||yy<0||xx>=int(a.width)||yy>=int(a.height))continue;auto j=(std::size_t(yy)*a.width+xx)*4+c;near_a=std::max(near_a,std::abs(int(a.rgba[j])-background[c]));near_b=std::max(near_b,std::abs(int(b.rgba[j])-background[c]));}ae+=av;be+=bv;ab+=std::min(av,near_b);ba+=std::min(bv,near_a);}
    return ae>0&&be>0&&std::min(ae,be)/std::max(ae,be)>=.9&&ab/ae>=.95&&ba/be>=.95;
}
inline bool cluster_image_match(const caseelephant::ProbeSceneBitmap& a,const caseelephant::ProbeSceneBitmap& b){const auto d=scene_difference(a,b);const auto mean=*std::max_element(d.mean.begin(),d.mean.end());return mean<=3&&(d.foreground<200||d.soft_iou>=.75||(d.foreground<1024&&mean<.1&&cluster_sparse_match(a,b)));}
