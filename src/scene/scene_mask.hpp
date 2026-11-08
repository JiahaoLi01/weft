#pragma once

#include "paint/display_list.hpp"
#include "style/values.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// CSS masks (CSS Masking 1 §4) of a scene element: its mask layers, gradients sized, placed and repeated over their
// boxes, which the element's group (its painting and its descendants', composited) is multiplied by. A scene pass
// evaluates them per pixel where the group composites (scene.hlsl's clipMaskCoverage), from the entries of a mask clip
// (scene_mask_entries).
namespace weft {
    struct SceneMaskLayer {
        enum class Composite : std::uint8_t {
            Add,
            Subtract,
            Intersect,
            Exclude
        };

        // The layer's gradient over its tile (paint::Gradient's tile: size, then its origin in the element's px); none
        // for mask-image none, a transparent layer, and for an image layer.
        std::optional<paint::Gradient> gradient;
        // An image layer (url()): the element's mask image over the tile (width, height, then its origin), its alpha or
        // under mask-mode luminance its luminance (match-source is alpha). An image the scene has none of (it could
        // not read it) is a transparent layer: no tile.
        bool image{};
        bool luminance{};
        std::array<float, 4> tile{};
        // Whether the tile repeats along x and y (mask-repeat repeat, or no-repeat).
        std::array<bool, 2> repeat{true, true};
        // The mask painting area (mask-clip's box: x, y, width, height in the element's px); none for no-clip.
        std::optional<std::array<float, 4>> clip;
        // How the layer composites over the layers below it (mask-composite).
        Composite composite{};
        // Each stop's mask value: its alpha, or under mask-mode luminance its luminance times its alpha (Rec. 709's
        // weights on its sRGB values, as Chrome's luminance masks take them).
        std::vector<float> values;
    };

    struct SceneMask {
        // Top first, as mask-image lists them.
        std::vector<SceneMaskLayer> layers;
        // Hits on the element and its descendants fall in its border box, as Chrome tests them, unless a layer is
        // no-clip; the mask's values do not change hits.
        bool clips_hits{};
    };

    // The text of the element's mask-image (none when it has no mask).
    std::string scene_mask_value(const StyleValues& style);

    // The URL of the element's mask's image layer (url(), as written; empty for an empty or unterminated one): the
    // first, when it has more than the one scene_mask takes. None without one.
    std::optional<std::string> scene_mask_url(const StyleValues& style);

    // The mask of an element of style whose border box is width x height px, or none (mask-image none), image its
    // image layer's image (none: the layer is transparent). Each list of mask-position, -size, -repeat, -origin,
    // -clip, -composite and -mode repeats to the layers' count. An image layer's size: its natural size where auto (one
    // side auto keeps its ratio), contain and cover keep the ratio. std::invalid_argument for what the scene does not
    // take: more than one image layer, an image other than url(), mask-repeat space or round, a margin-box origin or
    // clip, a size or position outside px, percentages and their calc().
    std::optional<SceneMask> scene_mask(const StyleValues& style, float width, float height,
                                        const paint::Asset* image = nullptr);

    // A mask's layers as the floats of a mask clip's entries (RoundedClip::Kind::Mask's data, as pairs), bottom layer
    // last as given; scene_mask_layer_floats floats a layer.
    std::shared_ptr<const std::vector<std::array<float, 2>>> scene_mask_data(const SceneMask& mask);
} // namespace weft
