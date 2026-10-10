#pragma once

#include "scene/scene.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Portable scene geometry: CSS/SVG transform lists, SVG path data, element boxes and hit testing. The D2D reference
// raster replays the same parsed transform operations and path commands with its own D2D math.
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

    // One function of a transform list with resolved lengths. Rotate holds degrees and the SVG rotation center.
    struct SceneTransformOp {
        enum class Kind {
            Translate,
            Scale,
            Rotate,
            Matrix
        };

        Kind kind{};
        std::array<float, 6> args{};
    };

    // A CSS (svg = false) or SVG transform attribute list; percentages resolve against w and h. Projective
    // functions and unknown functions are diagnostics (Diag::SceneRaster).
    std::vector<SceneTransformOp> scene_transform_ops(std::string_view list, float w, float h, bool svg);
    SceneAffine scene_transform(const std::vector<SceneTransformOp>& ops);
    // transform-origin of a w x h box, default 50% 50%.
    std::array<float, 2> scene_transform_origin(const SceneStyle& style, float w, float h);

    // SVG viewBox/preserveAspectRatio of an svg element as scale and translation.
    struct SceneViewport {
        float sx{1};
        float sy{1};
        float tx{};
        float ty{};
    };

    SceneViewport scene_svg_viewport(const SceneNode& svg);

    // SVG path data in absolute coordinates. Relative, smooth (S/T) and H/V commands are resolved; a drawing
    // command after Z or at the start begins a figure with an explicit Move. Arc keeps the endpoint form
    // {rx, ry, x-axis-rotation, large-arc, sweep, x, y}; Cubic {x1, y1, x2, y2, x, y}; Quadratic {x1, y1, x, y}.
    struct ScenePathCommand {
        enum class Kind {
            Move,
            Line,
            Cubic,
            Quadratic,
            Arc,
            Close
        };

        Kind kind{};
        std::array<float, 7> v{};
    };

    std::vector<ScenePathCommand> parse_scene_path(std::string_view data);
    // Whitespace/comma separated numbers (SVG points, stroke-dasharray).
    std::vector<float> parse_scene_numbers(std::string_view list);

    // Element boxes (getBoundingClientRect-style geometry) and hit testing of a laid-out scene, without any
    // platform graphics API. Owner thread only; text boxes come from SceneText.
    class SceneGeometry {
    public:
        explicit SceneGeometry(SceneText& text);
        ~SceneGeometry();
        SceneGeometry(const SceneGeometry&) = delete;
        SceneGeometry& operator=(const SceneGeometry&) = delete;

        std::vector<ElementGeometry> geometry(const SceneFrame& frame, bool svg_descendants = false);
        NodeIdentity hit_test(const SceneFrame& frame, float x, float y);
        // The node's CSS transform as {m11, m12, m21, m22, dx, dy}, without transform-origin.
        std::array<double, 6> transform_matrix(const SceneNode& node) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace weft
