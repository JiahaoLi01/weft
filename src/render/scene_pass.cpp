#include "render/scene_pass.hpp"

#include "base/plane.hpp"
#include "constants/common.hpp"
#include "render/rounded_clip.hpp"
#include "render/shadow_image.hpp"
#include "render/srgb.hpp"
#include "vector/path_raster.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace weft::render {
    std::array<std::uint32_t, 2> plane_pixels(const paint::Plane& plane, float scale) {
        const double margins = scene_limits::image_margins;
        return {std::uint32_t(std::ceil((plane.width + margins) * scale)),
                std::uint32_t(std::ceil((plane.height + margins) * scale))};
    }

    namespace {
        using Kind = paint::Item::Kind;

        // A straight sRGB color as the shader takes it: unchanged for an encoded pass (the shader premultiplies and
        // quantizes), linear and premultiplied for a linear pass.
        paint::Color in(Space space, paint::Color c) {
            if (space == Space::Linear) {
                for (unsigned i = 0; i < 3; ++i) {
                    c[i] = srgb_to_linear(c[i]) * c[3];
                }
            }
            return c;
        }

        // The device pixels a path item's mask covers: the path under the item's transform at scale, within its clip
        // and the target.
        std::array<std::int32_t, 4> path_mask(const paint::DisplayList& list, const paint::Item& item,
                                              const paint::ClipRect& clip, float scale, unsigned w, unsigned h) {
            paint::Affine device = item.transform;
            for (auto& v : device) {
                v *= scale;
            }
            const auto edge = [](float v, bool up) {
                const double d = up ? std::ceil(double(v)) : std::floor(double(v));
                return std::int32_t(std::clamp(d, -double(1 << 24), double(1 << 24)));
            };
            const std::array<std::int32_t, 4> bounds{std::max(0, edge(clip[0] * scale, false)),
                                                     std::max(0, edge(clip[1] * scale, false)),
                                                     std::min(std::int32_t(w), edge(clip[2] * scale, true)),
                                                     std::min(std::int32_t(h), edge(clip[3] * scale, true))};
            return vector::mask_bounds(list.paths[item.payload], device, bounds);
        }

        // The map from a plane's content image (in CSS px, the content image_margin px in) to target px: the plane's
        // homography after the shift by the margin.
        geometry::Plane plane_map(const paint::Plane& plane) {
            const double m = scene_limits::image_margin;
            const auto& h = plane.transform;
            return {{h[0], h[1], h[2] - m * (h[0] + h[1]), h[3], h[4], h[5] - m * (h[3] + h[4]), h[6], h[7],
                     h[8] - m * (h[6] + h[7])}};
        }

        // A plane's record: its content image projected. The inverse of the image's map gives the rows the shader
        // takes, the third (the depth) in outline; the bounds hold the projected corners. None when a corner of the
        // image lies behind the viewer or the map is singular.
        std::optional<SceneRecord> plane_record(const paint::Plane& plane, const paint::Item& item,
                                                const paint::ClipRect& clip, float scale) {
            const auto map = plane_map(plane);
            geometry::Plane inverse;
            if (!map.inverse(inverse)) {
                return std::nullopt;
            }
            const auto pixels = plane_pixels(plane, scale);
            const double w = pixels[0] / double(scale);
            const double h = pixels[1] / double(scale);
            double left = scene_limits::record_bounds_sentinel;
            double top = scene_limits::record_bounds_sentinel;
            double right = -left;
            double bottom = -top;
            for (const auto& [x, y] : std::array<std::array<double, 2>, 4>{{{0, 0}, {w, 0}, {0, h}, {w, h}}}) {
                const auto& t = map.m;
                const double z = t[6] * x + t[7] * y + t[8];
                if (!(z > constants::plane_min_depth)) {
                    return std::nullopt;
                }
                const double px = (t[0] * x + t[1] * y + t[2]) / z;
                const double py = (t[3] * x + t[4] * y + t[5]) / z;
                left = std::min(left, px);
                top = std::min(top, py);
                right = std::max(right, px);
                bottom = std::max(bottom, py);
            }
            SceneRecord r{};
            r.uv = {0, 0, 1, 1};
            r.clips = {float(item.clips_begin), float(item.clips_count), 0, 0};
            r.clip = clip;
            r.size = {float(w), float(h), 0, 0};
            r.extra = {0, item.opacity, scene_limits::flag_enabled, scene_limits::flag_disabled};
            r.mode = {scene_limits::image_mode_projective, 0, 0, 0};
            const auto& v = inverse.m;
            r.inv_x = {float(v[0]), float(v[1]), float(v[2]), 0};
            r.inv_y = {float(v[3]), float(v[4]), float(v[5]), 0};
            r.outline = {float(v[6]), float(v[7]), float(v[8]), 0};
            const double outset = scene_limits::record_bounds_outset / double(scale);
            const double x0 = std::max(left, double(clip[0])) - outset;
            const double y0 = std::max(top, double(clip[1])) - outset;
            const double x1 = std::min(right, double(clip[2])) + outset;
            const double y1 = std::min(bottom, double(clip[3])) + outset;
            r.bounds = {float(x0), float(y0), float(std::max(0.0, x1 - x0)), float(std::max(0.0, y1 - y0))};
            return r;
        }

        SceneRecord record(const paint::DisplayList& list, const paint::Item& item, const paint::ClipRect& clip,
                           float scale, unsigned w, unsigned h, Space space, const std::array<std::int32_t, 4>& mask) {
            auto m = item.transform;
            const bool image = item.kind == Kind::Text || item.kind == Kind::Layer || item.kind == Kind::Image ||
                               item.kind == Kind::Shadow || item.kind == Kind::Path || item.kind == Kind::Effect;
            SceneRecord r{};
            r.uv = {0, 0, 1, 1};
            r.clips = {float(item.clips_begin), float(item.clips_count), 0, 0};
            r.size = {item.width, item.height, 0, 0};
            r.clip = clip;
            r.extra = {0, item.opacity, image ? scene_limits::flag_enabled : scene_limits::flag_disabled,
                       scene_limits::flag_disabled};
            if (item.kind == Kind::Box) {
                const auto& box = list.boxes[item.payload];
                const auto& g = box.gradient;
                r.size[2] = float(static_cast<int>(g.kind));
                r.size[3] = float(g.stops.size());
                r.radii = box.radii;
                r.widths = box.widths;
                r.background = in(space, box.background);
                for (unsigned i = 0; i < r.edges.size(); ++i) {
                    r.edges[i] = in(space, box.edges[i]);
                }
                r.tile = g.tile;
                r.extra[0] = g.angle;
                for (unsigned i = 0; i < g.stops.size(); ++i) {
                    r.stops[i] = in(space, g.stops[i].color);
                    r.offsets[i / scene_limits::stops_per_offset_vector][i % scene_limits::stops_per_offset_vector] =
                        g.stops[i].offset;
                }
            } else if (item.kind == Kind::Image) {
                const auto& source = list.images[item.payload];
                r.uv = source.uv;
                r.background = in(space, source.color);
                r.outline = in(space, source.outline);
                // External textures are images with their own colors.
                const auto* asset = source.asset < list.assets.size() ? list.assets[source.asset].get() : nullptr;
                if (asset && asset->encoding == paint::Encoding::Coverage) {
                    r.mode[0] = scene_limits::image_mode_coverage;
                } else if (asset && asset->encoding == paint::Encoding::SignedDistance) {
                    r.mode = {scene_limits::image_mode_signed_distance, asset->spread, source.stroke, 0};
                }
            } else if (item.kind == Kind::Path) {
                // Coverage tinted with the path's color.
                r.background = in(space, list.paths[item.payload].color);
                r.extra[3] = scene_limits::flag_enabled;
            } else if (item.kind == Kind::Effect) {
                // The time and the parameters as the program's function reads them, unconverted; the corners shape the
                // box as a box's.
                const auto& effect = list.effects[item.payload];
                r.radii = effect.radii;
                r.extra[0] = effect.time;
                for (unsigned p = 0; p < effect.parameters.size(); ++p) {
                    effect_parameter(r, p) = effect.parameters[p];
                }
            } else if ((item.kind == Kind::Text || item.kind == Kind::Layer) && list.rasters[item.payload].mask) {
                r.background = in(space, list.rasters[item.payload].tint);
                r.extra[3] = scene_limits::flag_enabled;
            }
            if (item.kind == Kind::EndGroup) {
                m = paint::identity;
                r.size = {float(w) / scale, float(h) / scale, 0, 0};
                r.extra = {0, item.opacity, scene_limits::flag_enabled, scene_limits::flag_disabled};
            } else if (item.kind == Kind::Text || item.kind == Kind::Layer) {
                // Scene rasters keep a transparent margin around the node, more where ink leaves it.
                const float margin = scene_limits::image_margin + list.rasters[item.payload].overhang;
                m[4] -= margin * (m[0] + m[2]);
                m[5] -= margin * (m[1] + m[3]);
                r.size[0] += 2 * margin;
                r.size[1] += 2 * margin;
            } else if (item.kind == Kind::Path) {
                // The mask lies in device pixels, texel for pixel.
                m = {1, 0, 0, 1, mask[0] / scale, mask[1] / scale};
                r.size[0] = float(mask[2] - mask[0]) / scale;
                r.size[1] = float(mask[3] - mask[1]) / scale;
            } else if (item.kind == Kind::Shadow) {
                // A shadow image covers its own rectangle around the box, with its own premultiplied colors.
                const auto g = shadow_geometry(list.shadows[item.payload], item.width, item.height, scale);
                m[4] += g.rect[0] * m[0] + g.rect[1] * m[2];
                m[5] += g.rect[0] * m[1] + g.rect[1] * m[3];
                r.size[0] = g.rect[2];
                r.size[1] = g.rect[3];
            }
            float det = m[0] * m[3] - m[1] * m[2];
            r.inv_x = {m[3] / det, -m[2] / det, (m[2] * m[5] - m[3] * m[4]) / det, 0};
            r.inv_y = {-m[1] / det, m[0] / det, (m[1] * m[4] - m[0] * m[5]) / det, 0};
            if (item.kind == Kind::Image && r.mode[0] == scene_limits::image_mode_signed_distance && r.size[0] > 0 &&
                r.size[1] > 0) {
                // A distance field changes one source texel per texel, so its edges antialias over the texels that one
                // device pixel spans (the mean of both axes), from the item's mapping.
                const auto& a = *list.assets[list.images[item.payload].asset];
                const float tx = a.width * r.uv[2] / r.size[0] / scale;
                const float ty = a.height * r.uv[3] / r.size[1] / scale;
                r.mode[3] =
                    (std::hypot(tx * r.inv_x[0], ty * r.inv_y[0]) + std::hypot(tx * r.inv_x[1], ty * r.inv_y[1])) / 2;
            }
            float left = scene_limits::record_bounds_sentinel;
            float top = scene_limits::record_bounds_sentinel;
            float right = -scene_limits::record_bounds_sentinel;
            float bottom = -scene_limits::record_bounds_sentinel;
            for (auto point : std::array<std::array<float, 2>, 4>{
                     {{0, 0}, {r.size[0], 0}, {0, r.size[1]}, {r.size[0], r.size[1]}}}) {
                float x = m[0] * point[0] + m[2] * point[1] + m[4];
                float y = m[1] * point[0] + m[3] * point[1] + m[5];
                left = std::min(left, x);
                top = std::min(top, y);
                right = std::max(right, x);
                bottom = std::max(bottom, y);
            }
            // Bounds reach past the item by the antialiasing outset, and at most that far past its clip: the shader draws
            // nothing outside the clip, so an item clipped away covers no pixels. A linear pass's axis-aligned images
            // and boxes without corners cover the pixels whose centers they contain and need only half a pixel.
            const bool pixel_centers =
                space == Space::Linear && m[1] == 0 && m[2] == 0 &&
                (item.kind == Kind::Image || ((item.kind == Kind::Box || item.kind == Kind::Effect) &&
                                              std::all_of(r.radii.begin(), r.radii.end(), [](float v) {
                                                  return v == 0;
                                              })));
            const float outset =
                (pixel_centers ? scene_limits::pixel_center_outset : scene_limits::record_bounds_outset) / scale;
            const float x0 = std::max(left - outset, clip[0] - outset);
            const float y0 = std::max(top - outset, clip[1] - outset);
            const float x1 = std::min(right + outset, clip[2] + outset);
            const float y1 = std::min(bottom + outset, clip[3] + outset);
            r.bounds = {x0, y0, std::max(0.f, x1 - x0), std::max(0.f, y1 - y0)};
            return r;
        }

        // An opaque box that exactly covers the target inside its clip (crop included), as the first item, is the
        // clear color.
        bool clears(const paint::DisplayList& list, const paint::ClipRect& clip, float scale, unsigned w, unsigned h) {
            if (list.items.empty() || list.items.front().kind != Kind::Box) {
                return false;
            }
            const auto& p = list.items.front();
            const auto& box = list.boxes[p.payload];
            const auto zero = [](float v) {
                return v == 0;
            };
            return p.transform == paint::identity && p.width * scale == w && p.height * scale == h && clip[0] <= 0 &&
                   clip[1] <= 0 && clip[2] >= p.width && clip[3] >= p.height && p.opacity == 1 &&
                   box.background[3] == 1 && box.gradient.kind == paint::GradientKind::None &&
                   std::all_of(box.radii.begin(), box.radii.end(), zero) &&
                   std::all_of(box.widths.begin(), box.widths.end(), zero);
        }
    } // namespace

    Result<ScenePass> plan_scene_pass(const paint::DisplayList& list, float scale, unsigned width, unsigned height,
                                      std::optional<paint::ClipRect> crop, Space space,
                                      const std::vector<std::uint64_t>* textures) {
        ScenePass pass;
        pass.space = space;
        pass.clips.reserve(list.clips.size());
        for (const auto& c : list.clips) {
            try {
                const auto n = normalize_rounded(c);
                const auto& m = n.to_local;
                // Target px per unit of the clip's space: one over the square root of the map's area scale.
                const float span = 1 / std::sqrt(std::abs(m[0] * m[3] - m[1] * m[2]));
                pass.clips.push_back({n.rect, n.radius_x, n.radius_y, {m[0], m[2], m[4], span}, {m[1], m[3], m[5], 0}});
            } catch (const std::invalid_argument& e) {
                return make_diagnostic(Diag::PaintList, e.what());
            }
        }
        pass.records.reserve(list.items.size());
        pass.masks.reserve(list.items.size());
        for (const auto& item : list.items) {
            if (item.clips_count > std::uint32_t(scene_limits::max_rounded_clips)) {
                return make_diagnostic(Diag::PaintList, "rounded clip chain bound");
            }
            auto clip = item.clip;
            if (crop) {
                clip = {std::max(clip[0], (*crop)[0]), std::max(clip[1], (*crop)[1]), std::min(clip[2], (*crop)[2]),
                        std::min(clip[3], (*crop)[3])};
            }
            pass.masks.push_back(item.kind == Kind::Path ? path_mask(list, item, clip, scale, width, height)
                                                         : std::array<std::int32_t, 4>{});
            if (item.kind == Kind::Plane) {
                auto r = plane_record(list.planes[item.payload], item, clip, scale);
                if (!r) {
                    return make_diagnostic(Diag::PaintList, "plane image behind the viewer or singular");
                }
                pass.records.push_back(*r);
                continue;
            }
            pass.records.push_back(record(list, item, clip, scale, width, height, space, pass.masks.back()));
        }
        pass.group_depth = paint::group_depth(list);
        std::uint32_t i{};
        if (!pass.records.empty() && !list.items.front().clips_count &&
            clears(list, pass.records.front().clip, scale, width, height)) {
            pass.clear = in(space, list.boxes[list.items.front().payload].background);
            i = 1;
        }
        using Step = ScenePass::Step;
        while (i < list.items.size()) {
            switch (list.items[i].kind) {
                case Kind::BeginGroup:
                    pass.batches.push_back({Step::BeginGroup, i++, 0});
                    break;
                case Kind::EndGroup:
                    pass.batches.push_back({Step::EndGroup, i++, 1});
                    break;
                case Kind::Text:
                case Kind::Layer:
                case Kind::Shadow:
                case Kind::Path:
                case Kind::Plane:
                    pass.batches.push_back({Step::Image, i++, 1});
                    break;
                case Kind::Effect: {
                    // A run of effects of one program and the same textures.
                    const auto first = i;
                    const auto& head = list.effects[list.items[i].payload];
                    for (++i; i < list.items.size() && list.items[i].kind == Kind::Effect; ++i) {
                        const auto& next = list.effects[list.items[i].payload];
                        if (next.program != head.program || next.textures != head.textures) {
                            break;
                        }
                    }
                    pass.batches.push_back({Step::Effect, first, i - first, list.items[first].payload});
                    break;
                }
                case Kind::Image:
                    if (!textures) {
                        pass.batches.push_back({Step::Image, i, 1, list.images[list.items[i].payload].asset});
                        ++i;
                        break;
                    }
                    [[fallthrough]];
                default: {
                    // Boxes, and with texture names the images among them that sample one texture.
                    const auto first = i;
                    std::optional<std::uint64_t> texture;
                    std::uint32_t source{};
                    for (; i < list.items.size(); ++i) {
                        const auto& next = list.items[i];
                        if (next.kind == Kind::Box) {
                            continue;
                        }
                        if (next.kind != Kind::Image || !textures) {
                            break;
                        }
                        const auto image = list.images[next.payload].asset;
                        if (texture && *texture != textures->at(image)) {
                            break;
                        }
                        if (!texture) {
                            texture = textures->at(image);
                            source = image;
                        }
                    }
                    pass.batches.push_back({texture ? Step::Image : Step::Boxes, first, i - first, source});
                    break;
                }
            }
        }
        return pass;
    }
} // namespace weft::render
