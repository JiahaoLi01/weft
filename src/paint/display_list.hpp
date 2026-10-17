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
    // A 4 x 5 color matrix on straight RGBA, as CSS filter functions take colors (Filter Effects 1): row-major rows of r,
    // g, b and a, each with its offset in the fifth column; results clamp to [0, 1].
    using ColorMatrix = std::array<float, 20>;
    inline constexpr ColorMatrix identity_color_matrix{1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0};

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
        // A radial gradient's ending shape in its tile: its center, then its horizontal and vertical radii (equal for a
        // circle), both above zero. Its gradient line runs from the center (0) to the shape (1).
        std::array<float, 4> shape{};
        // The stops repeat along the gradient line, every span from the first stop's offset to the last's (above
        // zero): repeating-linear-gradient() and repeating-radial-gradient().
        bool repeating{};
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

    // Where a host texture lies over its item, as CSS lays out a background image of the texture's natural size, which
    // only the renderer knows (CSS Backgrounds 3): the item is the painting area (background-clip's box) and area the
    // positioning area (background-origin's box), x, y, width, height in item px. background-size scales the tile:
    // lengths, or the texture covering the area or contained in it; background-position places the first tile;
    // background-repeat repeats it on each axis (space spreads whole tiles over the area, round scales them to fit a
    // whole number). scene_texture_tiles (scene/scene_background.hpp) and the scene shader lay it out alike.
    struct TextureFit {
        enum class Scale : std::uint8_t {
            Lengths,
            Cover,
            Contain
        };
        enum class Repeat : std::uint8_t {
            Repeat,
            NoRepeat,
            Space,
            Round
        };
        std::array<float, 4> area{};
        Scale scale{};
        // The tile's size on each axis with Lengths, px; below 0 for auto: the natural size, or the other axis's at
        // the texture's ratio.
        std::array<float, 2> size{-1, -1};
        // background-position on each axis: the share of the room the tile leaves in the area (0 at its start, 1 at
        // its end), and a length added to it, px.
        std::array<float, 2> fraction{};
        std::array<float, 2> offset{};
        std::array<Repeat, 2> repeat{};

        friend bool operator==(const TextureFit&, const TextureFit&) = default;
    };

    // An asset drawn over the item, sampled over uv (normalized origin and extent): an image keeps its own colors, a
    // coverage mask paints color where it covers, and signed distances paint color inside the contour and outline up
    // to stroke source texels outside it. An asset index past DisplayList::assets names an external texture. A
    // texture (one past an index of DisplayList::textures) names a host texture instead, which a scene pass binds by
    // name for each frame, lays out over the item as fit says and draws with its own colors; its asset is then 0. A
    // fitted image (a scene's url() background) is an sRGB asset of the list a scene pass lays out as fit says, as it
    // lays out a host texture, the asset's size its natural size.
    struct Image {
        std::uint32_t asset{};
        std::array<float, 4> uv{0, 0, 1, 1};
        Color color{1, 1, 1, 1};
        float stroke{};
        Color outline{};
        std::uint32_t texture{};
        TextureFit fit;
        bool fitted{};
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
        // Room the image keeps beyond its usual transparent margin, px on each side, for ink that leaves the box:
        // the glyphs of slanted text lean out of it, and a blur spreads it.
        float overhang{};
        // A text shadow (text-shadow): the mask is the text's coverage (color glyphs' too) blurred by a Gaussian of
        // this standard deviation, in item px; 0 for none.
        float blur{};
    };

    // A box-shadow of the item's box [0, width] x [0, height]. An outer one: the box grown by spread on each side (its
    // radii too), offset by (x, y), blurred by a Gaussian of standard deviation blur / 2, and shown only outside the
    // box. An inset one, of an element's padding box: all that lies outside the box shrunk by spread on each side (its
    // radii too, not below zero), offset by (x, y), blurred alike, and shown only inside the box. Lengths in item px.
    struct Shadow {
        Color color{};
        float x{};
        float y{};
        float blur{};
        float spread{};
        // The box's corner radii, top-left first and clockwise: an outer shadow's as specified, an inset shadow's the
        // padding box's inner radii; each corner stays within half the shorter side.
        std::array<float, 4> radii{};
        bool inset{};
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

    // A drop shadow (filter: drop-shadow()) of the shapes an svg element paints: the coverage of the list's items
    // [first, first + count) (Path items, and Box items of one background and one border color and width without a
    // gradient: the svg shapes the box shader draws; each at its colors' alpha and its item's opacity, each over the
    // ones before), inside content (the element's viewport, in item px, which clips them), offset by (x, y), blurred by
    // a Gaussian of standard deviation blur and tinted with color. Its item lies before those paths, in the element's
    // space shifted so that its box [0, width] x [0, height] holds the whole shadow; x, y and blur are in item px.
    struct PathShadow {
        std::uint32_t first{};
        std::uint32_t count{};
        Color color{};
        float x{};
        float y{};
        float blur{};
        std::array<float, 4> content{};
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

    // How the particles of an emitter start, move and look (docs/architecture.md, "粒子"): each particle a disc or a
    // sprite frame centred on its position, its state a function of its emission's elapsed time alone (stateless, no
    // simulation). Lengths in item px, angles in degrees (0 along +x, 90 along +y), times in seconds; each particle
    // draws its lifetime, speed, direction, size factor, angle and spin from the ranges, uniformly.
    struct ParticleEmitter {
        enum class Shape : std::uint8_t {
            // At x, y of area.
            Point,
            // Anywhere in the rectangle x, y, width, height of area.
            Rect,
            // Anywhere in the disc of centre x, y and radius z of area.
            Circle,
            // On the outline of the rectangle of area.
            Edge
        };

        Shape shape{};
        std::array<float, 4> area{};
        // Particles per second of a stream.
        float rate{};
        std::array<float, 2> life{1, 1};
        std::array<float, 2> speed{};
        // The direction of travel, spread over the full width of spread around it.
        float direction{};
        float spread{};
        // Acceleration (px/s²), and linear drag (per second) towards the drift gravity reaches (gravity / drag).
        std::array<float, 2> gravity{};
        float drag{};
        // Initial angle (degrees) and spin (degrees per second).
        std::array<float, 2> angle{};
        std::array<float, 2> spin{};
        // Size across (px) at life 0, 1/3, 2/3 and 1, linear between, times the particle's factor from scale.
        std::array<float, 4> size{8, 8, 8, 8};
        std::array<float, 2> scale{1, 1};
        // Straight linear RGBA at life 0, 1/3, 2/3 and 1, interpolated in linear light.
        std::array<Color, 4> color{Color{1, 1, 1, 1}, Color{1, 1, 1, 1}, Color{1, 1, 1, 1}, Color{1, 1, 1, 1}};
        // A disc's soft edge, as a fraction of its radius from the rim inward (0: a hard edge, antialiased).
        float softness{};
        // A sprite instead of a disc: an asset of the list (coverage, or sRGB color) in a grid of columns x rows
        // frames read left to right, then down, tinted by the color. Frames advance at frame_rate per second of a
        // particle's life, looping; at frame_rate 0 each particle shows one frame drawn at random.
        std::optional<std::uint32_t> sprite;
        std::array<std::uint32_t, 2> frames{1, 1};
        float frame_rate{};

        friend bool operator==(const ParticleEmitter&, const ParticleEmitter&) = default;
    };

    // One emission of an emitter: a stream that emits the emitter's rate of particles per second from its start (until
    // stopped, seconds after its start, when set), or a burst of count particles emitted over duration seconds from
    // its start, at random times. elapsed is the time since its start at the list's frame. The seed draws each
    // particle's values. With a target (item px), particles fly from where they start towards their initial velocity,
    // then curve to arrive at it at the end of their lives, without gravity or drag.
    struct ParticleEmission {
        enum class Kind : std::uint8_t {
            Stream,
            Burst
        };

        Kind kind{};
        double elapsed{};
        std::optional<double> stopped;
        std::uint32_t count{};
        float duration{};
        std::uint32_t seed{};
        std::optional<std::array<float, 2>> target;

        friend bool operator==(const ParticleEmission&, const ParticleEmission&) = default;
    };

    // A backdrop filter (CSS backdrop-filter) over the item's box [0, width] x [0, height] with circular corner radii:
    // what the target holds where the box covers it (its bounds in device px, within the target), through pre, blurred
    // by a Gaussian of standard deviation blur (item px) whose taps past those bounds mirror back into them, through
    // post, then drawn in the box's shape times the item's opacity. With under it reads the target that encloses the
    // item's group and draws beneath what the group holds, before the group composites: an element whose group isolates
    // its opacity or its descendants' backdrops; otherwise it reads and draws over the target it draws into.
    struct Backdrop {
        float blur{};
        ColorMatrix pre = identity_color_matrix;
        ColorMatrix post = identity_color_matrix;
        std::array<float, 4> radii{};
        bool under{};
    };

    // The particles of one emission, in its item's space: they move with the item and draw in its place in paint order.
    struct Particles {
        ParticleEmitter emitter;
        ParticleEmission emission;
    };

    // A filter function as a filter's chain applies it (Filter Effects 1): a Gaussian blur of standard deviation blur
    // (item px); a color matrix on straight values, clamped, on linear-light values encoded again after when linear (an
    // SVG feColorMatrix in linearRGB); or a drop shadow: the image's alpha blurred by blur, offset by (x, y) (item px),
    // tinted with color (straight sRGB) and drawn under the image.
    struct FilterOp {
        enum class Kind : std::uint8_t {
            Blur,
            Matrix,
            DropShadow
        };

        Kind kind{};
        float blur{};
        ColorMatrix matrix = identity_color_matrix;
        bool linear{};
        Color color{};
        float x{};
        float y{};

        friend bool operator==(const FilterOp&, const FilterOp&) = default;
    };

    // A filtered element (CSS filter): content, its subtree drawn without its filter, opacity and transform, as a list
    // of its own over the item's box [0, width] x [0, height] (its target px are item px), drawn into an image of
    // pixels[0] x pixels[1] texels over that box (resolution of them per item px), then through the functions of ops in
    // order; the item draws the result with its opacity, clip and rounded clips. Images of equal sources
    // (scene_paint_source_equal), ops, pixels and resolutions are alike, whatever the item's transform and opacity;
    // source is null for content that follows the frame's time or a host's texture, whose image is drawn anew for each
    // pass.
    struct Filter {
        std::shared_ptr<const DisplayList> content;
        std::vector<FilterOp> ops;
        std::array<std::uint32_t, 2> pixels{};
        float resolution{};
        std::shared_ptr<const SceneNode> source;
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
            Effect,
            // Particles of one emission, in the item's space.
            Particles,
            // A Backdrop, which a renderer draws from a copy of its target.
            Backdrop,
            // A PathShadow, which a renderer prepares as coverage at device scale.
            PathShadow,
            // A Filter, whose content a renderer draws into an image, filters and draws over the item's box.
            Filter
        };

        Kind kind{};
        // Item space to target px.
        Affine transform = identity;
        ClipRect clip{};
        float width{};
        float height{};
        float opacity{1};
        // Index in DisplayList::boxes (Box), images (Image), rasters (Text, Layer), shadows (Shadow), paths (Path),
        // planes (Plane), effects (Effect), particles (Particles), backdrops (Backdrop), path_shadows (PathShadow) or
        // filters (Filter); unused by groups.
        std::uint32_t payload{};
        // The item's rounded clips, outermost first: DisplayList::clips[clips_begin, clips_begin + clips_count).
        std::uint32_t clips_begin{};
        std::uint32_t clips_count{};
        // 0, or 1 + the index of the anchored element whose anchor moves the item as a scene pass draws it (a scene
        // display list with deferred anchors): the item's transform, clip and rounded clips are then in the element's
        // own space, at its origin.
        std::uint32_t anchor{};
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
        std::vector<Particles> particles;
        std::vector<Backdrop> backdrops;
        std::vector<PathShadow> path_shadows;
        std::vector<Filter> filters;
        // Leases of the assets images, effects and particle sprites draw.
        std::vector<std::shared_ptr<const Asset>> assets;
        // Leases of the programs effects run.
        std::vector<std::shared_ptr<const EffectProgram>> programs;
        // Host textures images draw after the assets: image asset assets.size() + i is external texture i, linear
        // premultiplied and drawn with its own colors. The list only counts them; a renderer binds them per frame.
        std::uint32_t external_textures{};
        std::vector<RoundedClip> clips;
        // The names of the host textures images name (Image::texture): a scene page's -weft-texture(name)
        // backgrounds, which its host binds to textures for each frame.
        std::vector<std::string> textures;
    };

    // Whether a list is well formed: payloads in range and of the item's kind, images of a leased asset, an external
    // texture without a stroke or a named texture without an asset or a stroke, rasters with a source, shadows with
    // finite values and no negative blur, paths with a path of whole verbs and finite points and a stroke of finite,
    // non-negative lengths, planes of a well-formed content list (nested at most constants::max_plane_depth deep), a
    // positive size and a finite, invertible homography in front of the viewer, effects of a leased program with at
    // most its parameters and exactly its textures (coverage or sRGB assets of the list), finite values and
    // non-negative radii, particles with finite values, ordered ranges, positive lifetimes, non-negative rates, speeds,
    // drag, sizes and colors, a softness up to 1, a sprite of a coverage or sRGB asset of the list in at most
    // constants::max_particle_sprite_frames frames, and an emission of at most constants::max_particles_per_emission
    // particles that stopped no later than its elapsed time, backdrops with finite matrices and radii that are not
    // negative, a blur from 0 to constants::max_backdrop_blur, and under only inside a group, path shadows over the
    // Path items that follow them with finite values and no negative blur or content size, filters of a well-formed
    // content list (nested at most constants::max_filter_depth deep), pixels and a positive size, one to
    // constants::max_filter_functions functions of finite values, blurs from 0 to constants::max_filter_blur and drop
    // shadow offsets within constants::max_filter_offset, rounded clip ranges inside the clips, groups balanced and
    // nested at most max_group_depth deep. Diag::PaintList otherwise.
    Status check(const DisplayList& list, std::size_t max_group_depth);

    // The deepest group nesting of a well-formed list.
    std::size_t group_depth(const DisplayList& list);

    // Appends from's items after into's under transform, which maps from's target px into into's and must scale by
    // positive factors and translate (Diag::PaintList otherwise). Item transforms compose with it, clip rects and
    // rounded clips map through it, and payloads, assets, programs, rounded clips, external and named textures and the
    // items path shadows name are renumbered: from's assets follow into's, every external texture follows every asset,
    // and from's texture names follow into's. Filters keep their contents, which lie in their items' px.
    Status append(DisplayList& into, const DisplayList& from, const Affine& transform = identity);
} // namespace weft::paint
