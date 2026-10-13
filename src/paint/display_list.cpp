#include "paint/display_list.hpp"

#include "base/plane.hpp"
#include "constants/common.hpp"

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

    namespace {
        Status check_at(const DisplayList& list, std::size_t max_group_depth, unsigned planes);
    } // namespace

    Status check(const DisplayList& list, std::size_t max_group_depth) {
        return check_at(list, max_group_depth, 0);
    }

    namespace {
        // A plane's homography is invertible, and its corners lie in front of the viewer.
        bool plane_geometry(const Plane& p) {
            geometry::Plane map;
            std::copy(p.transform.begin(), p.transform.end(), map.m.begin());
            geometry::Plane inverse;
            if (!map.inverse(inverse)) {
                return false;
            }
            const auto& h = map.m;
            for (const auto& [x, y] :
                 std::array<std::array<double, 2>, 4>{{{0, 0}, {p.width, 0}, {0, p.height}, {p.width, p.height}}}) {
                if (!(h[6] * x + h[7] * y + h[8] > constants::plane_min_depth)) {
                    return false;
                }
            }
            return true;
        }

        // An emitter's and an emission's values (DisplayList::check).
        Status check_particles(const DisplayList& list, const Particles& p) {
            const auto& m = p.emitter;
            const auto& e = p.emission;
            const auto finite = [](const auto& values) {
                return std::all_of(values.begin(), values.end(), finite_value);
            };
            const auto ordered = [](const std::array<float, 2>& range) {
                return range[0] <= range[1];
            };
            const auto non_negative = [](const auto& values) {
                return std::all_of(values.begin(), values.end(), [](float v) {
                    return v >= 0;
                });
            };
            const bool colors = std::all_of(m.color.begin(), m.color.end(), [&](const Color& c) {
                return finite(c) && non_negative(c) && c[3] <= 1;
            });
            if (!finite(m.area) || !finite(m.life) || !finite(m.speed) || !finite(m.gravity) || !finite(m.angle) ||
                !finite(m.spin) || !finite(m.size) || !finite(m.scale) || !std::isfinite(m.rate) ||
                !std::isfinite(m.direction) || !std::isfinite(m.spread) || !std::isfinite(m.drag) ||
                !std::isfinite(m.softness) || !std::isfinite(m.frame_rate) || !colors) {
                return make_diagnostic(Diag::PaintList, "particle emitter value");
            }
            if (!ordered(m.life) || !ordered(m.speed) || !ordered(m.angle) || !ordered(m.spin) || !ordered(m.scale) ||
                !(m.life[0] > 0) || m.rate < 0 || m.speed[0] < 0 || m.drag < 0 || !non_negative(m.size) ||
                m.scale[0] < 0 || m.softness < 0 || m.softness > 1 || m.frame_rate < 0 ||
                (m.shape != ParticleEmitter::Shape::Point && (m.area[2] < 0 || m.area[3] < 0))) {
                return make_diagnostic(Diag::PaintList, "particle emitter range");
            }
            if (m.sprite) {
                const auto frames = std::uint64_t(m.frames[0]) * m.frames[1];
                if (*m.sprite >= list.assets.size() || !list.assets[*m.sprite] ||
                    list.assets[*m.sprite]->encoding == Encoding::SignedDistance || frames == 0 ||
                    frames > constants::max_particle_sprite_frames) {
                    return make_diagnostic(Diag::PaintList, "particle sprite");
                }
            }
            if (!std::isfinite(e.elapsed) || e.elapsed < 0 || !std::isfinite(e.duration) || e.duration < 0 ||
                (e.stopped && (!std::isfinite(*e.stopped) || *e.stopped < 0 || *e.stopped > e.elapsed)) ||
                (e.target && !finite(*e.target))) {
                return make_diagnostic(Diag::PaintList, "particle emission value");
            }
            const double count = e.kind == ParticleEmission::Kind::Burst
                                     ? double(e.count)
                                     : std::ceil(double(std::max(m.life[0], m.life[1])) * double(m.rate)) + 1;
            if ((e.kind == ParticleEmission::Kind::Burst && e.count == 0) ||
                count > constants::max_particles_per_emission) {
                return make_diagnostic(Diag::PaintList, "particle emission count");
            }
            return {};
        }

        // planes counts the plane contents around list: planes inside a plane's content and so on.
        Status check_at(const DisplayList& list, std::size_t max_group_depth, unsigned planes) {
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
                        if (image.asset < list.assets.size()
                                ? !list.assets[image.asset]
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
                            return make_diagnostic(Diag::PaintList,
                                                   "raster payload out of range or without source" + at);
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
                    case Item::Kind::Plane: {
                        if (item.payload >= list.planes.size()) {
                            return make_diagnostic(Diag::PaintList, "plane payload out of range" + at);
                        }
                        const auto& p = list.planes[item.payload];
                        if (!p.content || !std::all_of(p.transform.begin(), p.transform.end(), finite_value) ||
                            !(p.width > 0) || !(p.height > 0) || !std::isfinite(p.width) || !std::isfinite(p.height)) {
                            return make_diagnostic(Diag::PaintList, "plane value" + at);
                        }
                        if (!plane_geometry(p)) {
                            return make_diagnostic(Diag::PaintList, "plane behind the viewer or singular" + at);
                        }
                        if (planes >= constants::max_plane_depth) {
                            return make_diagnostic(Diag::PaintList, "plane depth bound" + at);
                        }
                        if (auto nested = check_at(*p.content, max_group_depth, planes + 1); !nested) {
                            return nested.error();
                        }
                        break;
                    }
                    case Item::Kind::Effect: {
                        if (item.payload >= list.effects.size()) {
                            return make_diagnostic(Diag::PaintList, "effect payload out of range" + at);
                        }
                        const auto& e = list.effects[item.payload];
                        if (e.program >= list.programs.size() || !list.programs[e.program]) {
                            return make_diagnostic(Diag::PaintList, "effect program out of range" + at);
                        }
                        const auto& program = *list.programs[e.program];
                        if (program.parameters > constants::max_effect_parameters ||
                            program.textures > constants::max_effect_textures ||
                            e.parameters.size() > program.parameters || e.textures.size() != program.textures ||
                            (!program.named_parameters.empty() &&
                             program.named_parameters.size() != program.parameters) ||
                            (!program.named_textures.empty() && program.named_textures.size() != program.textures)) {
                            return make_diagnostic(Diag::PaintList, "effect parameters or textures" + at);
                        }
                        for (const auto t : e.textures) {
                            if (t >= list.assets.size() || !list.assets[t] ||
                                list.assets[t]->encoding == Encoding::SignedDistance) {
                                return make_diagnostic(Diag::PaintList, "effect texture" + at);
                            }
                        }
                        const auto finite_parameters =
                            std::all_of(e.parameters.begin(), e.parameters.end(), [](const auto& v) {
                                return std::all_of(v.begin(), v.end(), finite_value);
                            });
                        if (!std::isfinite(e.time) || !finite_parameters ||
                            !std::all_of(e.radii.begin(), e.radii.end(), [](float r) {
                                return std::isfinite(r) && r >= 0;
                            })) {
                            return make_diagnostic(Diag::PaintList, "effect value" + at);
                        }
                        break;
                    }
                    case Item::Kind::Particles: {
                        if (item.payload >= list.particles.size()) {
                            return make_diagnostic(Diag::PaintList, "particles payload out of range" + at);
                        }
                        if (auto valid = check_particles(list, list.particles[item.payload]); !valid) {
                            return make_diagnostic(Diag::PaintList, valid.error().message + at);
                        }
                        break;
                    }
                    case Item::Kind::Backdrop: {
                        if (item.payload >= list.backdrops.size()) {
                            return make_diagnostic(Diag::PaintList, "backdrop payload out of range" + at);
                        }
                        const auto& b = list.backdrops[item.payload];
                        if (!(b.blur >= 0) || b.blur > constants::max_backdrop_blur ||
                            !std::all_of(b.pre.begin(), b.pre.end(), finite_value) ||
                            !std::all_of(b.post.begin(), b.post.end(), finite_value) ||
                            !std::all_of(b.radii.begin(), b.radii.end(), [](float r) {
                                return std::isfinite(r) && r >= 0;
                            })) {
                            return make_diagnostic(Diag::PaintList, "backdrop value" + at);
                        }
                        if (b.under && depth == 0) {
                            return make_diagnostic(Diag::PaintList, "backdrop under no group" + at);
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
    } // namespace

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
        const auto planes = std::uint32_t(into.planes.size());
        const auto effects = std::uint32_t(into.effects.size());
        const auto particles = std::uint32_t(into.particles.size());
        const auto backdrops = std::uint32_t(into.backdrops.size());
        const auto programs = std::uint32_t(into.programs.size());
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
        for (auto plane : from.planes) {
            // The homography maps into from's target px; into's are t of them: rows scale, translations follow.
            auto& h = plane.transform;
            for (unsigned c = 0; c < 3; ++c) {
                h[c] = t[0] * h[c] + t[4] * h[6 + c];
                h[3 + c] = t[3] * h[3 + c] + t[5] * h[6 + c];
            }
            into.planes.push_back(plane);
        }
        for (auto effect : from.effects) {
            // Effects sample assets of the list only, never external textures.
            effect.program += programs;
            for (auto& texture : effect.textures) {
                texture += assets;
            }
            into.effects.push_back(std::move(effect));
        }
        for (auto p : from.particles) {
            // Sprites are assets of the list, never external textures.
            if (p.emitter.sprite) {
                *p.emitter.sprite += assets;
            }
            into.particles.push_back(std::move(p));
        }
        into.backdrops.insert(into.backdrops.end(), from.backdrops.begin(), from.backdrops.end());
        into.assets.insert(into.assets.end(), from.assets.begin(), from.assets.end());
        into.programs.insert(into.programs.end(), from.programs.begin(), from.programs.end());
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
                case Item::Kind::Plane:
                    item.payload += planes;
                    break;
                case Item::Kind::Effect:
                    item.payload += effects;
                    break;
                case Item::Kind::Particles:
                    item.payload += particles;
                    break;
                case Item::Kind::Backdrop:
                    item.payload += backdrops;
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
