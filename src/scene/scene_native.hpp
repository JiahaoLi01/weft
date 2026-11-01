#pragma once

#include "constants/render.hpp"
#include "render/atlas_packer.hpp"
#include "render/filter_chain.hpp"
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
#include <span>
#include <vector>

// The backend-neutral half of a native scene pass: a frame's display list (scene_display_list), its plan
// (render/scene_pass.hpp) and the pixels its items sample: text images (scene_text_image), shadow images
// (render/shadow_image.hpp), path coverage (vector/path_raster.hpp), and the passes of perspective planes, explicit
// layers and filtered elements' contents, which a backend draws into images of its own (a filter's then through its
// chain of passes, render/filter_chain.hpp). Small text, shadow and path images lie on atlas pages of their
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
        // Text items drawn on the device's pixels: still text (paint::Raster::still) on a translation of the target,
        // without an anchor.
        std::size_t text_on_pixels{};
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
        // Filtered images prepared (their contents' passes and chains), and those taken from the cache for a filter of
        // the same source, functions and pixels; and filtered images a backend drew (a content pass and its chain).
        std::size_t filter_builds{};
        std::size_t filter_reuses{};
        std::size_t filter_passes{};
        // Backdrops copied from a target and filtered; and copies of what lies behind effects that read their backdrop.
        std::size_t backdrop_passes{};
        std::size_t backdrop_copies{};
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
        // Passes drawn again from a pass kept for a frame of the same revision, with the frame's anchors; and kept
        // passes prepared again because the anchors reordered records a batch could not take in another order.
        std::size_t kept_passes{};
        std::size_t kept_rebuilds{};
        // Passes placed in another order than the one they were planned in (runs their anchors' depths reordered).
        std::size_t reordered_passes{};
        // Kept passes patched for a frame of another revision (ScenePreparer::patch), and the records the patches
        // wrote; patches refused (the pass is prepared anew), by reason.
        std::size_t kept_patches{};
        std::size_t patched_records{};
        std::map<std::string, std::size_t> patch_refusals;
        // Draws of a kept pass whose slot held an earlier version and wrote only what the patches since changed.
        std::size_t partial_uploads{};
        // Host texture items drawn, and those whose name the frame bound to no texture (they draw nothing).
        std::size_t texture_draws{};
        std::size_t unbound_textures{};
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

    // A filtered element's image (paint::Filter): the pass of its content, which a backend draws at scale device px per
    // item px into an image of chain.images[0], then through chain's passes, and keeps the result (chain.result) by id.
    struct SceneFilterImage {
        std::uint64_t id{};
        float scale{};
        std::shared_ptr<const ScenePrepared> content;
        render::FilterChain chain;
    };

    // An effect of a prepared pass: its program, and the images of its textures in the program's order.
    struct ScenePreparedEffect {
        std::shared_ptr<const paint::EffectProgram> program;
        std::vector<std::shared_ptr<const SceneImage>> textures;
    };

    // A display list's pass ready to draw on a width x height device px target: its plan, and per item the image a
    // text, shadow or path item samples (null for a path with nothing in its clip), the pass of a plane's content
    // (drawn into an image of plane_pixels), a layer's image, a filter's image, or the host texture an image item names
    // (one past an index of texture_names, the list's DisplayList::textures; 0 for other items); per effect of the list
    // (DisplayList::effects, as Effect batches name them) its program and textures; and per emission
    // (DisplayList::particles, as Particles batches name them) its sprite's image, as an effect's texture, or null for
    // discs; and the atlas pages its Atlas steps draw from (ScenePass::Batch::source). The record of an image on a page
    // samples the page.
    struct ScenePrepared {
        render::ScenePass pass;
        unsigned width{};
        unsigned height{};
        std::vector<std::shared_ptr<const SceneImage>> images;
        std::vector<std::unique_ptr<ScenePrepared>> planes;
        std::vector<std::shared_ptr<const SceneLayerImage>> layers;
        std::vector<std::shared_ptr<const SceneFilterImage>> filters;
        std::vector<std::uint32_t> textures;
        std::vector<std::string> texture_names;
        std::vector<ScenePreparedEffect> effects;
        std::vector<std::shared_ptr<const SceneImage>> sprites;
        std::vector<SceneAtlasPage> pages;
        // Prepared with deferred anchors (ScenePreparer::prepare): the anchored elements the pass moves as it draws
        // (records name them, render::SceneRecord::clips), the runs of anchored siblings it orders by depth, and the
        // size of the frame's root box the anchors place them in. keepable: the pass draws any frame of its revision
        // when given the frame's anchors (ScenePreparer::place); otherwise anchors are part of it.
        std::vector<SceneListAnchor> anchors;
        std::vector<SceneListRun> runs;
        std::array<float, 2> view{};
        bool keepable{};
        // Drawn over what the target holds (SceneTargetStart::over): the page's root background is a box, not the
        // pass's clear.
        bool over{};
    };

    // A host texture a pass draws for a -weft-texture(name) background of its page: the backend's view of it (an
    // ID3D11ShaderResourceView, a VkImageView) and whether its rows start at the bottom.
    struct SceneTexture {
        std::string name;
        std::uint64_t view{};
        bool bottom_left{};
    };

    // The texture textures binds to each name of a prepared pass (ScenePrepared::texture_names; the first of a name
    // when it appears twice), null for a name they leave unbound.
    std::vector<const SceneTexture*> scene_texture_bindings(const ScenePrepared& prepared,
                                                            std::span<const SceneTexture> textures);

    // Whether the texture bound to each name of a prepared pass has rows that start at the bottom (false when unbound):
    // its records sample them flipped, so a kept pass draws again only for the same.
    std::vector<bool> scene_texture_flips(const ScenePrepared& prepared, std::span<const SceneTexture> textures);

    // Whether a pass draws a host texture inside one of its planes or filtered images, at any depth: a kept pass draws
    // their content once, which would keep a texture's content of that frame.
    bool scene_planes_draw_textures(const ScenePrepared& prepared);

    // How a pass starts on its target: it replaces what the target holds (the probes' targets: the page's root
    // background folded into the clear), or it draws over it, as a host's frame (a game's) is drawn over, first
    // cleared to clear (encoded premultiplied) when given.
    struct SceneTargetStart {
        bool over{};
        std::optional<paint::Color> clear;
    };

    // What a pass with deferred anchors draws with at one frame: per anchored element its anchor's map to target px,
    // {scale, x, y, shown} (shown 0 when hidden), and the record each draw position takes (render::ScenePass::batches
    // count positions); empty for the order the pass was planned in.
    struct SceneAnchorFrame {
        std::vector<std::array<float, 4>> anchors;
        std::vector<std::uint32_t> order;
    };

    // What a patch changed in a kept pass (ScenePreparer::patch): the records [begin, end) and the rounded clips
    // [begin, end) it wrote, which a backend holding the pass's records in its buffers writes again.
    struct ScenePatch {
        std::vector<std::array<std::uint32_t, 2>> records;
        std::vector<std::array<std::uint32_t, 2>> clips;
    };

    // The patches a kept pass took (ScenePreparer::patch), by the serials a backend gives the pass's versions: a slot
    // that holds the records of an earlier version writes again only what the patches since changed, while the
    // history holds them (the last few).
    class ScenePatchHistory {
    public:
        // A patch took the pass from version `from` (the last patch's `to`, or the version the pass was prepared as)
        // to version `to`.
        void add(std::uint64_t from, std::uint64_t to, const ScenePatch& patch);

        // The ranges the patches after version `from` wrote, up to the last; false when the history does not reach
        // back to it.
        bool since(std::uint64_t from, ScenePatch& out) const;

    private:
        struct Applied {
            std::uint64_t from{};
            std::uint64_t to{};
            ScenePatch patch;
        };

        std::vector<Applied> applied_;
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
        // view what they took in its last pass or placing. With defer, and without crop, anchors are deferred where the
        // list can defer them: the pass is keepable, and place gives each frame's anchors. Null, with a fallback and
        // its reason counted in work, when a native pass cannot draw the frame.
        // over prepares the pass to draw over its target's contents (ScenePrepared::over).
        std::unique_ptr<ScenePrepared> prepare(const SceneFrame& frame, float scale, unsigned width, unsigned height,
                                               std::optional<std::array<float, 4>> crop, unsigned view_index,
                                               const std::vector<NodeIdentity>& layers,
                                               const SceneAnchorTable* anchors = nullptr, bool defer = false,
                                               bool over = false);

        // The reason of the last fallback prepare counted (empty before the first).
        const std::string& last_fallback() const {
            return last_fallback_;
        }

        // Brings prepared, the keepable pass the last prepare gave view_index, to frame in place where it can: the
        // subtrees of frame whose storage differs from the last listed frame's (SceneChildren) are listed again in
        // their elements' context (scene_display_list's part), and their items, rounded clips, records, masks and
        // images replace those of the elements they replace. Returns what it wrote, or nullopt (prepared unchanged)
        // where the pass must be prepared anew: another root, children in another order or z-index, a list of the
        // view with effects, an anchoring or backdrop filter added or gone, a part that lists other items (in number,
        // kind or payload) or anything but boxes, text, shadows, paths, filtered images and groups, an image that moves
        // batches (to another atlas page, on or off the atlas), or a part at the list's first item (which may be the
        // clear).
        std::optional<ScenePatch> patch(ScenePrepared& prepared, const SceneFrame& frame, unsigned view_index,
                                        const SceneAnchorTable* anchors);

        // The list and spans patch keeps for a view, null for none: tests compare them with a fresh list's.
        const SceneDisplayList* listed(unsigned view_index) const {
            const auto it = listed_.find(view_index);
            return it == listed_.end() ? nullptr : &it->second.list;
        }

        const SceneListSpans* listed_spans(unsigned view_index) const {
            const auto it = listed_.find(view_index);
            return it == listed_.end() ? nullptr : &it->second.spans;
        }

        // Drops what the preparer keeps for a view (its anchored elements' last anchors, its last list).
        void forget(unsigned view_index) {
            anchored_.erase(view_index);
            listed_.erase(view_index);
        }

        // Places a keepable pass's anchored elements by anchors, as the list would place them, into frame; false when
        // they are to be ordered otherwise than a batch of the pass can draw (prepare the frame again).
        bool place(const ScenePrepared& prepared, unsigned view_index, const SceneAnchorTable* anchors,
                   SceneAnchorFrame& frame);

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
        std::string last_fallback_;
        std::uint32_t max_image_dimension_{};
        Reference reference_;
        SceneGeometry geometry_;
        vector::Rasterizer vector_;
        std::uint64_t next_id_{};
        // SVG elements' values between passes.
        SceneDisplayCache svg_;

        // A text image, and whether it goes on the device's pixels: drawn from portable glyphs for its item's place
        // across them (not by the reference raster, whose text lies where its line puts it).
        struct Text {
            std::shared_ptr<const SceneImage> image;
            bool on_pixels{};
        };

        // Text images by paint source (scene_paint_source_key), scale and margin, whatever node they come from, and for
        // still text where it lies across a device pixel.
        SceneImageCache<std::string, Text> texts_;
        // Layer images by view and node identity.
        SceneImageCache<std::array<std::uint64_t, 5>, Layer> layers_;
        // Shadow images by value: offset, blur, spread, color, radii, box size, scale and whether they are inset.
        SceneImageCache<std::array<float, 16>, std::shared_ptr<const SceneImage>> shadows_;
        // Path coverage images by value: the path's digest and sizes, its fill rule and stroke (the dashes by digest),
        // the device map and the mask's pixels, each number by its bits.
        SceneImageCache<std::array<std::uint32_t, 21>, PathImage> paths_;

        // Filtered images by view and node identity, with the source, functions, pixels and resolution they were made
        // of: the same image while those stay (whatever the element's transform and opacity).
        struct Filtered {
            std::shared_ptr<const SceneNode> source;
            std::vector<paint::FilterOp> ops;
            std::array<std::uint32_t, 2> pixels{};
            float resolution{};
            std::shared_ptr<const SceneFilterImage> image;
        };

        SceneImageCache<std::array<std::uint64_t, 4>, Filtered> filters_;

        // Path shadow images by the values they depend on (the paths by their digests), with the paths, which a key of the
        // same digests must equal.
        struct PathShadowImage {
            std::vector<std::shared_ptr<const paint::Path>> paths;
            std::shared_ptr<const SceneImage> image;
        };

        SceneImageCache<std::string, PathShadowImage> path_shadows_;
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

        // What each view's last keepable deferred pass was listed from, for patch: the frame (its root, whose nodes the
        // spans name), the list and its elements' spans, and the scale and target size.
        struct Listed {
            SceneFrame frame;
            SceneDisplayList list;
            SceneListSpans spans;
            float scale{};
            unsigned width{};
            unsigned height{};
        };

        std::unordered_map<unsigned, Listed> listed_;

        // Places a new text, shadow or path image on its format's atlas when it is small enough and a page has room.
        void place(const std::shared_ptr<SceneImage>& image);

        // Plans list and builds the images it samples, its planes' and layers' contents included; anchors is how many
        // anchors move its items. Throws std::invalid_argument with the reason for a list a native pass cannot draw.
        std::unique_ptr<ScenePrepared> prepare(const paint::DisplayList& list, float scale, unsigned width,
                                               unsigned height, std::optional<std::array<float, 4>> crop,
                                               unsigned view_index, std::uint32_t anchors = 0, bool over = false);

        struct Built {
            std::shared_ptr<const SceneImage> text;
            std::shared_ptr<const SceneLayerImage> layer;
            // How far (target px) the item's record moves to put still text on the device's pixels.
            std::array<float, 2> shift{};
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
        // A path shadow item's coverage over mask (device pixels): its paths rasterized again where the shadow puts
        // them, together, inside the content they were clipped to, blurred.
        std::shared_ptr<const SceneImage> path_shadow(const paint::DisplayList& list, const paint::Item& item,
                                                      float scale, const std::array<std::int32_t, 4>& mask);
        // The image of an effect's texture: coverage as data, an opaque sRGB image as sRGB, a translucent one as
        // linear premultiplied floats.
        std::shared_ptr<const SceneImage> effect_texture(const std::shared_ptr<const paint::Asset>& asset);
        // A filter item's image, from the cache while its source, functions, pixels and resolution stay; throws
        // std::invalid_argument past the image bounds.
        std::shared_ptr<const SceneFilterImage> filter(const paint::Filter& filter, unsigned view_index);
    };
} // namespace weft
