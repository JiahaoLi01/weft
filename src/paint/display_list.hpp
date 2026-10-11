#pragma once

#include "base/result.hpp"
#include "paint/rounded_clip.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// The portable paint IR (docs/architecture.md, "FramePacket 与 GPU 生命周期"): what one frame draws, in paint order,
// as owned values in CSS px of the target. No device object, atlas coordinate, DOM node or VM value; a renderer
// prepares it for its device.
namespace weft {
    struct SceneNode;
} // namespace weft

namespace weft::paint {
    // Straight sRGB color.
    using Color = std::array<float, 4>;
    // Row-vector affine map {a, b, c, d, e, f}: (x, y) -> (x a + y c + e, x b + y d + f), as SceneAffine.
    using Affine = std::array<float, 6>;
    inline constexpr Affine identity{1, 0, 0, 1, 0, 0};
    // Axis-aligned clip in target px: left, top, right, bottom.
    using ClipRect = std::array<float, 4>;

    // Texel encodings of an asset. The values equal weft_render_encoding.
    enum class Encoding {
        SrgbStraight,
        Coverage,
        SignedDistance
    };

    // Immutable pixels: straight sRGB RGBA8, an 8-bit coverage mask, or 8-bit signed distances.
    struct Asset {
        std::uint32_t width{};
        std::uint32_t height{};
        Encoding encoding{};
        std::vector<std::uint8_t> bytes;
        // Distance range in source texels of a SignedDistance asset.
        float spread{};
    };

    // Values equal the gradient codes of the shared shaders (src/render/backends/d3d11/scene.hlsl).
    enum class GradientKind : std::uint8_t {
        None,
        Linear,
        Radial,
        Conic
    };

    struct GradientStop {
        float offset{};
        Color color{};
    };

    struct Gradient {
        GradientKind kind{};
        float angle{}; // Degrees.
        // The repeated tile in box space: width, height, then the offset of its origin.
        std::array<float, 4> tile{};
        std::vector<GradientStop> stops;
    };

    // A CSS box in its item space [0, width] x [0, height]: background, gradient and a solid border with circular
    // corner radii.
    struct Box {
        std::array<float, 4> radii{};  // Top-left, top-right, bottom-right, bottom-left.
        std::array<float, 4> widths{}; // Border top, right, bottom, left.
        std::array<Color, 4> edges{};  // Border colors, same order.
        Color background{};
        Gradient gradient;
    };

    // An asset drawn over the item, sampled over uv (normalized origin and extent): an image keeps its own colors, a
    // coverage mask paints color where it covers, and signed distances paint color inside the contour and outline up
    // to stroke source texels outside it.
    struct Image {
        std::uint32_t asset{};
        std::array<float, 4> uv{0, 0, 1, 1};
        Color color{1, 1, 1, 1};
        float stroke{};
        Color outline{};
    };

    // Content the renderer rasterizes at device scale and draws as an image over the item. Transitional: scene text
    // and explicit layers keep their scene source until glyph runs and nested lists replace them (step 10.3 of
    // docs/refactoring-plan.md).
    struct Raster {
        std::shared_ptr<const SceneNode> source;
        // A coverage image tinted with tint; otherwise the image's own colors.
        bool mask{};
        Color tint{};
    };

    struct Item {
        enum class Kind : std::uint8_t {
            Box,
            // A Raster of scene text.
            Text,
            // A Raster of an explicit layer.
            Layer,
            // An Image of a leased asset.
            Image,
            // Items up to the matching EndGroup composite as one isolated group with the EndGroup's opacity.
            BeginGroup,
            EndGroup
        };

        Kind kind{};
        // Item space to target px.
        Affine transform = identity;
        ClipRect clip{};
        float width{};
        float height{};
        float opacity{1};
        // Index in DisplayList::boxes (Box), images (Image) or rasters (Text, Layer); unused by groups.
        std::uint32_t payload{};
        // The item's rounded clips, outermost first: DisplayList::clips[clips_begin, clips_begin + clips_count).
        std::uint32_t clips_begin{};
        std::uint32_t clips_count{};
    };

    struct DisplayList {
        std::vector<Item> items;
        std::vector<Box> boxes;
        std::vector<Image> images;
        std::vector<Raster> rasters;
        // Leases of the assets images draw.
        std::vector<std::shared_ptr<const Asset>> assets;
        std::vector<RoundedClip> clips;
    };

    // Whether a list is well formed: payloads in range and of the item's kind, images of a leased asset, rasters with
    // a source, rounded clip ranges inside the clips, groups balanced and nested at most max_group_depth deep.
    // Diag::PaintList otherwise.
    Status check(const DisplayList& list, std::size_t max_group_depth);

    // The deepest group nesting of a well-formed list.
    std::size_t group_depth(const DisplayList& list);
} // namespace weft::paint
