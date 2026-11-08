#pragma once

#include "paint/display_list.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

// SVG geometry as portable paths: the shapes of SVG elements in their user units, arcs and quadratic curves turned
// into cubic curves, for renderers that rasterize paths. Hit testing flattens the same arcs (scene_geometry).
namespace weft {
    // An elliptical arc in center form (SVG implementation notes F.6.5): center, radii, x-axis rotation, and the start
    // angle and signed sweep of its parameter, in radians.
    struct SceneArc {
        double cx{};
        double cy{};
        double rx{};
        double ry{};
        double phi{};
        double start{};
        double sweep{};

        std::array<double, 2> at(double t) const;
    };

    // The arc from p0 to p1 on an ellipse of radii (rx, ry) rotated by degrees, chosen by the large-arc and sweep
    // flags. None when the endpoints meet or a radius is zero (the arc is then a line); radii too small to span the
    // endpoints grow until they do.
    std::optional<SceneArc> scene_center_arc(std::array<double, 2> p0, double rx, double ry, double degrees, bool large,
                                             bool sweep, std::array<double, 2> p1);

    // An SVG rect, circle, ellipse or line by its geometry attributes in its user units. A rect: (x, y) and the size
    // (w, h), with its corner radii (rx, ry) as the path takes them (either one standing for a missing other, each at
    // most half its side; 0 for square corners). A circle or an ellipse: its center (x, y) and radii (rx, ry). A line:
    // from (x, y) to (w, h).
    struct SceneSvgShape {
        enum class Kind : std::uint8_t {
            Rect,
            Ellipse,
            Line
        };

        Kind kind{};
        double x{};
        double y{};
        double w{};
        double h{};
        double rx{};
        double ry{};
    };

    // The shape of an SVG rect, circle, ellipse or line; null for any other element and for a rectangle or an ellipse
    // without area.
    std::optional<SceneSvgShape> scene_svg_shape(const SceneNode& element);

    // The path of an SVG shape element (rect, circle, ellipse, path, polygon, polyline, line) in its user units, with
    // its fill rule; null for any other element and for a shape that renders nothing (no path data, a rectangle or
    // circle without area).
    std::shared_ptr<const paint::Path> scene_svg_path(const SceneNode& element);

    // SVG path data (a path's d, CSS path()) as a path with that fill rule; null for data of no command.
    std::shared_ptr<const paint::Path> scene_path_data(std::string_view data, bool evenodd);

    // An SVG element's stroke style: stroke-width, -linecap, -linejoin, -dasharray and -dashoffset, and SVG's
    // initial miter limit (4). An unknown cap or join, a negative dash or more than max_stroke_dashes fails; an odd
    // dash list repeats and an all-zero one draws solid.
    paint::Stroke scene_svg_stroke(const SceneStyle& style);
} // namespace weft
