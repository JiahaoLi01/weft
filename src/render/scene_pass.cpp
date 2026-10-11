#include "render/scene_pass.hpp"

#include <algorithm>

namespace weft::render {
    namespace {
        using Kind = paint::Item::Kind;

        SceneRecord record(const paint::DisplayList& list, const paint::Item& item, const paint::ClipRect& clip,
                           float scale, unsigned w, unsigned h) {
            auto m = item.transform;
            const bool image = item.kind == Kind::Text || item.kind == Kind::Layer;
            SceneRecord r{};
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
                r.background = box.background;
                r.edges = box.edges;
                r.tile = g.tile;
                r.extra[0] = g.angle;
                for (unsigned i = 0; i < g.stops.size(); ++i) {
                    r.stops[i] = g.stops[i].color;
                    r.offsets[i / scene_limits::stops_per_offset_vector][i % scene_limits::stops_per_offset_vector] =
                        g.stops[i].offset;
                }
            } else if (image && list.rasters[item.payload].mask) {
                r.background = list.rasters[item.payload].tint;
                r.extra[3] = scene_limits::flag_enabled;
            }
            if (item.kind == Kind::EndGroup) {
                m = paint::identity;
                r.size = {float(w) / scale, float(h) / scale, 0, 0};
                r.extra = {0, item.opacity, scene_limits::flag_enabled, scene_limits::flag_disabled};
            } else if (image) {
                m[4] -= scene_limits::image_margin * (m[0] + m[2]);
                m[5] -= scene_limits::image_margin * (m[1] + m[3]);
                r.size[0] += scene_limits::image_margins;
                r.size[1] += scene_limits::image_margins;
            }
            float det = m[0] * m[3] - m[1] * m[2];
            r.inv_x = {m[3] / det, -m[2] / det, (m[2] * m[5] - m[3] * m[4]) / det, 0};
            r.inv_y = {-m[1] / det, m[0] / det, (m[1] * m[4] - m[0] * m[5]) / det, 0};
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
            r.bounds = {left - scene_limits::record_bounds_outset / scale,
                        top - scene_limits::record_bounds_outset / scale,
                        right - left + scene_limits::record_bounds_outsets / scale,
                        bottom - top + scene_limits::record_bounds_outsets / scale};
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

    ScenePass plan_scene_pass(const paint::DisplayList& list, float scale, unsigned width, unsigned height,
                              std::optional<paint::ClipRect> crop) {
        ScenePass pass;
        pass.records.reserve(list.items.size());
        for (const auto& item : list.items) {
            auto clip = item.clip;
            if (crop) {
                clip = {std::max(clip[0], (*crop)[0]), std::max(clip[1], (*crop)[1]), std::min(clip[2], (*crop)[2]),
                        std::min(clip[3], (*crop)[3])};
            }
            pass.records.push_back(record(list, item, clip, scale, width, height));
        }
        pass.group_depth = paint::group_depth(list);
        std::uint32_t i{};
        if (!pass.records.empty() && clears(list, pass.records.front().clip, scale, width, height)) {
            pass.clear = list.boxes[list.items.front().payload].background;
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
                    pass.batches.push_back({Step::Image, i++, 1});
                    break;
                default: {
                    const auto first = i++;
                    while (i < list.items.size() && list.items[i].kind == Kind::Box) {
                        ++i;
                    }
                    pass.batches.push_back({Step::Boxes, first, i - first});
                    break;
                }
            }
        }
        return pass;
    }
} // namespace weft::render
