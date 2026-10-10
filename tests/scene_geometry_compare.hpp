#pragma once

#include "scene/scene.hpp"
#include "scene/scene_geometry.hpp"
#include "support/check.hpp"
#include "test_constants/scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

// Differential oracle for the portable SceneGeometry: the D2D reference raster's geometry and hit testing on the same
// frame. Element lists must match one to one; rects and matrices are compared numerically. A hit test may differ only
// at a boundary, where the reference itself answers differently within a quarter pixel.
struct SceneGeometryDifference {
    double max_rect{};
    double max_matrix{};
    std::size_t frames{};
    std::size_t elements{};
    std::size_t hits{};
    std::size_t boundary_hits{};
};

inline void compare_scene_geometry(weft::SceneRaster& reference, weft::SceneGeometry& portable,
                                   const weft::SceneFrame& frame, bool svg_descendants, float width, float height,
                                   SceneGeometryDifference& total,
                                   float step = weft::test_constants::scene::geometry_hit_grid_px, bool hits = true) {
    const auto expected = reference.geometry(frame, svg_descendants);
    const auto actual = portable.geometry(frame, svg_descendants);
    weft::testing::require(expected.size() == actual.size(), "portable geometry element count");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& e = expected[i];
        const auto& a = actual[i];
        weft::testing::require(e.identity == a.identity && e.offset_parent == a.offset_parent,
                               "portable geometry element identity");
        for (std::size_t k = 0; k < 4; ++k) {
            total.max_rect = std::max(total.max_rect, std::abs(e.rect[k] - a.rect[k]));
            total.max_rect = std::max(total.max_rect, std::abs(double(e.offset[k]) - double(a.offset[k])));
        }
        for (std::size_t k = 0; k < 6; ++k) {
            total.max_matrix = std::max(total.max_matrix, std::abs(e.matrix[k] - a.matrix[k]));
        }
    }
    total.elements += expected.size();
    ++total.frames;
    const float slack = weft::test_constants::scene::geometry_hit_boundary_px;
    for (float y = step / 2; hits && y < height; y += step) {
        for (float x = step / 2; x < width; x += step) {
            const auto r = reference.hit_test(frame, x, y);
            const auto p = portable.hit_test(frame, x, y);
            ++total.hits;
            if (r == p) {
                continue;
            }
            bool boundary = false;
            for (const auto& d : std::array<std::array<float, 2>, 8>{{{-slack, 0},
                                                                      {slack, 0},
                                                                      {0, -slack},
                                                                      {0, slack},
                                                                      {-slack, -slack},
                                                                      {slack, slack},
                                                                      {-slack, slack},
                                                                      {slack, -slack}}}) {
                boundary = boundary || reference.hit_test(frame, x + d[0], y + d[1]) == p;
            }
            if (!boundary) {
                // The node and its ancestors, innermost first, with the attributes that shape geometry.
                auto describe = [&](weft::NodeIdentity id) {
                    std::vector<const weft::SceneNode*> path;
                    auto find = [&](auto&& self, const weft::SceneNode& n) -> bool {
                        path.push_back(&n);
                        if (n.identity == id) {
                            return true;
                        }
                        for (const auto& c : n.children) {
                            if (self(self, c)) {
                                return true;
                            }
                        }
                        path.pop_back();
                        return false;
                    };
                    if (!find(find, frame.root)) {
                        return std::string("none");
                    }
                    std::string text;
                    for (auto i = path.rbegin(); i != path.rend(); ++i) {
                        const auto& n = **i;
                        text += "<" + n.tag;
                        for (const auto* k :
                             {"d", "transform", "clip-path", "viewBox", "cx", "cy", "r", "x", "y", "width", "height"}) {
                            if (auto a = n.attributes.find(k); a != n.attributes.end()) {
                                text += std::string(" ") + k + "=\"" + a->second.substr(0, 120) + "\"";
                            }
                        }
                        for (const auto* k : {"transform", "display", "pointer-events", "fill", "overflow"}) {
                            if (auto s = n.style.find(k); s != n.style.end()) {
                                text += std::string(" style:") + k + "=\"" + s->second.substr(0, 60) + "\"";
                            }
                        }
                        text += " box=" + std::to_string(n.box[0]) + "," + std::to_string(n.box[1]) + "," +
                                std::to_string(n.box[2]) + "," + std::to_string(n.box[3]) + "> ";
                    }
                    return text;
                };
                throw std::runtime_error("portable hit test differs from the reference away from a boundary at (" +
                                         std::to_string(x) + ", " + std::to_string(y) + "): reference " + describe(r) +
                                         ", portable " + describe(p));
            }
            ++total.boundary_hits;
        }
    }
}
