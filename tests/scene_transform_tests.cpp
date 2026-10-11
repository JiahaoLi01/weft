#include "scene/scene_transform.hpp"
#include "support/check.hpp"
#include "test_constants/scene.hpp"
#include <cmath>
#include <iostream>
#include <string>

// The scene's one transform model (parse_scene_transform, scene_affine, scene_perspective, scene_plane), which
// geometry, hit testing, native paint and the D2D reference raster share.
namespace {
    using weft::testing::require;
    namespace limits = weft::test_constants::scene;

    bool near(double a, double b, double tolerance = limits::transform_tolerance) {
        return std::abs(a - b) <= tolerance;
    }

    bool same(const weft::SceneAffine& a, const weft::SceneAffine& b, double tolerance = limits::transform_tolerance) {
        return near(a.m11, b.m11, tolerance) && near(a.m12, b.m12, tolerance) && near(a.m21, b.m21, tolerance) &&
               near(a.m22, b.m22, tolerance) && near(a.dx, b.dx, tolerance) && near(a.dy, b.dy, tolerance);
    }

    weft::SceneAffine affine(std::string_view text, bool svg = false, float w = 0, float h = 0) {
        return weft::scene_affine(weft::parse_scene_transform(text, svg).value(), w, h);
    }

    bool refused(std::string_view text, bool svg = false) {
        const auto parsed = weft::parse_scene_transform(text, svg);
        return !parsed && parsed.error().code == weft::Diag::SceneRaster;
    }

    template <class F>
    bool diagnostic(F&& f) {
        try {
            f();
        } catch (const weft::DiagnosticError& e) {
            return e.code() == weft::Diag::SceneRaster;
        }
        return false;
    }
} // namespace

int main() {
    try {
        using A = weft::SceneAffine;
        // Lists compose in order: the last function applies first.
        require(same(affine("translate(10px, 20px) scale(2)"), A::scale(2, 2) * A::translation(10, 20)), "list order");
        // Translate percentages resolve against the box; scale percentages are fractions.
        require(same(affine("translate(-50%, 25%)", false, 200, 80), A::translation(-100, 20)), "translate %");
        require(same(affine("scale(50%)"), A::scale(.5, .5)), "scale %");
        // A CSS angle needs deg; a unitless one has been radians. In SVG a unitless angle is degrees, and rotate takes
        // a center (the D3 radial labels of the gallery depend on both).
        require(same(affine("rotate(90deg)"), A::rotation(90)), "css degrees");
        require(same(affine("rotate(1.5707963267948966)"), A::rotation(90), limits::unitless_angle_tolerance),
                "css unitless radians");
        require(same(affine("rotate(90)", true), A::rotation(90)), "svg unitless degrees");
        require(same(affine("rotate(90 10 20)", true), A::rotation(90, 10, 20)), "svg rotation center");
        require(refused("rotate(90deg, 10px, 20px)"), "css rotation center accepted");
        // SVG lists may separate functions with commas; units are optional.
        require(same(affine("translate(5,6), scale(2)", true), A::scale(2, 2) * A::translation(5, 6)),
                "svg separators");
        require(same(affine("matrix(1, 2, 3, 4, 5, 6)"), A{1, 2, 3, 4, 5, 6}), "matrix");
        require(affine("none").m11 == 1 && affine("").dx == 0, "identity");
        // Outside the grammar: Diag::SceneRaster.
        for (const auto* text :
             {"skewX(10deg)", "translate(1px", "matrix(1, 2, 3)", "scale()", "rotate(1rad)", "perspective(10)"}) {
            require(refused(text), std::string("accepted ") + text);
        }
        // The one projective form, perspective(d) rotateY(a), is a plane; affine resolution refuses it.
        const auto plane = weft::parse_scene_transform("perspective(400px)rotateY(30deg)").value();
        const auto p = weft::scene_perspective(plane);
        require(p && p->distance == 400 && near(p->radians, std::acos(-1.) / 6), "perspective plane");
        require(!weft::scene_perspective(weft::parse_scene_transform("rotate(30deg)").value()), "affine list");
        require(diagnostic([&] {
                    (void)weft::scene_affine(plane, 0, 0);
                }),
                "affine resolution accepted a projective list");
        require(diagnostic([] {
                    (void)weft::scene_perspective(
                        weft::parse_scene_transform("rotateY(30deg) perspective(400px)").value());
                }),
                "projective list outside the plane subset accepted");
        // A node's plane: placed at its box, about its transform-origin.
        weft::SceneNode node;
        node.box = {10, 20, 100, 50};
        node.style["transform"] = "rotate(90deg)";
        node.style["transform-origin"] = "0 0";
        const auto corner = weft::scene_plane(node).point(100, 0);
        require(near(corner[0], 10) && near(corner[1], 120), "node plane");
        node.style["transform-origin"] = "50% 50%";
        const auto centre = weft::scene_plane(node).point(50, 25);
        require(near(centre[0], 60) && near(centre[1], 45), "transform-origin");
        std::cout << "PASS one scene transform model: order, percentages, CSS and SVG angles, rotation center, "
                     "separators, diagnostics and the perspective plane\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
