#include "paint/display_list.hpp"

#include <algorithm>
#include <string>

namespace weft::paint {
    Status check(const DisplayList& list, std::size_t max_group_depth) {
        std::size_t depth{};
        for (std::size_t i = 0; i < list.items.size(); ++i) {
            const auto& item = list.items[i];
            const auto at = " at item " + std::to_string(i);
            if (item.clips_begin > list.clips.size() || item.clips_count > list.clips.size() - item.clips_begin) {
                return make_diagnostic(Diag::PaintList, "rounded clips out of range" + at);
            }
            switch (item.kind) {
                case Item::Kind::Box:
                    if (item.payload >= list.boxes.size()) {
                        return make_diagnostic(Diag::PaintList, "box payload out of range" + at);
                    }
                    break;
                case Item::Kind::Image:
                    if (item.payload >= list.images.size() || list.images[item.payload].asset >= list.assets.size() ||
                        !list.assets[list.images[item.payload].asset]) {
                        return make_diagnostic(Diag::PaintList, "image payload or asset out of range" + at);
                    }
                    break;
                case Item::Kind::Text:
                case Item::Kind::Layer:
                    if (item.payload >= list.rasters.size() || !list.rasters[item.payload].source) {
                        return make_diagnostic(Diag::PaintList, "raster payload out of range or without source" + at);
                    }
                    break;
                case Item::Kind::BeginGroup:
                    if (++depth > max_group_depth) {
                        return make_diagnostic(Diag::PaintList, "group depth bound" + at);
                    }
                    break;
                case Item::Kind::EndGroup:
                    if (depth == 0) {
                        return make_diagnostic(Diag::PaintList, "group end without begin" + at);
                    }
                    --depth;
                    break;
                default:
                    return make_diagnostic(Diag::PaintList, "unknown item kind" + at);
            }
        }
        if (depth) {
            return make_diagnostic(Diag::PaintList, "unclosed group");
        }
        return {};
    }

    std::size_t group_depth(const DisplayList& list) {
        std::size_t deepest{};
        std::size_t depth{};
        for (const auto& item : list.items) {
            if (item.kind == Item::Kind::BeginGroup) {
                deepest = std::max(deepest, ++depth);
            } else if (item.kind == Item::Kind::EndGroup && depth) {
                --depth;
            }
        }
        return deepest;
    }
} // namespace weft::paint
