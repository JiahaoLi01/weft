#pragma once

#include "render/backends/vulkan/device.hpp"

#include <cstddef>
#include <cstdint>

// What the Vulkan renderers of the shared scene shader (src/render/shaders/scene.hlsl) record alike: shader modules,
// image barriers, the render passes and pipeline of a color target format, the descriptor set layout of the shader's
// bindings (constants::render::vulkan), its sampler and descriptor pools.
namespace weft::render::vulkan {
    // A shader module of SPIR-V bytes, whole words.
    VkShaderModule shader_module(const Device& d, const unsigned char* bytes, std::size_t size);

    VkImageMemoryBarrier image_barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags written,
                                       VkAccessFlags read);

    void transition(const Device& d, VkCommandBuffer command, VkImage image, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags written, VkAccessFlags read, VkPipelineStageFlags before,
                    VkPipelineStageFlags after) noexcept;

    // The render passes of a color target format, clear (discarding the target's contents) and load (continuing on
    // them), both leaving the target a color attachment, and the pipeline of a shader pair drawing the scene shader's
    // records into them with premultiplied source-over.
    struct ScenePasses {
        VkRenderPass clear{};
        VkRenderPass load{};
        VkPipeline pipeline{};
    };

    // A failure throws and leaves nothing behind.
    ScenePasses create_scene_passes(const Device& d, VkFormat format, VkPipelineLayout layout, VkShaderModule vertex,
                                    VkShaderModule fragment);
    // The pipeline of a shader pair for the render passes of pass (those of one target format, as create_scene_passes
    // makes them): the scene shader's records with premultiplied source-over.
    VkPipeline create_scene_pipeline(const Device& d, VkRenderPass pass, VkPipelineLayout layout, VkShaderModule vertex,
                                     VkShaderModule fragment);
    void destroy(const Device& d, ScenePasses& passes) noexcept;

    // The scene shader's one descriptor set: View (b0), records (t0), image (t1), clips (t2) and the sampler (s0), and
    // an effect's textures (t3-t6) and repeating sampler (s1).
    VkDescriptorSetLayout create_scene_set_layout(const Device& d);
    // Linear filtering, clamped to the edge, or repeating.
    VkSampler create_linear_sampler(const Device& d, bool repeat = false);
    // A descriptor pool for capacity sets of the scene set layout.
    VkDescriptorPool create_scene_pool(const Device& d, std::uint32_t capacity);
} // namespace weft::render::vulkan
