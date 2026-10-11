#pragma once

#include "paint/display_list.hpp"
#include "render/backends/vulkan/device.hpp"
#include "render/resource_lifetime.hpp"
#include "render/scene_pass.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

// Vulkan renderer of display lists: the shared scene shader's linear pass (src/render/shaders/scene.hlsl compiled to
// SPIR-V with SPACE_LINEAR) through the portable pass plan, into a color target of the renderer's size, isolated groups
// in linear float layers. Colors composite in linear premultiplied light as in the D3D11 renderer.
//
// prepare() creates the textures of a list's assets; encode() records their pending uploads and the pass into a command
// buffer that the caller submits. GPU resources retire only after the caller reports completion (ResourceLifetime): a
// texture once no frame or use pins it, the buffers, descriptors and framebuffer of one recorded frame with its use.
// draw() is that sequence on the renderer's own command buffer and fence. Failures throw std::runtime_error; the C ABI
// adapter (backend.hpp) turns them into results.
namespace weft::render::vulkan {
    using Pixel = std::array<std::uint8_t, 4>;

    // An asset's texture. Its texels stay until an upload of them was submitted, so that a cancelled recording uploads
    // again.
    struct NativeAsset {
        ResourceId id;
        Image image;
        std::vector<std::uint8_t> texels;
    };

    struct PreparedFrame {
        PreparedFrameId id;
        std::shared_ptr<const paint::DisplayList> list;
        // The textures of the list's assets, in order; the frame pins them.
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
    // framebuffer. Destroyed with the frame's use: once the frame completed, or at once when its commands were discarded
    // unsubmitted.
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
        Buffer view;
        VkDescriptorPool pool{};
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

        // Creates a texture for each of the list's assets and pins them for the frame. The list draws boxes, images
        // and isolated groups (no scene rasters, no external textures) at one device px per CSS px.
        PreparedFrame prepare(std::shared_ptr<const paint::DisplayList> list);
        // Releases the frame's CPU leases; its textures retire once no use pins them.
        void release(const PreparedFrame& frame);
        // Records the frame's pending uploads and its pass over target into command, a primary command buffer that is
        // recording outside a render pass. Every resource the commands use exists before the first command is recorded:
        // a failure records nothing and discards the use. clear replaces the target's contents with clear_color (linear
        // premultiplied); otherwise the frame draws over them.
        Encoded encode(const PreparedFrame& frame, VkCommandBuffer command, const Target& target, bool clear,
                       const paint::Color& clear_color);
        // Destroys the textures that retired and drops the texels of submitted uploads.
        void collect();

        // Draws list over a transparent target of the renderer's size and returns its pixels once the frame completed:
        // prepare, encode into the renderer's own command buffer, submit, wait and retire. The renderer must have no
        // other frame or use, since it numbers these submissions itself.
        std::vector<Pixel> draw(const paint::DisplayList& list);

        Device& gpu;
        const std::uint32_t width;
        const std::uint32_t height;
        ResourceLifetime tracker;
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
        Passes create_passes(VkFormat format);
        VkFramebuffer framebuffer(VkImageView view, VkFormat format);

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
        Buffer readback_;
        // Textures that have not retired yet.
        std::vector<std::unique_ptr<NativeAsset>> assets_;
    };
} // namespace weft::render::vulkan
