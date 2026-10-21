#include "scene/scene.hpp"
#include "base/css_scalar.hpp"
#include "constants/scene.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace weft {
    SceneDamage scene_damage(const SceneFrame& a, const SceneFrame& b, float scale) {
        if (!std::isfinite(scale) || scale <= 0 || scale > constants::scene::max_raster_scale) {
            throw std::invalid_argument("WEFT_SCENE_DAMAGE_SCALE");
        }
        SceneDamage out;
        const auto w = b.root.box[2];
        const auto h = b.root.box[3];
        auto full = [&] {
            out.changed = out.full = true;
            out.rect = {0, 0, w, h};
        };
        if (a.root.identity != b.root.identity || a.root.box[2] != w || a.root.box[3] != h || a.elements.empty() ||
            b.elements.empty()) {
            full();
            return out;
        }
        using Key = std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>;
        auto key = [](NodeIdentity n) {
            return Key{n.view_epoch, n.slot, n.generation};
        };
        std::map<Key, const ElementGeometry*> old_boxes;
        std::map<Key, const ElementGeometry*> new_boxes;
        for (const auto& e : a.elements) {
            old_boxes.emplace(key(e.identity), &e);
        }
        for (const auto& e : b.elements) {
            new_boxes.emplace(key(e.identity), &e);
        }
        std::array<double, 4> bounds{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
                                     -std::numeric_limits<double>::infinity(),
                                     -std::numeric_limits<double>::infinity()};
        const auto projects = [](const SceneNode& n) {
            const auto* t = n.style.find(PropertyId::Transform);
            return t && t->text().find("perspective(") != std::string::npos;
        };
        // A filter spreads and moves what its element's subtree paints.
        const auto filtered = [](const SceneNode& n) {
            const auto* v = n.style.find(PropertyId::Filter);
            return v && !css::trim(v->text()).empty() && css::trim(v->text()) != "none";
        };
        // projected: an ancestor of n is a perspective plane.
        auto add = [&](auto&& self, const SceneNode& n, const auto& boxes, bool projected) -> void {
            // Text may overhang its layout box; SVG/projective paint and filters can have nonlocal extents. The
            // independent full path owns those cases.
            const auto* shadow = n.style.find(PropertyId::BoxShadow);
            if ((shadow && !shadow->text().empty() && shadow->text() != "none") || !n.text.empty() ||
                (!n.space.empty() && n.space != constants::xhtml_namespace_uri) || projects(n) || filtered(n)) {
                full();
                return;
            }
            auto i = boxes.find(key(n.identity));
            if (i == boxes.end()) {
                full();
                return;
            }
            const auto& g = *i->second;
            auto r = g.rect;
            for (auto v : r) {
                if (!std::isfinite(v)) {
                    full();
                    return;
                }
            }
            bounds[0] = std::min(bounds[0], r[0]);
            bounds[1] = std::min(bounds[1], r[1]);
            bounds[2] = std::max(bounds[2], r[0] + r[2]);
            bounds[3] = std::max(bounds[3], r[1] + r[3]);
            // Particles reach as far as they can fly from the box (scene_particle_reach), mapped to the viewport;
            // inside a perspective plane, anywhere.
            if (!n.particles.empty()) {
                if (projected) {
                    full();
                    return;
                }
                // Geometry without maps (the solver's own element list) places the box at its rect.
                auto m = g.matrix;
                if (m == std::array<double, 6>{1, 0, 0, 1, 0, 0}) {
                    m[4] = r[0];
                    m[5] = r[1];
                }
                for (const auto& emission : n.particles) {
                    const auto reach = scene_particle_reach(emission, n.box[2], n.box[3]);
                    for (const auto& [x, y] : std::array<std::array<double, 2>, 4>{{{reach[0], reach[1]},
                                                                                    {reach[2], reach[1]},
                                                                                    {reach[0], reach[3]},
                                                                                    {reach[2], reach[3]}}}) {
                        const double px = m[0] * x + m[2] * y + m[4];
                        const double py = m[1] * x + m[3] * y + m[5];
                        if (!std::isfinite(px) || !std::isfinite(py)) {
                            full();
                            return;
                        }
                        bounds[0] = std::min(bounds[0], px);
                        bounds[1] = std::min(bounds[1], py);
                        bounds[2] = std::max(bounds[2], px);
                        bounds[3] = std::max(bounds[3], py);
                    }
                }
            }
            for (const auto& c : n.children) {
                self(self, c, boxes, projected);
                if (out.full) {
                    return;
                }
            }
        };
        // A backdrop filter, or an effect that reads its backdrop, shows what lies behind its element, wherever that
        // changes: a frame with one repaints whole when anything changed.
        const auto filters = [](auto&& self, const SceneNode& n) -> bool {
            const auto* v = n.style.find(PropertyId::BackdropFilter);
            if ((v && !css::trim(v->text()).empty() && css::trim(v->text()) != "none") ||
                (n.effect && n.effect->program->backdrop)) {
                return true;
            }
            return std::any_of(n.children.begin(), n.children.end(), [&](const SceneNode& c) {
                return self(self, c);
            });
        };
        auto visit = [&](auto&& self, const SceneNode& x, const SceneNode& y, bool projected_x,
                         bool projected_y) -> void {
            if (out.full) {
                return;
            }
            // An effect that reads the time changes every frame.
            bool local = x.identity != y.identity || x.tag != y.tag || x.space != y.space || x.text != y.text ||
                         x.attributes != y.attributes || x.style != y.style || x.box != y.box || x.lines != y.lines ||
                         x.fragments != y.fragments || x.children.size() != y.children.size() ||
                         bool(x.paint) != bool(y.paint) || (x.paint && x.paint->values != y.paint->values) ||
                         !scene_effect_equal(x.effect, y.effect) || (y.effect && y.effect->program->animated) ||
                         x.background_image != y.background_image || x.particles != y.particles ||
                         !(x.field == y.field || (x.field && y.field && *x.field == *y.field));
            if (!local) {
                for (std::size_t i = 0; i < x.children.size(); ++i) {
                    if (x.children[i].identity != y.children[i].identity) {
                        local = true;
                        break;
                    }
                }
            }
            if (local) {
                out.changed = true;
                add(add, x, old_boxes, projected_x);
                if (!out.full) {
                    add(add, y, new_boxes, projected_y);
                }
                return;
            }
            // A change inside a filtered element.
            if ((filtered(x) || filtered(y)) && !(x.children == y.children)) {
                full();
                return;
            }
            for (std::size_t i = 0; i < x.children.size(); ++i) {
                self(self, x.children[i], y.children[i], projected_x || projects(x), projected_y || projects(y));
            }
        };
        visit(visit, a.root, b.root, false, false);
        if (out.full || !out.changed) {
            return out;
        }
        if (filters(filters, a.root) || filters(filters, b.root)) {
            full();
            return out;
        }
        // Integer physical pixel boundaries, including AA and flattening guards.
        out.rect = {
            float(std::max(0., std::floor((bounds[0] - constants::scene::damage_guard_px) * scale) / scale)),
            float(std::max(0., std::floor((bounds[1] - constants::scene::damage_guard_px) * scale) / scale)),
            float(std::min(double(w), std::ceil((bounds[2] + constants::scene::damage_guard_px) * scale) / scale)),
            float(std::min(double(h), std::ceil((bounds[3] + constants::scene::damage_guard_px) * scale) / scale))};
        if (out.rect[2] <= out.rect[0] || out.rect[3] <= out.rect[1]) {
            out.changed = false;
            return out;
        }
        if ((out.rect[2] - out.rect[0]) * (out.rect[3] - out.rect[1]) >
            w * h * constants::scene::damage_full_repaint_ratio) {
            full();
        }
        return out;
    }
} // namespace weft
