#include "render/scene_pass.hpp"

#include "base/plane.hpp"
#include "constants/common.hpp"
#include "paint/particles.hpp"
#include "render/rounded_clip.hpp"
#include "render/shadow_image.hpp"
#include "render/srgb.hpp"
#include "vector/path_raster.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace weft::render {
    BackdropConstants backdrop_area_constants(unsigned from, unsigned to, bool across) {
        BackdropConstants c;
        // Source texels per output texel, the axis and the source's texels on it.
        c.texel = {float(from) / float(to), across ? 1.f : 0.f, float(from), 0};
        return c;
    }

    BackdropConstants backdrop_blur_constants(float sigma, bool across, unsigned w, unsigned h) {
        BackdropConstants c;
        c.texel = {1.f / float(w), 1.f / float(h), across ? 1.f / float(w) : 0.f, across ? 0.f : 1.f / float(h)};
        const auto taps =
            std::min(int(std::ceil(scene_limits::backdrop_kernel_sigmas * sigma)), scene_limits::max_backdrop_taps);
        std::array<float, 16> w16{};
        float sum{};
        for (int i = 0; i <= taps; ++i) {
            w16[std::size_t(i)] = std::exp(-float(i * i) / (2 * sigma * sigma));
            sum += i ? 2 * w16[std::size_t(i)] : w16[std::size_t(i)];
        }
        for (int i = 0; i <= taps; ++i) {
            c.weights[std::size_t(i) / 4][std::size_t(i) % 4] = w16[std::size_t(i)] / sum;
        }
        c.extent = {float(taps), 0, 0, 0};
        return c;
    }

    BackdropConstants backdrop_matrix_constants(const paint::ColorMatrix& m) {
        BackdropConstants c;
        for (unsigned k = 0; k < 4; ++k) {
            c.rows[k] = {m[k * 5], m[k * 5 + 1], m[k * 5 + 2], m[k * 5 + 3]};
        }
        c.offsets = {m[4], m[9], m[14], m[19]};
        return c;
    }

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
        // and the target (in its element's space, an item an anchor moves has no target).
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
            const bool target = !item.anchor;
            const std::array<std::int32_t, 4> bounds{
                target ? std::max(0, edge(clip[0] * scale, false)) : edge(clip[0] * scale, false),
                target ? std::max(0, edge(clip[1] * scale, false)) : edge(clip[1] * scale, false),
                target ? std::min(std::int32_t(w), edge(clip[2] * scale, true)) : edge(clip[2] * scale, true),
                target ? std::min(std::int32_t(h), edge(clip[3] * scale, true)) : edge(clip[3] * scale, true)};
            return vector::mask_bounds(list.paths[item.payload], device, bounds);
        }

        // The device pixels a path shadow's coverage covers: its box under the item's map at scale, rounded out, within
        // its clip (and the target, unless an anchor moves it).
        std::array<std::int32_t, 4> shadow_mask(const paint::Item& item, const paint::ClipRect& clip, float scale,
                                                unsigned w, unsigned h) {
            const auto& m = item.transform;
            double left = scene_limits::record_bounds_sentinel;
            double top = scene_limits::record_bounds_sentinel;
            double right = -left;
            double bottom = -top;
            for (const auto& [x, y] : std::array<std::array<double, 2>, 4>{
                     {{0, 0}, {item.width, 0}, {0, item.height}, {item.width, item.height}}}) {
                const double px = (m[0] * x + m[2] * y + m[4]) * scale;
                const double py = (m[1] * x + m[3] * y + m[5]) * scale;
                left = std::min(left, px);
                top = std::min(top, py);
                right = std::max(right, px);
                bottom = std::max(bottom, py);
            }
            const auto edge = [](double v, bool up) {
                const double d = up ? std::ceil(v) : std::floor(v);
                return std::int32_t(std::clamp(d, -double(1 << 24), double(1 << 24)));
            };
            const bool target = !item.anchor;
            std::array<std::int32_t, 4> out{std::max(edge(left, false), edge(clip[0] * scale, false)),
                                            std::max(edge(top, false), edge(clip[1] * scale, false)),
                                            std::min(edge(right, true), edge(clip[2] * scale, true)),
                                            std::min(edge(bottom, true), edge(clip[3] * scale, true))};
            if (target) {
                out = {std::max(0, out[0]), std::max(0, out[1]), std::min(std::int32_t(w), out[2]),
                       std::min(std::int32_t(h), out[3])};
            }
            if (out[2] <= out[0] || out[3] <= out[1]) {
                return {};
            }
            return out;
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

        // The device pixels a backdrop reads: its box's bounds under the item's map at scale, rounded out, within the
        // target.
        std::array<std::int32_t, 4> backdrop_region(const paint::Item& item, float scale, unsigned w, unsigned h) {
            const auto& m = item.transform;
            double left = scene_limits::record_bounds_sentinel;
            double top = scene_limits::record_bounds_sentinel;
            double right = -left;
            double bottom = -top;
            for (const auto& [x, y] : std::array<std::array<double, 2>, 4>{
                     {{0, 0}, {item.width, 0}, {0, item.height}, {item.width, item.height}}}) {
                const double px = (m[0] * x + m[2] * y + m[4]) * scale;
                const double py = (m[1] * x + m[3] * y + m[5]) * scale;
                left = std::min(left, px);
                top = std::min(top, py);
                right = std::max(right, px);
                bottom = std::max(bottom, py);
            }
            const auto edge = [](double v, bool up, unsigned limit) {
                return std::int32_t(std::clamp(up ? std::ceil(v) : std::floor(v), 0.0, double(limit)));
            };
            return {edge(left, false, w), edge(top, false, h), edge(right, true, w), edge(bottom, true, h)};
        }

        SceneRecord record(const paint::DisplayList& list, const paint::Item& item, const paint::ClipRect& clip,
                           float scale, unsigned w, unsigned h, Space space, const std::array<std::int32_t, 4>& mask) {
            auto m = item.transform;
            const bool image = item.kind == Kind::Text || item.kind == Kind::Layer || item.kind == Kind::Image ||
                               item.kind == Kind::Shadow || item.kind == Kind::Path || item.kind == Kind::Effect ||
                               item.kind == Kind::PathShadow;
            SceneRecord r{};
            r.uv = {0, 0, 1, 1};
            r.clips = {float(item.clips_begin), float(item.clips_count), float(item.anchor), 0};
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
                // A radial gradient's ending shape takes the image's fields, which a box does not sample.
                if (g.kind == paint::GradientKind::Radial) {
                    r.uv = g.shape;
                }
                r.mode[1] = g.repeating ? scene_limits::flag_enabled : scene_limits::flag_disabled;
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
                // A host texture's linear colors, in the pass's encoding, laid out as its fit says (scene.hlsl,
                // fittedTexture); external textures are images with their own colors.
                if (source.texture) {
                    const auto& f = source.fit;
                    r.mode[0] = scene_limits::image_mode_texture;
                    r.stops[0] = f.area;
                    r.stops[1] = {f.size[0], f.size[1], float(static_cast<int>(f.scale)), 0};
                    r.stops[2] = {f.fraction[0], f.fraction[1], f.offset[0], f.offset[1]};
                    r.stops[3] = {float(static_cast<int>(f.repeat[0])), float(static_cast<int>(f.repeat[1])), 0, 0};
                }
                const auto* asset =
                    !source.texture && source.asset < list.assets.size() ? list.assets[source.asset].get() : nullptr;
                if (asset && asset->encoding == paint::Encoding::Coverage) {
                    r.mode[0] = scene_limits::image_mode_coverage;
                } else if (asset && asset->encoding == paint::Encoding::SignedDistance) {
                    r.mode = {scene_limits::image_mode_signed_distance, asset->spread, source.stroke, 0};
                }
            } else if (item.kind == Kind::Path) {
                // Coverage, a byte per texel, tinted with the path's color.
                r.background = in(space, list.paths[item.payload].color);
                r.extra[3] = scene_limits::flag_enabled;
            } else if (item.kind == Kind::PathShadow) {
                // Coverage, a byte per texel, tinted with the shadow's color.
                r.background = in(space, list.path_shadows[item.payload].color);
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
            } else if (item.kind == Kind::Particles) {
                // The emission as the particle shaders read it (particle_field), its timing from the shared model; the
                // colors straight and linear, as the model interpolates them.
                const auto& particles = list.particles[item.payload];
                const auto& e = particles.emitter;
                const auto& emission = particles.emission;
                const auto timing = paint::particle_timing(particles);
                const bool burst = emission.kind == paint::ParticleEmission::Kind::Burst;
                particle_field(r, ParticleMapX) = {m[0], m[2], m[4], 0};
                particle_field(r, ParticleMapY) = {m[1], m[3], m[5], 0};
                particle_field(r, ParticleTiming) = {timing.age, timing.interval, float(timing.newest),
                                                     burst ? scene_limits::flag_enabled : scene_limits::flag_disabled};
                particle_field(r, ParticleEmission) = {float(timing.count), emission.duration,
                                                       float(emission.seed % constants::particle_index_period),
                                                       float(static_cast<int>(e.shape))};
                particle_field(r, ParticleArea) = e.area;
                particle_field(r, ParticleLifeSpeed) = {e.life[0], e.life[1], e.speed[0], e.speed[1]};
                particle_field(r, ParticleHeading) = {e.direction, e.spread, e.drag, e.softness};
                particle_field(r, ParticleForces) = {e.gravity[0], e.gravity[1], e.spin[0], e.spin[1]};
                particle_field(r, ParticleTurn) = {e.angle[0], e.angle[1], e.scale[0], e.scale[1]};
                particle_field(r, ParticleSize) = e.size;
                for (unsigned k = 0; k < e.color.size(); ++k) {
                    particle_field(r, ParticleField(ParticleColor + k)) = e.color[k];
                }
                float mode = scene_limits::particle_sprite_none;
                if (e.sprite) {
                    mode = list.assets[*e.sprite]->encoding == paint::Encoding::Coverage
                               ? scene_limits::particle_sprite_coverage
                               : scene_limits::particle_sprite_color;
                }
                particle_field(r, ParticleSprite) = {float(e.frames[0]), float(e.frames[1]), e.frame_rate, mode};
                if (emission.target) {
                    particle_field(r, ParticleTarget) = {(*emission.target)[0], (*emission.target)[1],
                                                         scene_limits::flag_enabled, 0};
                }
            } else if (item.kind == Kind::Backdrop) {
                // The box's shape, its color matrix after the blur when it has one, and the device px of its region,
                // which the blurred copy covers.
                const auto& b = list.backdrops[item.payload];
                r.radii = b.radii;
                r.mode[0] = scene_limits::image_mode_backdrop;
                if (b.post != paint::identity_color_matrix) {
                    for (unsigned k = 0; k < 4; ++k) {
                        effect_parameter(r, k) = {b.post[k * 5], b.post[k * 5 + 1], b.post[k * 5 + 2],
                                                  b.post[k * 5 + 3]};
                    }
                    effect_parameter(r, 4) = {b.post[4], b.post[9], b.post[14], b.post[19]};
                    r.extra[3] = scene_limits::flag_enabled;
                }
                const auto region = backdrop_region(item, scale, w, h);
                r.uv = {float(region[0]), float(region[1]), float(region[2] - region[0]), float(region[3] - region[1])};
            } else if ((item.kind == Kind::Text || item.kind == Kind::Layer) && list.rasters[item.payload].mask) {
                // A mask tinted with the text's color: a text mask's coverage is a byte per texel.
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
            } else if (item.kind == Kind::Path || item.kind == Kind::PathShadow) {
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
                                      const std::vector<std::uint64_t>* textures, std::uint32_t anchors, bool fold) {
        // A linear pass (a display list's) binds no textures by name.
        if (space == Space::Linear && !list.textures.empty()) {
            return make_diagnostic(Diag::PaintList, "named textures in a linear pass");
        }
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
            if (item.anchor > anchors) {
                return make_diagnostic(Diag::PaintList, "item anchor out of range");
            }
            auto clip = item.clip;
            if (crop && !item.anchor) {
                clip = {std::max(clip[0], (*crop)[0]), std::max(clip[1], (*crop)[1]), std::min(clip[2], (*crop)[2]),
                        std::min(clip[3], (*crop)[3])};
            }
            pass.masks.push_back(item.kind == Kind::Path         ? path_mask(list, item, clip, scale, width, height)
                                 : item.kind == Kind::PathShadow ? shadow_mask(item, clip, scale, width, height)
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
        if (fold && !pass.records.empty() && !list.items.front().clips_count &&
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
                case Kind::PathShadow:
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
                case Kind::Backdrop: {
                    // A backdrop over no pixel of the target reads and draws nothing.
                    const auto& item = list.items[i];
                    const auto& b = list.backdrops[item.payload];
                    ScenePass::Backdrop step;
                    step.region = backdrop_region(item, scale, width, height);
                    step.pre = b.pre;
                    step.under = b.under;
                    // The blur in device px, by the item's mean scale. A wider one than the passes blur scales the
                    // copy down first, to the whole texels that bring it nearest the passes' (at least one), as Skia
                    // does; the blur at that size follows from the scale on each axis.
                    const auto& m = item.transform;
                    const float sigma = b.blur * scale * std::sqrt(std::abs(m[0] * m[3] - m[1] * m[2]));
                    const auto w = step.region[2] - step.region[0];
                    const auto h = step.region[3] - step.region[1];
                    step.size = {unsigned(std::max(w, 0)), unsigned(std::max(h, 0))};
                    step.sigma = {sigma, sigma};
                    if (sigma > scene_limits::max_backdrop_sigma && w > 0 && h > 0) {
                        const float fraction = scene_limits::max_backdrop_sigma / sigma;
                        step.size = {unsigned(std::max(1.f, std::floor(float(w) * fraction))),
                                     unsigned(std::max(1.f, std::floor(float(h) * fraction)))};
                        step.sigma = {sigma * float(step.size[0]) / float(w), sigma * float(step.size[1]) / float(h)};
                    }
                    if (w > 0 && h > 0) {
                        pass.batches.push_back({Step::Backdrop, i, 1, std::uint32_t(pass.backdrops.size())});
                        pass.backdrops.push_back(step);
                    }
                    ++i;
                    break;
                }
                case Kind::Particles: {
                    // One emission's particles in one draw; none alive, no draw.
                    const auto payload = list.items[i].payload;
                    if (const auto count = paint::particle_timing(list.particles[payload]).count) {
                        pass.batches.push_back({Step::Particles, i, count, payload});
                    }
                    ++i;
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
