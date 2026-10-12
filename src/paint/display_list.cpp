#include "paint/display_list.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace weft::paint {
    namespace {
        bool finite_value(float v) {
            return std::isfinite(v);
        }

        // Verbs that use exactly the points there are, a Move first, and finite points.
        bool well_formed(const Path& path) {
            std::size_t points{};
            for (std::size_t i = 0; i < path.verbs.size(); ++i) {
                switch (path.verbs[i]) {
                    case Path::Verb::Move:
                    case Path::Verb::Line:
                        ++points;
                        break;
                    case Path::Verb::Cubic:
                        points += 3;
                        break;
                    case Path::Verb::Close:
                        break;
                    default:
                        return false;
                }
                if (i == 0 && path.verbs[i] != Path::Verb::Move) {
                    return false;
                }
            }
            return points == path.points.size() && std::all_of(path.points.begin(), path.points.end(), [](auto p) {
                       return finite_value(p[0]) && finite_value(p[1]);
                   });
        }

        bool well_formed(const Stroke& s) {
            return finite_value(s.width) && s.width >= 0 && finite_value(s.miter_limit) && s.miter_limit >= 1 &&
                   finite_value(s.dash_offset) && std::all_of(s.dashes.begin(), s.dashes.end(), [](float v) {
                       return finite_value(v) && v >= 0;
                   });
        }

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
            for (auto c = item.clips_begin; c < item.clips_begin + item.clips_count; ++c) {
                const auto& m = list.clips[c].to_local;
                if (!std::all_of(m.begin(), m.end(), finite_value) || !std::isnormal(m[0] * m[3] - m[1] * m[2])) {
                    return make_diagnostic(Diag::PaintList, "rounded clip map" + at);
                }
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
                case Item::Kind::Shadow: {
                    if (item.payload >= list.shadows.size()) {
                        return make_diagnostic(Diag::PaintList, "shadow payload out of range" + at);
                    }
                    const auto& s = list.shadows[item.payload];
                    if (!std::all_of(s.color.begin(), s.color.end(), finite_value) ||
                        !std::all_of(s.radii.begin(), s.radii.end(), finite_value) || !std::isfinite(s.x) ||
                        !std::isfinite(s.y) || !std::isfinite(s.spread) || !std::isfinite(s.blur) || s.blur < 0) {
                        return make_diagnostic(Diag::PaintList, "shadow value" + at);
                    }
                    break;
                }
                case Item::Kind::Path: {
                    if (item.payload >= list.paths.size()) {
                        return make_diagnostic(Diag::PaintList, "path payload out of range" + at);
                    }
                    const auto& p = list.paths[item.payload];
                    if (!p.path || !well_formed(*p.path) ||
                        !std::all_of(p.color.begin(), p.color.end(), finite_value) ||
                        (p.stroke && !well_formed(*p.stroke))) {
                        return make_diagnostic(Diag::PaintList, "path value" + at);
                    }
                    break;
                }
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
        const auto shadows = std::uint32_t(into.shadows.size());
        const auto paths = std::uint32_t(into.paths.size());
        const auto clips = std::uint32_t(into.clips.size());
        into.boxes.insert(into.boxes.end(), from.boxes.begin(), from.boxes.end());
        for (auto image : from.images) {
            image.asset = image.asset < added ? assets + image.asset
                                              : assets + added + into.external_textures + (image.asset - added);
            into.images.push_back(image);
        }
        into.rasters.insert(into.rasters.end(), from.rasters.begin(), from.rasters.end());
        into.shadows.insert(into.shadows.end(), from.shadows.begin(), from.shadows.end());
        into.paths.insert(into.paths.end(), from.paths.begin(), from.paths.end());
        into.assets.insert(into.assets.end(), from.assets.begin(), from.assets.end());
        into.external_textures += from.external_textures;
        for (auto clip : from.clips) {
            if (clip.to_local != identity_clip_map) {
                // A clip in its own space keeps its rectangle; its map takes into's target px back through the
                // transform first.
                const auto& m = clip.to_local;
                clip.to_local = {m[0] / t[0],
                                 m[1] / t[0],
                                 m[2] / t[3],
                                 m[3] / t[3],
                                 m[4] - (t[4] / t[0]) * m[0] - (t[5] / t[3]) * m[2],
                                 m[5] - (t[4] / t[0]) * m[1] - (t[5] / t[3]) * m[3]};
                into.clips.push_back(clip);
                continue;
            }
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
                case Item::Kind::Shadow:
                    item.payload += shadows;
                    break;
                case Item::Kind::Path:
                    item.payload += paths;
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
