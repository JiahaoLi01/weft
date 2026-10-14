#pragma once

#include "constants/render.hpp"
#include "render/atlas_packer.hpp"
#include "render/scene_pass.hpp"
#include "scene/scene.hpp"
#include "scene/scene_display_list.hpp"
#include "scene/scene_geometry.hpp"
#include "scene/scene_image_cache.hpp"
#include "vector/path_raster.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// The backend-neutral half of a native scene pass: a frame's display list (scene_display_list), its plan
// (render/scene_pass.hpp) and the pixels its items sample: text images (scene_text_image), shadow images
// (render/shadow_image.hpp), path coverage (vector/path_raster.hpp), and the passes of perspective planes and explicit
// layers, which a backend draws into images of its own. Small text, shadow and path images lie on atlas pages of their
// format, so that runs of them and of boxes draw at once. A backend uploads each image once, by its id, while the image
// lives (in the preparer's caches or in the passes that sample it), into its page or a texture of its own, and draws
// the prepared passes.
//
// Owner thread of the SceneText.
namespace weft {
    // What a native scene renderer did, counted alike on every backend.
    struct SceneNativeWork {
        std::size_t passes{};
        std::size_t fallbacks{};
        std::size_t draws{};
        std::size_t text_builds{};
        // Text builds from portable images (the rest are reference raster images of color glyph images).
        std::size_t text_portable_builds{};
        // Images the reference raster drew: text whose color glyphs are images (PNG or SVG), and nothing else.
        std::size_t reference_builds{};
        std::size_t text_reuses{};
        std::size_t layer_builds{};
        std::size_t layer_reuses{};
        std::size_t shadow_builds{};
        std::size_t shadow_reuses{};
        std::size_t path_builds{};
        std::size_t path_reuses{};
        std::size_t effect_texture_builds{};
        std::size_t effect_texture_reuses{};
        // SVG elements listed from their kept values (SceneDisplayCache), and values taken anew.
        std::size_t svg_value_builds{};
        std::size_t svg_value_reuses{};
        // Plane contents drawn by nested passes into their images.
        std::size_t plane_passes{};
        // Backdrops copied from a target and filtered.
        std::size_t backdrop_passes{};
        // Cached images evicted for the caches' bounds (SceneCacheBounds).
        std::size_t image_evictions{};
        // Text, shadow and path images placed on atlas pages, and those small enough for one that found no page with
        // room (they get textures of their own).
        std::size_t atlas_placements{};
        std::size_t atlas_refusals{};
        // The most text, shadow and path images one pass sampled, and the most bytes, its planes' and layers' passes
        // included: the working set the caches' bounds must hold.
        std::size_t peak_pass_images{};
        std::size_t peak_pass_image_bytes{};
        // Fallback counts by reason.
        std::map<std::string, std::size_t> reasons;
    };

    // Where an image lies on a page of its preparer's atlas of its format: the page, and the texel of the image's first
    // pixel. Its edge texels repeat around it, atlas_border texels deep, so that filtering at its edges reads what a
    // clamped texture of its own would.
    struct SceneAtlasPlace {
        std::uint32_t page{};
        std::uint32_t x{};
        std::uint32_t y{};
    };

    // Pixels a pass samples, immutable once built. id is unique among a preparer's images and layers.
    struct SceneImage {
        enum class Format : std::uint8_t {
            // Premultiplied encoded sRGB in BGRA byte order, as the scene targets hold them: text of its own colors and
            // shadows.
            Bgra,
            // One byte per texel, sampled in r: coverage that its record tints (paths, and text masks: text of one
            // color), or an effect's coverage texture, read as data.
            Data,
            // Opaque sRGB in RGBA byte order, which the sampler decodes to linear light before filtering: an effect's
            // color texture.
            Srgb,
            // Linear premultiplied RGBA floats: an effect's translucent color texture.
            Linear
        };

        std::uint64_t id{};
        std::uint32_t width{};
        std::uint32_t height{};
        Format format{};
        std::vector<std::uint8_t> pixels;
        // On an atlas page, or (none) a texture of its own.
        std::optional<SceneAtlasPlace> atlas;
    };

    // The bytes of one texel of an image format.
    inline std::size_t scene_image_texel_bytes(SceneImage::Format format) {
        return format == SceneImage::Format::Data ? 1 : format == SceneImage::Format::Linear ? 4 * sizeof(float) : 4;
    }

    // An atlas page: the format of its images (Bgra or Data) and its number among the preparer's pages of that format.
    struct SceneAtlasPage {
        SceneImage::Format format{};
        std::uint32_t page{};

        bool operator==(const SceneAtlasPage&) const = default;
    };

    // The side of the atlas pages of a format (Bgra or Data), in texels.
    std::uint32_t scene_atlas_page_size(SceneImage::Format format);
    // An atlas image's pixels inside its border, (width + 2 border) x (height + 2 border) texels, rows top-down: what a
    // backend writes at (x - border, y - border) of its page.
    std::vector<std::uint8_t> scene_atlas_pixels(const SceneImage& image);

    struct ScenePrepared;

    // An explicit layer's image: the pass of its content, which a backend draws once into a width x height image and
    // keeps by id.
    struct SceneLayerImage {
        std::uint64_t id{};
        std::uint32_t width{};
        std::uint32_t height{};
        std::shared_ptr<const ScenePrepared> content;
    };

    // An effect of a prepared pass: its program, and the images of its textures in the program's order.
    struct ScenePreparedEffect {
        std::shared_ptr<const paint::EffectProgram> program;
        std::vector<std::shared_ptr<const SceneImage>> textures;
    };

    // A display list's pass ready to draw on a width x height device px target: its plan, and per item the image a
    // text, shadow or path item samples (null for a path with nothing in its clip), the pass of a plane's content
    // (drawn into an image of plane_pixels), or a layer's image; per effect of the list (DisplayList::effects, as
    // Effect batches name them) its program and textures; and per emission (DisplayList::particles, as Particles
    // batches name them) its sprite's image, as an effect's texture, or null for discs; and the atlas pages its Atlas
    // steps draw from (ScenePass::Batch::source). The record of an image on a page samples the page.
    struct ScenePrepared {
        render::ScenePass pass;
        unsigned width{};
        unsigned height{};
        std::vector<std::shared_ptr<const SceneImage>> images;
        std::vector<std::unique_ptr<ScenePrepared>> planes;
        std::vector<std::shared_ptr<const SceneLayerImage>> layers;
        std::vector<ScenePreparedEffect> effects;
        std::vector<std::shared_ptr<const SceneImage>> sprites;
        std::vector<SceneAtlasPage> pages;
    };

    // Scratch bytes the targets of a prepared pass and of its planes' passes take: their groups, and a plane's pass
    // its image too.
    std::size_t scene_scratch_bytes(const ScenePrepared& prepared, bool plane = false);

    // The paint source of a node as bytes: two nodes have the same key exactly when scene_paint_source_equal finds them
    // equal but for their identities (a node with an effect or particles, or with a number that is not a number,
    // has a key of its own).
    std::string scene_paint_source_key(const SceneNode& node);

    // Bounds of each of a preparer's image caches (text, layers, shadows, paths). Past them the least recently used
    // images of earlier passes go; what the current pass uses stays (scene_image_cache.hpp). And the atlas pages of each
    // format: with none, every image has a texture of its own.
    struct SceneCacheBounds {
        std::size_t entries = std::size_t(constants::render::scene::image_cache_max_entries);
        std::size_t bytes = std::size_t(constants::render::scene::image_cache_budget_bytes);
        std::uint32_t atlas_pages = std::uint32_t(constants::render::scene::atlas_max_pages);
    };

    class ScenePreparer {
    public:
        // Draws a frame as the reference raster draws it, for text whose color glyphs are images; without one such
        // text cannot be drawn.
        using Reference = std::function<SceneBitmap(const SceneFrame& frame, float scale)>;

        // Plane and layer images are at most max_image_dimension device px wide and high.
        ScenePreparer(SceneText& text, SceneNativeWork& work, std::uint32_t max_image_dimension,
                      Reference reference = {}, SceneCacheBounds bounds = {});
        ScenePreparer(const ScenePreparer&) = delete;
        ScenePreparer& operator=(const ScenePreparer&) = delete;

        // The frame's pass on a width x height device px target at scale device px per CSS px, clipped to crop when
        // given; layers are the nodes drawn as cached layer images, view_index tells views apart in the caches. anchors
        // place the anchored elements (scene_display_list); for data-anchor-missing: stay the preparer remembers per
        // view what they took in its last pass. Null, with a fallback and its reason counted in work, when a native
        // pass cannot draw the frame.
        std::unique_ptr<ScenePrepared> prepare(const SceneFrame& frame, float scale, unsigned width, unsigned height,
                                               std::optional<std::array<float, 4>> crop, unsigned view_index,
                                               const std::vector<NodeIdentity>& layers,
                                               const SceneAnchorTable* anchors = nullptr);

    private:
        // A layer image by view and node identity, while its paint source and scale stay the same.
        struct Layer {
            SceneNode key;
            float scale{};
            std::shared_ptr<const SceneLayerImage> image;
        };

        // A path coverage image, with the path and dashes it was drawn from: a key of the same digest is the same
        // coverage when they are equal.
        struct PathImage {
            std::shared_ptr<const paint::Path> path;
            std::vector<float> dashes;
            std::shared_ptr<const SceneImage> image;
        };

        // The content digest of a path object, while it lives.
        struct Digest {
            std::weak_ptr<const paint::Path> path;
            std::uint64_t value{};
        };

        // An image on an atlas page and its region (border included), which goes back once the image is gone: no
        // cache, pass or backend lease holds it, so nothing may sample it any more.
        struct Placed {
            std::weak_ptr<const SceneImage> image;
            // On the coverage atlas, or the color one.
            bool coverage{};
            render::AtlasPacker::Region region;
        };

        SceneText& text_;
        SceneNativeWork& work_;
        std::uint32_t max_image_dimension_{};
        Reference reference_;
        SceneGeometry geometry_;
        vector::Rasterizer vector_;
        std::uint64_t next_id_{};
        // SVG elements' values between passes.
        SceneDisplayCache svg_;
        // Text images by paint source (scene_paint_source_key), scale and margin, whatever node they come from.
        SceneImageCache<std::string, std::shared_ptr<const SceneImage>> texts_;
        // Layer images by view and node identity.
        SceneImageCache<std::array<std::uint64_t, 5>, Layer> layers_;
        // Shadow images by value: offset, blur, spread, color, radii, box size and scale.
        SceneImageCache<std::array<float, 15>, std::shared_ptr<const SceneImage>> shadows_;
        // Path coverage images by value: the path's digest and sizes, its fill rule and stroke (the dashes by digest),
        // the device map and the mask's pixels, each number by its bits.
        SceneImageCache<std::array<std::uint32_t, 21>, PathImage> paths_;
        // Path objects' digests, so that a path drawn again is keyed without reading its points.
        std::unordered_map<const paint::Path*, Digest> digests_;
        std::size_t digests_swept_{};
        // The images of effects' textures by asset; the cache holds the asset while it keeps its image.
        SceneImageCache<std::shared_ptr<const paint::Asset>, std::shared_ptr<const SceneImage>> effect_textures_;
        // The atlas pages of coverage (Data) and of colors (Bgra), and the images on them.
        render::AtlasPacker coverage_atlas_;
        render::AtlasPacker color_atlas_;
        bool atlas_{};
        std::vector<Placed> placed_;
        // What each view's anchored elements took in its last pass.
        std::unordered_map<unsigned, std::vector<ScenePlacedAnchor>> anchored_;

        // Places a new text, shadow or path image on its format's atlas when it is small enough and a page has room.
        void place(const std::shared_ptr<SceneImage>& image);

        // Plans list and builds the images it samples, its planes' and layers' contents included; throws
        // std::invalid_argument with the reason for a list a native pass cannot draw.
        std::unique_ptr<ScenePrepared> prepare(const paint::DisplayList& list, float scale, unsigned width,
                                               unsigned height, std::optional<std::array<float, 4>> crop,
                                               unsigned view_index);

        struct Built {
            std::shared_ptr<const SceneImage> text;
            std::shared_ptr<const SceneLayerImage> layer;
        };

        // A text or layer item's image, from the cache while its source and scale stay the same.
        Built image(const paint::Item& item, const paint::Raster& raster, float scale, unsigned view_index);
        // The content digest of a path: its verbs, points and fill rule.
        std::uint64_t digest(const std::shared_ptr<const paint::Path>& path);
        // A shadow item's image; throws std::invalid_argument past the shadow bitmap bound.
        std::shared_ptr<const SceneImage> shadow(const paint::Item& item, const paint::Shadow& shadow, float scale);
        // A path item's coverage over mask (device pixels); throws std::invalid_argument past the mask bounds.
        std::shared_ptr<const SceneImage> path(const paint::Item& item, const paint::PathPaint& paint, float scale,
                                               const std::array<std::int32_t, 4>& mask);
        // The image of an effect's texture: coverage as data, an opaque sRGB image as sRGB, a translucent one as
        // linear premultiplied floats.
        std::shared_ptr<const SceneImage> effect_texture(const std::shared_ptr<const paint::Asset>& asset);
    };
} // namespace weft
