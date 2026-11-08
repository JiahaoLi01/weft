#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace weft {
    // A rounded rectangle that clips what is drawn inside it: x, y, width, height with elliptical corners, in the
    // clip's own space. Geometry and coverage are in render/rounded_clip.hpp.
    //
    // A chain of clips may also hold a polygon (CSS clip-path polygon() and path()): an entry of kind Polygon (rect
    // its bounds, to_local its map, evenodd its fill rule), followed by entries of kind Points that carry its points,
    // clip_polygon_points_per_entry of them each, from `first` on; all of them share the polygon's points. The points
    // are closed contours in the clip's space, each ending on its first point, one apart from the next by a point whose
    // x is clip_polygon_break.
    //
    // Or a mask (CSS mask-image's layers, scene_mask): an entry of kind Mask (to_local its map into the element's px,
    // `layers` its layer count, `asset` 1 + the index in its list's assets of its image layer's image, 0 without one)
    // followed by Points entries that carry its layers' floats as pairs, clip_mask_layer_floats a layer. A pass binds
    // the image where an item whose chain holds the mask draws.
    struct RoundedClip {
        enum class Kind : std::uint8_t {
            Rounded,
            Polygon,
            Points,
            Mask
        };

        std::array<float, 4> rect{};
        // TL, TR, BR, BL, normalized.
        std::array<float, 4> radius_x{};
        std::array<float, 4> radius_y{};
        // Target px into the clip's space, a row-vector affine {a, b, c, d, e, f} as paint::Affine:
        // (x, y) -> (x a + y c + e, x b + y d + f). The identity for axis-aligned boxes, whose clips lie in target px;
        // the inverse of a rotated or skewed box's transform otherwise.
        std::array<float, 6> to_local{1, 0, 0, 1, 0, 0};
        Kind kind{};
        bool evenodd{};
        std::uint32_t first{};
        std::uint32_t layers{};
        std::uint32_t asset{};
        std::shared_ptr<const std::vector<std::array<float, 2>>> polygon;
    };

    inline constexpr std::array<float, 6> identity_clip_map{1, 0, 0, 1, 0, 0};
    // The points of a polygon one Points entry carries (the 18 floats of its rectangle, radii and map).
    inline constexpr std::uint32_t clip_polygon_points_per_entry = 9;
    // The x of the point between two contours of a polygon.
    inline constexpr float clip_polygon_break = -1e30f;
    // The points a polygon may hold, its breaks included: each pixel under the clip visits each of its edges.
    inline constexpr std::size_t max_clip_polygon_points = 1024;
    // The floats of one mask layer (scene.hlsl's MaskLayerFloats), and the layers a mask may hold.
    inline constexpr std::uint32_t clip_mask_layer_floats = 36;
    inline constexpr std::uint32_t max_clip_mask_layers = 8;

    // The entries of a polygon clip whose points and bounds lie in its own space, which to_local maps target px into:
    // its Polygon entry, then its Points entries.
    inline std::vector<RoundedClip> clip_polygon_entries(std::shared_ptr<const std::vector<std::array<float, 2>>> points,
                                                         bool evenodd, const std::array<float, 4>& bounds,
                                                         const std::array<float, 6>& to_local) {
        std::vector<RoundedClip> out;
        RoundedClip head;
        head.kind = RoundedClip::Kind::Polygon;
        head.rect = bounds;
        head.to_local = to_local;
        head.evenodd = evenodd;
        head.polygon = points;
        out.push_back(head);
        for (std::uint32_t first = 0; first < points->size(); first += clip_polygon_points_per_entry) {
            RoundedClip part;
            part.kind = RoundedClip::Kind::Points;
            part.first = first;
            part.polygon = points;
            out.push_back(part);
        }
        return out;
    }

    // The entries of a mask clip whose layers' floats data holds (scene_mask_data), its map to_local: its Mask entry,
    // then its Points entries.
    inline std::vector<RoundedClip> clip_mask_entries(std::shared_ptr<const std::vector<std::array<float, 2>>> data,
                                                      std::uint32_t layers, const std::array<float, 6>& to_local,
                                                      std::uint32_t asset = 0) {
        std::vector<RoundedClip> out;
        RoundedClip head;
        head.kind = RoundedClip::Kind::Mask;
        head.to_local = to_local;
        head.layers = layers;
        head.asset = asset;
        head.polygon = data;
        out.push_back(head);
        for (std::uint32_t first = 0; first < data->size(); first += clip_polygon_points_per_entry) {
            RoundedClip part;
            part.kind = RoundedClip::Kind::Points;
            part.first = first;
            part.polygon = data;
            out.push_back(part);
        }
        return out;
    }
} // namespace weft
