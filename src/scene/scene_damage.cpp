#include "scene/scene.hpp"
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
        std::map<Key, std::array<double, 4>> old_boxes;
        std::map<Key, std::array<double, 4>> new_boxes;
        for (const auto& e : a.elements) {
            old_boxes.emplace(key(e.identity), e.rect);
        }
        for (const auto& e : b.elements) {
            new_boxes.emplace(key(e.identity), e.rect);
        }
        std::array<double, 4> bounds{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
                                     -std::numeric_limits<double>::infinity(),
                                     -std::numeric_limits<double>::infinity()};
        auto add = [&](auto&& self, const SceneNode& n, const auto& boxes) -> void {
            // Text may overhang its layout box; SVG/projective paint can have
            // nonlocal extents. The independent full path owns those cases.
            const auto* t = n.style.find(PropertyId::Transform);
            const auto* shadow = n.style.find(PropertyId::BoxShadow);
            if ((shadow && !shadow->text().empty() && shadow->text() != "none") || !n.text.empty() ||
                (!n.space.empty() && n.space != constants::xhtml_namespace_uri) ||
                (t && t->text().find("perspective(") != std::string::npos)) {
                full();
                return;
            }
            auto i = boxes.find(key(n.identity));
            if (i == boxes.end()) {
                full();
                return;
            }
            auto r = i->second;
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
            for (const auto& c : n.children) {
                self(self, c, boxes);
            }
        };
        auto visit = [&](auto&& self, const SceneNode& x, const SceneNode& y) -> void {
            if (out.full) {
                return;
            }
            // An effect that reads the time changes every frame.
            bool local = x.identity != y.identity || x.tag != y.tag || x.space != y.space || x.text != y.text ||
                         x.attributes != y.attributes || x.style != y.style || x.box != y.box || x.lines != y.lines ||
                         x.fragments != y.fragments || x.children.size() != y.children.size() ||
                         bool(x.paint) != bool(y.paint) || (x.paint && x.paint->values != y.paint->values) ||
                         !scene_effect_equal(x.effect, y.effect) || (y.effect && y.effect->program->animated);
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
                add(add, x, old_boxes);
                add(add, y, new_boxes);
                return;
            }
            for (std::size_t i = 0; i < x.children.size(); ++i) {
                self(self, x.children[i], y.children[i]);
            }
        };
        visit(visit, a.root, b.root);
        if (out.full || !out.changed) {
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
