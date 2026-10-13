#pragma once

#include "base/result.hpp"
#include "paint/rounded_clip.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
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

    // Values equal the gradient codes of the shared shaders (src/render/shaders/scene.hlsl).
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
    // to stroke source texels outside it. An asset index past DisplayList::assets names an external texture.
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
        // Which of its source node's images this is: a line of its text. Images of one node are cached apart.
        std::uint32_t part{};
    };

    // An outer box-shadow of the item's box [0, width] x [0, height]: the box grown by spread on each side (its radii
    // too), offset by (x, y), blurred by a Gaussian of standard deviation blur / 2, and shown only outside the box.
    // Lengths in item px.
    struct Shadow {
        Color color{};
        float x{};
        float y{};
        float blur{};
        float spread{};
        // The box's corner radii as specified, top-left first and clockwise; each corner stays within half the shorter
        // side.
        std::array<float, 4> radii{};
    };

    // A path in item px: subpaths of lines and cubic Bezier curves. Each verb takes its points from points in order:
    // Move and Line one, Cubic three (two controls, then the end), Close none.
    struct Path {
        enum class Verb : std::uint8_t {
            Move,
            Line,
            Cubic,
            Close
        };

        std::vector<Verb> verbs;
        std::vector<std::array<float, 2>> points;
        // Fill rule: even-odd, otherwise non-zero.
        bool evenodd{};
    };

    // How a path is stroked (SVG 2, 13.5): a line of width centered on the path; caps end open subpaths and joins meet
    // segments, a miter join past miter_limit times the width becomes a bevel. A non-empty dash pattern alternates
    // dash and gap lengths from dash_offset into it, restarting on each subpath. Lengths in item px.
    struct Stroke {
        enum class Cap : std::uint8_t {
            Butt,
            Round,
            Square
        };

        enum class Join : std::uint8_t {
            Miter,
            Round,
            Bevel
        };

        float width{1};
        Cap cap{};
        Join join{};
        float miter_limit{4};
        std::vector<float> dashes;
        float dash_offset{};
    };

    // A path filled, or stroked when stroke is set, with one color.
    struct PathPaint {
        std::shared_ptr<const Path> path;
        Color color{};
        std::optional<Stroke> stroke;
    };

    struct DisplayList;

    // A plane in perspective: a nested list drawn flat over [0, width] x [0, height] of its own px, then mapped into
    // the target by a homography in column-vector layout [a b c; d e f; g h i]: (x, y) -> ((a x + b y + c) / w,
    // (d x + e y + f) / w) with w = g x + h y + i, above constants::plane_min_depth at the plane's corners (in front
    // of the viewer). Renderers draw the content with a transparent margin, so the plane's edges antialias like an
    // image's.
    struct Plane {
        std::shared_ptr<const DisplayList> content;
        std::array<float, 9> transform{1, 0, 0, 0, 1, 0, 0, 0, 1};
        float width{};
        float height{};
    };

    // A parameter of an effect program that CSS sets: the custom property --name on the element or its ancestors, a
    // value of type, or initial when no property is set.
    struct EffectParameter {
        enum class Type : std::uint8_t {
            // One number, in x.
            Number,
            // Two to four numbers separated by spaces, in x, y, z and w.
            Vector2,
            Vector3,
            Vector4,
            // A CSS color, as straight linear RGB and alpha.
            Color
        };

        std::string name;
        Type type{};
        // CSS text.
        std::string initial;
    };

    // A compiled UI shader effect (docs/architecture.md, "UI 着色器效果、粒子与背景拷贝"): its HLSL pixel function as
    // each backend runs it in each composition space, and what the function reads. Immutable; lists lease it as they
    // lease assets.
    struct EffectProgram {
        std::string name;
        // Parameters (float4 each) and textures the function reads: at most constants::max_effect_parameters and
        // constants::max_effect_textures.
        std::uint32_t parameters{};
        std::uint32_t textures{};
        // Its manifest, for CSS: each parameter by name and type, in the order the function reads them, and the
        // textures by name, all of them or none (a program that only display lists draw); and whether the function
        // reads the time, so that frames keep coming while it shows.
        std::vector<EffectParameter> named_parameters;
        std::vector<std::string> named_textures;
        bool animated{};
        // Pixel shader bytecode, empty where not built: D3D11 DXBC and Vulkan SPIR-V, for an encoded and a linear
        // pass.
        std::vector<std::uint8_t> d3d11_encoded;
        std::vector<std::uint8_t> d3d11_linear;
        std::vector<std::uint8_t> spirv_encoded;
        std::vector<std::uint8_t> spirv_linear;
    };

    // A UI shader effect over the item's box [0, width] x [0, height], inside its corner radii (top-left first, then
    // clockwise, as a box's): the program's function at time (seconds) with parameters (absent ones zero), sampling
    // textures, assets of the list read as data (coverage) or as linear premultiplied color (sRGB images). The
    // function returns linear premultiplied color.
    struct Effect {
        // In DisplayList::programs.
        std::uint32_t program{};
        float time{};
        std::vector<std::array<float, 4>> parameters;
        std::vector<std::uint32_t> textures;
        std::array<float, 4> radii{};
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
            EndGroup,
            // A Shadow, which a renderer prepares as an image.
            Shadow,
            // A PathPaint, which a renderer rasterizes as coverage at device scale.
            Path,
            // A Plane, whose content a renderer draws into an image and projects.
            Plane,
            // An Effect, a program's pixel function over the item's box.
            Effect
        };

        Kind kind{};
        // Item space to target px.
        Affine transform = identity;
        ClipRect clip{};
        float width{};
        float height{};
        float opacity{1};
        // Index in DisplayList::boxes (Box), images (Image), rasters (Text, Layer), shadows (Shadow), paths (Path),
        // planes (Plane) or effects (Effect); unused by groups.
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
        std::vector<Shadow> shadows;
        std::vector<PathPaint> paths;
        std::vector<Plane> planes;
        std::vector<Effect> effects;
        // Leases of the assets images and effects draw.
        std::vector<std::shared_ptr<const Asset>> assets;
        // Leases of the programs effects run.
        std::vector<std::shared_ptr<const EffectProgram>> programs;
        // Host textures images draw after the assets: image asset assets.size() + i is external texture i, linear
        // premultiplied and drawn with its own colors. The list only counts them; a renderer binds them per frame.
        std::uint32_t external_textures{};
        std::vector<RoundedClip> clips;
    };

    // Whether a list is well formed: payloads in range and of the item's kind, images of a leased asset or an external
    // texture without a stroke, rasters with a source, shadows with finite values and no negative blur, paths with a
    // path of whole verbs and finite points and a stroke of finite, non-negative lengths, planes of a well-formed
    // content list (nested at most constants::max_plane_depth deep), a positive size and a finite, invertible
    // homography in front of the viewer, effects of a leased program with at most its parameters and exactly its
    // textures (coverage or sRGB assets of the list), finite values and non-negative radii, rounded clip ranges inside
    // the clips, groups balanced and nested at most max_group_depth deep. Diag::PaintList otherwise.
    Status check(const DisplayList& list, std::size_t max_group_depth);

    // The deepest group nesting of a well-formed list.
    std::size_t group_depth(const DisplayList& list);

    // Appends from's items after into's under transform, which maps from's target px into into's and must scale by
    // positive factors and translate (Diag::PaintList otherwise). Item transforms compose with it, clip rects and
    // rounded clips map through it, and payloads, assets, programs, rounded clips and external textures are
    // renumbered: from's assets follow into's, and every external texture follows every asset.
    Status append(DisplayList& into, const DisplayList& from, const Affine& transform = identity);
} // namespace weft::paint
