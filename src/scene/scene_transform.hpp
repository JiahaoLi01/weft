#pragma once

#include "scene/scene.hpp"
#include "scene/scene_paint.hpp"

#include <array>
#include <optional>
#include <span>

// The scene's one transform model: the transform lists parse_scene_transform reads, resolved in a box. Portable
// geometry and hit testing, the native paint compiler and the D2D reference raster all use it; each keeps its own
// choice between the style text and the compiled paint value.
namespace weft {
    // 2D affine map in row-vector layout, as D2D1::Matrix3x2F: (x, y) -> (x m11 + y m21 + dx, x m12 + y m22 + dy).
    // a * b applies a, then b.
    struct SceneAffine {
        double m11{1};
        double m12{};
        double m21{};
        double m22{1};
        double dx{};
        double dy{};

        static SceneAffine translation(double x, double y) {
            return {1, 0, 0, 1, x, y};
        }

        static SceneAffine scale(double x, double y) {
            return {x, 0, 0, y, 0, 0};
        }

        // Clockwise in y-down coordinates, about (cx, cy).
        static SceneAffine rotation(double degrees, double cx = 0, double cy = 0);

        SceneAffine operator*(const SceneAffine& b) const {
            return {m11 * b.m11 + m12 * b.m21, m11 * b.m12 + m12 * b.m22,      m21 * b.m11 + m22 * b.m21,
                    m21 * b.m12 + m22 * b.m22, dx * b.m11 + dy * b.m21 + b.dx, dx * b.m12 + dy * b.m22 + b.dy};
        }

        std::array<double, 2> point(double x, double y) const {
            return {x * m11 + y * m21 + dx, x * m12 + y * m22 + dy};
        }
    };

    // Projective plane in column-vector layout: m = [a b c; d e f; g h i].
    struct ScenePlane {
        std::array<double, 9> m{1, 0, 0, 0, 1, 0, 0, 0, 1};

        ScenePlane operator*(const ScenePlane& b) const;
        // A point mapped through the plane; far away when it maps to infinity.
        std::array<double, 2> point(double x, double y) const;
        // False for a singular plane.
        bool inverse(ScenePlane& out) const;

        static ScenePlane translation(double x, double y) {
            return {{1, 0, x, 0, 1, y, 0, 0, 1}};
        }

        static ScenePlane affine(const SceneAffine& a) {
            return {{a.m11, a.m21, a.dx, a.m12, a.m22, a.dy, 0, 0, 1}};
        }
    };

    // The functions composed in list order, percentages resolving against a w x h box. Projective functions are
    // a diagnostic (Diag::SceneRaster): they need the plane of scene_perspective.
    SceneAffine scene_affine(std::span<const PaintOperation> operations, float w, float h);

    // perspective(d) rotateY(a), the one projective transform the scene draws, as a plane layer.
    struct ScenePerspective {
        double distance{};
        double radians{};
    };

    // nullopt for a list without projective functions. Any other list with one is a diagnostic.
    std::optional<ScenePerspective> scene_perspective(std::span<const PaintOperation> operations);

    // transform-origin of a w x h box, default 50% 50%.
    std::array<float, 2> scene_transform_origin(const SceneStyle& style, float w, float h);

    // The node's transform property in its box: translate(box + origin) * transform * translate(-origin), in its
    // parent's coordinates.
    ScenePlane scene_plane(const SceneNode& node);

    // The node's transform property without transform-origin. Projective lists are a diagnostic.
    SceneAffine scene_node_affine(const SceneNode& node);
} // namespace weft
