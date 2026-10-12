#pragma once

#include "constants/render.hpp"
#include "paint/display_list.hpp"
#include "render/atlas_packer.hpp"
#include "render/backends/vulkan/device.hpp"
#include "render/resource_lifetime.hpp"
#include "render/scene_pass.hpp"
#include "render/texture_cache.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

// Vulkan renderer of display lists: the shared scene shader's linear pass (src/render/shaders/scene.hlsl compiled to
// SPIR-V with SPACE_LINEAR) through the portable pass plan, into a color target of the renderer's size, isolated groups
// in linear float layers. Colors composite in linear premultiplied light as in the D3D11 renderer.
//
// prepare() creates the textures of a list's assets; encode() records their pending uploads and the pass into a command
// buffer that the caller submits. GPU resources retire only after the caller reports completion (ResourceLifetime): a
// texture once no frame, use or cache holds it, the buffers, descriptors and framebuffer of one recorded frame with its
// use. A caching renderer keeps textures across frames and puts small coverage images on glyph atlas pages with the
// policy that the D3D11 renderer shares (render/texture_cache.hpp). draw() is the frame sequence on the renderer's own
// command buffer and fence. Failures throw std::runtime_error; the C ABI adapter (backend.hpp) turns them into results.
namespace weft::render::vulkan {
    using Pixel = std::array<std::uint8_t, 4>;

    // Thrown by encode() when an atlas page that the frame samples is first written by another recording that has not
    // been submitted: that recording's first layout transition discards the page, so it must be submitted or discarded
    // before another recording writes the page.
    struct Busy : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    // An asset's texture: an image of its own, or a region of an atlas page. Its texels (for a region, the bordered
    // image) stay until an upload of them was submitted, so that a cancelled recording uploads again.
    struct NativeAsset {
        ResourceId id;
        Image image;
        std::vector<std::uint8_t> texels;
        std::optional<AtlasPacker::Region> atlas;
    };

    // A glyph atlas page: an R8 image that regions share, and its resource, whose upload is the page's first layout
    // transition.
    struct AtlasPage {
        ResourceId id;
        Image image;
    };

    // What the texture cache keeps of a texture: its resource and the texture in the renderer's registry.
    struct CachedTexture {
        ResourceId id;
        NativeAsset* texture{};
    };

    struct PreparedFrame {
        PreparedFrameId id;
        std::shared_ptr<const paint::DisplayList> list;
        // The textures of the list's assets, in order; the frame holds a lease of each and pins them, and the atlas
        // pages they are on.
        std::vector<NativeAsset*> assets;
    };

    // A color target of the renderer's size: a view of a single-sample 2D image of format, one mip level and layer, in
    // VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL when the commands execute. The commands leave it in that layout.
    struct Target {
        VkImageView view{};
        VkFormat format{};
    };

    class Renderer;

    // What one recorded frame uses besides its textures: pass data, staged uploads, descriptors and the target's
    // framebuffer. Released with the frame's use: once the frame completed, or at once when its commands were discarded
    // unsubmitted. The renderer then reuses the buffers and the descriptor pool for later frames.
    class FrameResources {
    public:
        FrameResources() = default;
        explicit FrameResources(Renderer& renderer);
        FrameResources(FrameResources&& other) noexcept;
        FrameResources& operator=(FrameResources&& other) noexcept;
        ~FrameResources();

        Buffer staging;
        Buffer records;
        Buffer clips;
        VkDescriptorPool pool{};
        // The descriptor sets the pool holds.
        std::uint32_t pool_sets{};
        VkFramebuffer framebuffer{};

    private:
        void release() noexcept;

        Renderer* renderer_{};
    };

    struct Encoded {
        UseTokenId token;
        FrameResources resources;
    };

    class Renderer {
    public:
        Renderer(Device& device, std::uint32_t width, std::uint32_t height);
        ~Renderer();
        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        // Finds or creates a texture for each of the list's assets and pins them for the frame. The list draws boxes,
        // images and isolated groups (no scene rasters, no external textures) at one device px per CSS px.
        PreparedFrame prepare(std::shared_ptr<const paint::DisplayList> list);
        // Releases the frame's CPU leases; its textures retire once no use or cache holds them.
        void release(const PreparedFrame& frame);
        // Records the frame's pending uploads and its pass over target into command, a primary command buffer that is
        // recording outside a render pass. Every resource the commands use exists before the first command is recorded:
        // a failure, Busy among them, records nothing and discards the use. clear replaces the target's contents with
        // clear_color (linear premultiplied); otherwise the frame draws over them.
        Encoded encode(const PreparedFrame& frame, VkCommandBuffer command, const Target& target, bool clear,
                       const paint::Color& clear_color);
        // Destroys the textures that retired and drops the texels of submitted uploads.
        void collect();
        // Releases every cached texture and the atlas pages, for a renderer with no frame prepared or in flight that
        // shuts down.
        void drain();
        // The largest buffer kept from released frames for later ones.
        VkDeviceSize largest_spare() const;

        // Draws list over a transparent target of the renderer's size and returns its pixels once the frame completed:
        // prepare, encode into the renderer's own command buffer, submit, wait and retire. The renderer must have no
        // other frame or use, since it numbers these submissions itself.
        std::vector<Pixel> draw(const paint::DisplayList& list);

        Device& gpu;
        const std::uint32_t width;
        const std::uint32_t height;
        ResourceLifetime tracker;
        // Keep textures by asset identity across frames, and put small coverage images on the glyph atlas.
        bool cache_assets{};
        TextureCache<CachedTexture> cache{weft::constants::render::texture_cache::max_resources,
                                          weft::constants::render::texture_cache::max_bytes};
        AssetIdentities asset_identities;
        AtlasPacker atlas{weft::constants::render::texture_cache::atlas_page_size,
                          weft::constants::render::texture_cache::atlas_max_pages};
        std::vector<AtlasPage> atlas_pages;
        std::size_t atlas_placements{};
        // Draw calls and the records they drew, uploads recorded, and encodes that recorded commands.
        std::size_t draws{};
        std::size_t instances{};
        std::size_t uploads{};
        std::size_t recordings{};
        // Textures and frame resources created and destroyed.
        std::size_t native_creations{};
        std::size_t native_releases{};
        std::size_t frames_created{};
        std::size_t frames_released{};
        // Makes the next encode fail once its resources exist, before it records anything (rollback tests).
        bool inject_encode_failure{};

    private:
        friend class FrameResources;

        // One render pass per target format: clear discards the target's contents, load continues on them. Both leave
        // the target as a color attachment.
        struct Passes {
            VkRenderPass clear{};
            VkRenderPass load{};
            VkPipeline pipeline{};
        };

        // The render passes and pipeline of format, created on first use.
        const Passes& passes(VkFormat format);
        VkFramebuffer framebuffer(VkImageView view, VkFormat format);
        // The texture of a new asset, in the registry and leased once: an atlas region of a caching renderer when the
        // asset fits there, otherwise an image of its own.
        NativeAsset& create(const paint::Asset& asset, std::uint64_t asset_id, std::uint64_t version);
        // A buffer of at least size, from spares or new with room to grow.
        Buffer take(std::vector<Buffer>& spares, VkDeviceSize size, VkBufferUsageFlags usage);
        // A reset descriptor pool for at least sets sets, from the spares or new with room to grow.
        std::pair<VkDescriptorPool, std::uint32_t> take_pool(std::uint32_t sets);
        // Keeps a released frame's buffers and descriptor pool for later frames; destroys what is not kept.
        void recycle(FrameResources& resources) noexcept;
        // A region for the asset's bordered image, on a new page when the open ones are full; nothing when the atlas is.
        std::optional<AtlasPacker::Region> place_on_atlas(const paint::Asset& asset);
        // The view a texture is sampled through: its page's, or its own.
        VkImageView view(const NativeAsset& texture) const;

        std::uint64_t next_asset_id_{1};
        VkCommandPool pool_{};
        VkCommandBuffer command_{};
        VkFence fence_{};
        VkSampler sampler_{};
        VkShaderModule vertex_{};
        VkShaderModule fragment_{};
        VkDescriptorSetLayout set_layout_{};
        VkPipelineLayout layout_{};
        VkFormat layer_format_{};
        std::map<VkFormat, Passes> passes_;
        Image output_;
        // Linear float targets of nested groups, created when a pass first nests that deep.
        std::vector<Image> layers_;
        // Bound where a draw samples no image or the pass has no rounded clips.
        Image blank_image_;
        Buffer blank_buffer_;
        // The View constants of every pass: the renderer's size, scale 1 and base instance 0.
        Buffer view_;
        // draw()'s copy of the target, created by its first call.
        Buffer readback_;
        // Buffers and descriptor pools of released frames, at most spare_frame_resources of each kind.
        std::vector<Buffer> spare_staging_;
        std::vector<Buffer> spare_records_;
        std::vector<Buffer> spare_clips_;
        std::vector<std::pair<VkDescriptorPool, std::uint32_t>> spare_pools_;
        // Textures that have not retired yet.
        std::vector<std::unique_ptr<NativeAsset>> assets_;
    };
} // namespace weft::render::vulkan
