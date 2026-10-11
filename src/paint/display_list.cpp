#include "paint/display_list.hpp"

#include <algorithm>
#include <string>

namespace weft::paint {
    namespace {
        // The row-vector composition: a, then b.
        Affine then(const Affine& a, const Affine& b) {
            return {a[0] * b[0] + a[1] * b[2], a[0] * b[1] + a[1] * b[3],        a[2] * b[0] + a[3] * b[2],
                    a[2] * b[1] + a[3] * b[3], a[4] * b[0] + a[5] * b[2] + b[4], a[4] * b[1] + a[5] * b[3] + b[5]};
        }
    } // namespace

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
                case Item::Kind::Image: {
                    if (item.payload >= list.images.size()) {
                        return make_diagnostic(Diag::PaintList, "image payload or asset out of range" + at);
                    }
                    const auto& image = list.images[item.payload];
                    if (image.asset < list.assets.size() ? !list.assets[image.asset]
                                                         : image.asset - list.assets.size() >= list.external_textures) {
                        return make_diagnostic(Diag::PaintList, "image payload or asset out of range" + at);
                    }
                    if (image.asset >= list.assets.size() && image.stroke != 0) {
                        return make_diagnostic(Diag::PaintList, "external texture with a distance stroke" + at);
                    }
                    break;
                }
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

    Status append(DisplayList& into, const DisplayList& from, const Affine& transform) {
        const auto& t = transform;
        if (t[1] != 0 || t[2] != 0 || !(t[0] > 0) || !(t[3] > 0)) {
            return make_diagnostic(Diag::PaintList, "appended lists scale by positive factors and translate");
        }
        const auto assets = std::uint32_t(into.assets.size());
        const auto added = std::uint32_t(from.assets.size());
        // into's external textures move past from's assets.
        for (auto& image : into.images) {
            if (image.asset >= assets) {
                image.asset += added;
            }
        }
        const auto boxes = std::uint32_t(into.boxes.size());
        const auto images = std::uint32_t(into.images.size());
        const auto rasters = std::uint32_t(into.rasters.size());
        const auto clips = std::uint32_t(into.clips.size());
        into.boxes.insert(into.boxes.end(), from.boxes.begin(), from.boxes.end());
        for (auto image : from.images) {
            image.asset = image.asset < added ? assets + image.asset
                                              : assets + added + into.external_textures + (image.asset - added);
            into.images.push_back(image);
        }
        into.rasters.insert(into.rasters.end(), from.rasters.begin(), from.rasters.end());
        into.assets.insert(into.assets.end(), from.assets.begin(), from.assets.end());
        into.external_textures += from.external_textures;
        for (auto clip : from.clips) {
            clip.rect = {t[0] * clip.rect[0] + t[4], t[3] * clip.rect[1] + t[5], t[0] * clip.rect[2],
                         t[3] * clip.rect[3]};
            for (auto& r : clip.radius_x) {
                r *= t[0];
            }
            for (auto& r : clip.radius_y) {
                r *= t[3];
            }
            into.clips.push_back(clip);
        }
        for (auto item : from.items) {
            item.transform = then(item.transform, t);
            item.clip = {t[0] * item.clip[0] + t[4], t[3] * item.clip[1] + t[5], t[0] * item.clip[2] + t[4],
                         t[3] * item.clip[3] + t[5]};
            item.clips_begin += clips;
            switch (item.kind) {
                case Item::Kind::Box:
                    item.payload += boxes;
                    break;
                case Item::Kind::Image:
                    item.payload += images;
                    break;
                case Item::Kind::Text:
                case Item::Kind::Layer:
                    item.payload += rasters;
                    break;
                default:
                    break;
            }
            into.items.push_back(item);
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
