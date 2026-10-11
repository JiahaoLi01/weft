#pragma once

#include "base/function_ref.hpp"
#include "base/plane.hpp"
#include "render/rounded_clip.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

// The one hit-testing path of documents and scenes. A formatter lists the areas it painted, in paint order; a point
// hits the topmost area whose shapes contain it, inside every clip of the area's chain. Pure values: no DOM, script or
// GPU references.
namespace weft {
    // Flattened outlines in an area's coordinates: closed polygons for fill, polylines for stroke.
    struct HitOutline {
        struct Figure {
            std::vector<std::array<double, 2>> points;
            bool closed{};
        };

        std::vector<Figure> figures;
        bool evenodd{};
    };

    // An outline's stroke as SVG paints it: width, caps at open and dash ends, round joins, dashes from the offset.
    struct HitStroke {
        enum class Cap : std::uint8_t {
            Butt,
            Round,
            Square
        };

        double width{1};
        Cap cap{};
        std::vector<double> dashes;
        double offset{};
    };

    struct HitShape {
        enum class Kind : std::uint8_t {
            // [x, x + w) x [y, y + h); empty unless w and h are positive.
            Box,
            // [x, x + w] x [y, y + h]: SVG text bounds.
            Bounds,
            // A rounded rect, as contains_rounded tests it.
            Rounded,
            // Inside the outline under its fill rule.
            Fill,
            // Under the outline's stroke.
            Stroke
        };

        Kind kind{};
        // x, y, w, h of Box and Bounds.
        std::array<double, 4> rect{};
        RoundedClip rounded;
        std::shared_ptr<const HitOutline> outline;
        HitStroke stroke;

        static HitShape box(double x, double y, double w, double h) {
            return {Kind::Box, {x, y, w, h}};
        }

        static HitShape bounds(double x, double y, double w, double h) {
            return {Kind::Bounds, {x, y, w, h}};
        }
    };

    struct HitAreas {
        static constexpr std::size_t none = std::numeric_limits<std::size_t>::max();

        // A shape every hit below it must fall in (an overflow box, an svg viewport, a clip-path, a rounded clip),
        // in the coordinates of its own inverse plane. Clips form chains through their parents.
        struct Clip {
            geometry::Plane inverse;
            HitShape shape;
            std::size_t parent{none};
        };

        // One painted area: the point mapped into its coordinates must fall in one of its shapes.
        struct Area {
            // The caller's index of what was hit.
            std::size_t target{};
            geometry::Plane inverse;
            std::vector<HitShape> shapes;
            // Innermost clip of the area's chain.
            std::size_t clip{none};
        };

        std::vector<Area> areas;
        std::vector<Clip> clips;

        // Appends a clip below `parent` and returns its index.
        std::size_t add_clip(const geometry::Plane& inverse, HitShape shape, std::size_t parent) {
            clips.push_back({inverse, std::move(shape), parent});
            return clips.size() - 1;
        }
    };

    bool hit_contains(const HitShape& shape, double x, double y);

    // The target of the topmost area containing (x, y) whose target `accept` takes; nullopt for none.
    std::optional<std::size_t> hit_areas(const HitAreas& areas, double x, double y,
                                         FunctionRef<bool(std::size_t target)> accept);
    std::optional<std::size_t> hit_areas(const HitAreas& areas, double x, double y);
} // namespace weft
