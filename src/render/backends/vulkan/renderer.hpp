#pragma once

#include "paint/display_list.hpp"
#include "render/backends/vulkan/device.hpp"
#include "render/scene_pass.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// Vulkan renderer of display lists: the shared scene shader's linear pass (src/render/shaders/scene.hlsl compiled to
// SPIR-V with SPACE_LINEAR) through the portable pass plan, into an offscreen sRGB RGBA8 target of the renderer's size,
// isolated groups in linear float layers. Colors composite in linear premultiplied light as in the D3D11 renderer.
//
// This first renderer prepares a frame's assets with the frame and waits for every frame: draw() records, submits and
// waits for its fence, then frees the frame's resources. Failures throw std::runtime_error.
namespace weft::render::vulkan {
    using Pixel = std::array<std::uint8_t, 4>;

    struct Image {
        VkImage image{};
        VkDeviceMemory memory{};
        VkImageView view{};
        VkFramebuffer framebuffer{};
        VkFormat format{};
        std::uint32_t width{};
        std::uint32_t height{};
    };

    struct Buffer {
        VkBuffer buffer{};
        VkDeviceMemory memory{};
        void* mapped{};
        VkDeviceSize size{};
    };

    class Renderer {
    public:
        Renderer(Device& device, std::uint32_t width, std::uint32_t height);
        ~Renderer();
        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        // Draws list (boxes, images and isolated groups; no external textures) over a transparent target and returns
        // the target's pixels once the frame completed.
        std::vector<Pixel> draw(const paint::DisplayList& list);

        Device& gpu;
        const std::uint32_t width;
        const std::uint32_t height;
        // Draw calls and the records they drew, across all frames.
        std::size_t draws{};
        std::size_t instances{};

    private:
        // One render pass per target format: clear discards the target's contents, load continues on them. Both leave
        // the target as a color attachment.
        struct Passes {
            VkRenderPass clear{};
            VkRenderPass load{};
            VkPipeline pipeline{};
        };

        Image image(VkFormat format, std::uint32_t w, std::uint32_t h, VkImageUsageFlags usage);
        Buffer buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool mapped);
        void destroy(Image& image);
        void destroy(Buffer& buffer);
        Passes passes(VkFormat format);
        void framebuffer(Image& target, const Passes& passes);

        VkCommandPool pool_{};
        VkCommandBuffer command_{};
        VkFence fence_{};
        VkSampler sampler_{};
        VkShaderModule vertex_{};
        VkShaderModule fragment_{};
        VkDescriptorSetLayout set_layout_{};
        VkPipelineLayout layout_{};
        VkFormat layer_format_{};
        Passes output_passes_;
        Passes layer_passes_;
        Image output_;
        // Linear float targets of nested groups, created on first use.
        std::vector<Image> layers_;
        // Bound where a draw samples no image or the pass has no rounded clips.
        Image blank_image_;
        Buffer blank_buffer_;
        Buffer readback_;
    };
} // namespace weft::render::vulkan
