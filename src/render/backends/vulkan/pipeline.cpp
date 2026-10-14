#include "render/backends/vulkan/pipeline.hpp"

#include "constants/render.hpp"

#include <cstring>
#include <iterator>
#include <vector>

namespace weft::render::vulkan {
    namespace {
        namespace limits = weft::constants::render::vulkan;
    } // namespace

    VkShaderModule shader_module(const Device& d, const unsigned char* bytes, std::size_t size) {
        ensure(size % sizeof(std::uint32_t) == 0, "SPIR-V is whole words");
        std::vector<std::uint32_t> words(size / sizeof(std::uint32_t));
        std::memcpy(words.data(), bytes, size);
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = size;
        info.pCode = words.data();
        VkShaderModule module{};
        check(d.vkd.vkCreateShaderModule(d.device, &info, nullptr, &module), "vkCreateShaderModule");
        return module;
    }

    VkImageMemoryBarrier image_barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags written,
                                       VkAccessFlags read) {
        VkImageMemoryBarrier out{};
        out.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        out.srcAccessMask = written;
        out.dstAccessMask = read;
        out.oldLayout = from;
        out.newLayout = to;
        out.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        out.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        out.image = image;
        out.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        return out;
    }

    void transition(const Device& d, VkCommandBuffer command, VkImage image, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags written, VkAccessFlags read, VkPipelineStageFlags before,
                    VkPipelineStageFlags after) noexcept {
        const auto b = image_barrier(image, from, to, written, read);
        d.vkd.vkCmdPipelineBarrier(command, before, after, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    ScenePasses create_scene_passes(const Device& d, VkFormat format, VkPipelineLayout layout, VkShaderModule vertex,
                                    VkShaderModule fragment) {
        ScenePasses out;
        try {
            for (const bool clear : {true, false}) {
                VkAttachmentDescription attachment{};
                attachment.format = format;
                attachment.samples = VK_SAMPLE_COUNT_1_BIT;
                attachment.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
                attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                attachment.initialLayout = clear ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                const VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
                VkSubpassDescription subpass{};
                subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
                subpass.colorAttachmentCount = 1;
                subpass.pColorAttachments = &color;
                // Earlier writes of the target, of uploads and of group layers, and earlier reads of a layer as an
                // image, come first; later reads of the target as an image or a copy source wait for its writes.
                const VkSubpassDependency dependencies[]{
                    {VK_SUBPASS_EXTERNAL, 0,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_SHADER_READ_BIT,
                     0},
                    {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                     0}};
                VkRenderPassCreateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
                info.attachmentCount = 1;
                info.pAttachments = &attachment;
                info.subpassCount = 1;
                info.pSubpasses = &subpass;
                info.dependencyCount = std::uint32_t(std::size(dependencies));
                info.pDependencies = dependencies;
                check(d.vkd.vkCreateRenderPass(d.device, &info, nullptr, clear ? &out.clear : &out.load),
                      "vkCreateRenderPass");
            }

            out.pipeline = create_scene_pipeline(d, out.clear, layout, vertex, fragment);
        } catch (...) {
            destroy(d, out);
            throw;
        }
        return out;
    }

    VkPipeline create_scene_pipeline(const Device& d, VkRenderPass pass, VkPipelineLayout layout, VkShaderModule vertex,
                                     VkShaderModule fragment, bool under) {
        const VkPipelineShaderStageCreateInfo stages[]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                                        VK_SHADER_STAGE_VERTEX_BIT, vertex, "vs_main", nullptr},
                                                       {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                                        VK_SHADER_STAGE_FRAGMENT_BIT, fragment, "ps_main", nullptr}};
        VkPipelineVertexInputStateCreateInfo vertices{};
        vertices.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        // Premultiplied source-over, as the D3D11 renderer blends; or destination-over.
        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = under ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = under ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = blend.srcColorBlendFactor;
        blend.dstAlphaBlendFactor = blend.dstColorBlendFactor;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        blend.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blending{};
        blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blending.attachmentCount = 1;
        blending.pAttachments = &blend;
        const VkDynamicState dynamic_states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = std::uint32_t(std::size(dynamic_states));
        dynamic.pDynamicStates = dynamic_states;
        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount = std::uint32_t(std::size(stages));
        info.pStages = stages;
        info.pVertexInputState = &vertices;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pColorBlendState = &blending;
        info.pDynamicState = &dynamic;
        info.layout = layout;
        info.renderPass = pass;
        VkPipeline out{};
        check(d.vkd.vkCreateGraphicsPipelines(d.device, VK_NULL_HANDLE, 1, &info, nullptr, &out),
              "vkCreateGraphicsPipelines");
        return out;
    }

    void destroy(const Device& d, ScenePasses& passes) noexcept {
        d.vkd.vkDestroyPipeline(d.device, passes.pipeline, nullptr);
        d.vkd.vkDestroyRenderPass(d.device, passes.clear, nullptr);
        d.vkd.vkDestroyRenderPass(d.device, passes.load, nullptr);
        passes = {};
    }

    VkDescriptorSetLayout create_scene_set_layout(const Device& d) {
        const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        const VkDescriptorSetLayoutBinding bindings[]{
            {std::uint32_t(limits::binding_view), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stages, nullptr},
            {std::uint32_t(limits::binding_records), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr},
            {std::uint32_t(limits::binding_image), VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, stages, nullptr},
            {std::uint32_t(limits::binding_clips), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr},
            {std::uint32_t(limits::binding_sampler), VK_DESCRIPTOR_TYPE_SAMPLER, 1, stages, nullptr},
            {std::uint32_t(limits::binding_effect_textures), VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
             VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {std::uint32_t(limits::binding_effect_textures) + 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
             VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {std::uint32_t(limits::binding_effect_textures) + 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
             VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {std::uint32_t(limits::binding_effect_textures) + 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
             VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {std::uint32_t(limits::binding_effect_sampler), VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
             nullptr},
            {std::uint32_t(limits::binding_order), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT,
             nullptr},
            {std::uint32_t(limits::binding_anchors), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr}};
        static_assert(weft::constants::max_effect_textures == 4);
        VkDescriptorSetLayoutCreateInfo set{};
        set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set.bindingCount = std::uint32_t(std::size(bindings));
        set.pBindings = bindings;
        VkDescriptorSetLayout out{};
        check(d.vkd.vkCreateDescriptorSetLayout(d.device, &set, nullptr, &out), "vkCreateDescriptorSetLayout");
        return out;
    }

    VkSampler create_linear_sampler(const Device& d, bool repeat) {
        VkSamplerCreateInfo sampler{};
        sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler.magFilter = VK_FILTER_LINEAR;
        sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        const auto mode = repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeU = mode;
        sampler.addressModeV = mode;
        sampler.addressModeW = mode;
        sampler.maxLod = VK_LOD_CLAMP_NONE;
        VkSampler out{};
        check(d.vkd.vkCreateSampler(d.device, &sampler, nullptr, &out), "vkCreateSampler");
        return out;
    }

    VkSampler create_mirror_sampler(const Device& d) {
        VkSamplerCreateInfo sampler{};
        sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler.magFilter = VK_FILTER_LINEAR;
        sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        sampler.maxLod = VK_LOD_CLAMP_NONE;
        VkSampler out{};
        check(d.vkd.vkCreateSampler(d.device, &sampler, nullptr, &out), "vkCreateSampler");
        return out;
    }

    VkDescriptorSetLayout create_backdrop_set_layout(const Device& d) {
        const VkDescriptorSetLayoutBinding bindings[]{
            {std::uint32_t(limits::binding_view), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
             nullptr},
            {std::uint32_t(limits::binding_records), VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
             nullptr},
            {std::uint32_t(limits::binding_sampler), VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
             nullptr}};
        VkDescriptorSetLayoutCreateInfo set{};
        set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set.bindingCount = std::uint32_t(std::size(bindings));
        set.pBindings = bindings;
        VkDescriptorSetLayout out{};
        check(d.vkd.vkCreateDescriptorSetLayout(d.device, &set, nullptr, &out), "vkCreateDescriptorSetLayout");
        return out;
    }

    VkDescriptorPool create_scene_pool(const Device& d, std::uint32_t capacity) {
        // Every set holds every binding of the layout, the effect's included: records, clips, order and anchors.
        const auto images = 1 + std::uint32_t(weft::constants::max_effect_textures);
        const VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, capacity},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * capacity},
                                           {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, images * capacity},
                                           {VK_DESCRIPTOR_TYPE_SAMPLER, 2 * capacity}};
        VkDescriptorPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        info.maxSets = capacity;
        info.poolSizeCount = std::uint32_t(std::size(sizes));
        info.pPoolSizes = sizes;
        VkDescriptorPool pool{};
        check(d.vkd.vkCreateDescriptorPool(d.device, &info, nullptr, &pool), "vkCreateDescriptorPool");
        return pool;
    }
} // namespace weft::render::vulkan
