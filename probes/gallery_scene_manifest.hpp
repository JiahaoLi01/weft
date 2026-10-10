#pragma once
#include <array>
#include <cstddef>
#include <string>
inline constexpr unsigned gallery_scene_count=64, gallery_native_case_count=66;
struct GallerySceneDescriptor {const char* group;unsigned index,catalog_index;float width,height;};
inline constexpr auto gallery_scene_descriptors=[] {
    std::array<GallerySceneDescriptor,gallery_scene_count> out{};unsigned n{};
    for(unsigned i=0;i<26;++i)out[n++]={"CSS_TESTS",i,i,387,288};
    for(unsigned i=0;i<8;++i)out[n++]={"D3_TESTS",i,26+i,384,384};
    const char* groups[]={"D3_INTERACTIVE_TESTS","KLINE_TESTS","FRAMER_TESTS","SPRING_TESTS","ICON_TESTS","ANIME_TESTS"};
    const unsigned counts[]={4,1,6,3,2,14},catalog[]={34,38,61,67,70,47};
    const float widths[]={1596,1596,518.6666667f,518.6666667f,788,387},heights[]={1500,800,320,288,288,288};
    for(unsigned g=0;g<6;++g)for(unsigned i=0;i<counts[g];++i)out[n++]={groups[g],i,catalog[g]+i,widths[g],heights[g]};
    return out;
}();
inline bool gallery_cluster_group(const std::string& group){for(unsigned i=34;i<gallery_scene_count;++i)if(group==gallery_scene_descriptors[i].group)return true;return false;}
