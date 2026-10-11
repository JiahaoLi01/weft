#include "render/backends/vulkan/renderer.hpp"

#include "constants/common.hpp"
#include "constants/render.hpp"
#include "render/srgb.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace weft::render::vulkan {
    namespace {
        namespace limits = weft::constants::render::vulkan;

        // The shared scene shader's linear pass as SPIR-V (cmake/WeftSpirv.cmake).
        constexpr unsigned char vertex_spirv[] = {
#include "shaders/display_linear.vs.spv.inc"
        };
        constexpr unsigned char fragment_spirv[] = {
#include "shaders/display_linear.ps.spv.inc"
        };

        constexpr VkFormat output_format = VK_FORMAT_R8G8B8A8_SRGB;
        // Staging offsets keep every texel aligned for vkCmdCopyBufferToImage.
        constexpr VkDeviceSize staging_alignment = 16;

        VkShaderModule shader(const Device& d, const unsigned char* bytes, std::size_t size) {
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

        void transition(const Device& d, VkCommandBuffer command, VkImage image, VkImageLayout from, VkImageLayout to,
                        VkAccessFlags written, VkAccessFlags read, VkPipelineStageFlags before,
                        VkPipelineStageFlags after) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = written;
            barrier.dstAccessMask = read;
            barrier.oldLayout = from;
            barrier.newLayout = to;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            d.vkd.vkCmdPipelineBarrier(command, before, after, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }

        // An asset's texels as the renderer samples them: coverage and distances as R8, an opaque sRGB image as an sRGB
        // texture that decodes before filtering, a translucent one as linear premultiplied floats. The D3D11 renderer
        // converts those on its GPU with tables that reproduce this float arithmetic exactly.
        struct Texels {
            VkFormat format{};
            std::vector<std::uint8_t> bytes;
        };

        Texels texels(const paint::Asset& a) {
            if (a.encoding != paint::Encoding::SrgbStraight) {
                return {VK_FORMAT_R8_UNORM, a.bytes};
            }
            bool opaque = true;
            for (std::size_t p = 3; opaque && p < a.bytes.size(); p += weft::constants::rgba8_bytes_per_pixel) {
                opaque = a.bytes[p] == weft::constants::color_channel_max;
            }
            if (opaque) {
                return {VK_FORMAT_R8G8B8A8_SRGB, a.bytes};
            }
            const auto pixels = std::size_t(a.width) * a.height;
            std::vector<float> linear(pixels * weft::constants::rgba8_bytes_per_pixel);
            for (std::size_t i = 0; i < pixels; ++i) {
                const auto* p = a.bytes.data() + i * weft::constants::rgba8_bytes_per_pixel;
                const float alpha = p[3] / weft::constants::render::color_channel_max_float;
                for (unsigned c = 0; c < 3; ++c) {
                    linear[i * 4 + c] = srgb_to_linear(p[c] / weft::constants::render::color_channel_max_float) * alpha;
                }
                linear[i * 4 + 3] = alpha;
            }
            Texels out{VK_FORMAT_R32G32B32A32_SFLOAT, std::vector<std::uint8_t>(linear.size() * sizeof(float))};
            std::memcpy(out.bytes.data(), linear.data(), out.bytes.size());
            return out;
        }
    } // namespace

    Renderer::Renderer(Device& device, std::uint32_t w, std::uint32_t h) : gpu(device), width(w), height(h) {
        ensure(w && h && w <= unsigned(limits::max_surface_dimension) && h <= unsigned(limits::max_surface_dimension),
               "bounded Vulkan target");
        const auto& d = gpu;
        VkCommandPoolCreateInfo pool{};
        pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = d.queue_family;
        check(d.vkd.vkCreateCommandPool(d.device, &pool, nullptr, &pool_), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = pool_;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        check(d.vkd.vkAllocateCommandBuffers(d.device, &allocate, &command_), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fence{};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        check(d.vkd.vkCreateFence(d.device, &fence, nullptr, &fence_), "vkCreateFence");

        VkSamplerCreateInfo sampler{};
        sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler.magFilter = VK_FILTER_LINEAR;
        sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod = VK_LOD_CLAMP_NONE;
        check(d.vkd.vkCreateSampler(d.device, &sampler, nullptr, &sampler_), "vkCreateSampler");
        vertex_ = shader(d, vertex_spirv, sizeof(vertex_spirv));
        fragment_ = shader(d, fragment_spirv, sizeof(fragment_spirv));

        const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        const VkDescriptorSetLayoutBinding bindings[]{
            {std::uint32_t(limits::binding_view), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stages, nullptr},
            {std::uint32_t(limits::binding_records), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr},
            {std::uint32_t(limits::binding_image), VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, stages, nullptr},
            {std::uint32_t(limits::binding_clips), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr},
            {std::uint32_t(limits::binding_sampler), VK_DESCRIPTOR_TYPE_SAMPLER, 1, stages, nullptr}};
        VkDescriptorSetLayoutCreateInfo set{};
        set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set.bindingCount = std::uint32_t(std::size(bindings));
        set.pBindings = bindings;
        check(d.vkd.vkCreateDescriptorSetLayout(d.device, &set, nullptr, &set_layout_), "vkCreateDescriptorSetLayout");
        VkPipelineLayoutCreateInfo layout{};
        layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &set_layout_;
        check(d.vkd.vkCreatePipelineLayout(d.device, &layout, nullptr, &layout_), "vkCreatePipelineLayout");
        // Group layers are linear floats as in the D3D11 renderer; Vulkan only requires blending on 16-bit floats.
        VkFormatProperties wide{};
        d.vki.vkGetPhysicalDeviceFormatProperties(d.physical, VK_FORMAT_R32G32B32A32_SFLOAT, &wide);
        layer_format_ = wide.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT
                            ? VK_FORMAT_R32G32B32A32_SFLOAT
                            : VK_FORMAT_R16G16B16A16_SFLOAT;
        output_passes_ = passes(output_format);
        layer_passes_ = passes(layer_format_);

        output_ = image(output_format, w, h, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        framebuffer(output_, output_passes_);
        blank_image_ = image(VK_FORMAT_R8_UNORM, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        blank_buffer_ = buffer(sizeof(SceneClip), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        readback_ = buffer(VkDeviceSize(w) * h * sizeof(Pixel), VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        // The blank image is only bound, never sampled for a visible result; it needs a shader-readable layout.
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(d.vkd.vkBeginCommandBuffer(command_, &begin), "vkBeginCommandBuffer");
        transition(d, command_, blank_image_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   0, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        check(d.vkd.vkEndCommandBuffer(command_), "vkEndCommandBuffer");
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_;
        check(d.vkd.vkQueueSubmit(d.queue, 1, &submit, fence_), "vkQueueSubmit");
        d.wait(fence_);
        check(d.vkd.vkResetFences(d.device, 1, &fence_), "vkResetFences");
    }

    Renderer::~Renderer() {
        const auto& d = gpu;
        d.vkd.vkDeviceWaitIdle(d.device);
        destroy(readback_);
        destroy(blank_buffer_);
        destroy(blank_image_);
        for (auto& layer : layers_) {
            destroy(layer);
        }
        destroy(output_);
        for (auto* p : {&output_passes_, &layer_passes_}) {
            d.vkd.vkDestroyPipeline(d.device, p->pipeline, nullptr);
            d.vkd.vkDestroyRenderPass(d.device, p->clear, nullptr);
            d.vkd.vkDestroyRenderPass(d.device, p->load, nullptr);
        }
        d.vkd.vkDestroyPipelineLayout(d.device, layout_, nullptr);
        d.vkd.vkDestroyDescriptorSetLayout(d.device, set_layout_, nullptr);
        d.vkd.vkDestroyShaderModule(d.device, fragment_, nullptr);
        d.vkd.vkDestroyShaderModule(d.device, vertex_, nullptr);
        d.vkd.vkDestroySampler(d.device, sampler_, nullptr);
        d.vkd.vkDestroyFence(d.device, fence_, nullptr);
        d.vkd.vkDestroyCommandPool(d.device, pool_, nullptr);
    }

    Image Renderer::image(VkFormat format, std::uint32_t w, std::uint32_t h, VkImageUsageFlags usage) {
        const auto& d = gpu;
        Image out;
        out.format = format;
        out.width = w;
        out.height = h;
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {w, h, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        try {
            check(d.vkd.vkCreateImage(d.device, &info, nullptr, &out.image), "vkCreateImage");
            VkMemoryRequirements needs{};
            d.vkd.vkGetImageMemoryRequirements(d.device, out.image, &needs);
            VkMemoryAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocate.allocationSize = needs.size;
            allocate.memoryTypeIndex = d.memory_type(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            check(d.vkd.vkAllocateMemory(d.device, &allocate, nullptr, &out.memory), "vkAllocateMemory image");
            check(d.vkd.vkBindImageMemory(d.device, out.image, out.memory, 0), "vkBindImageMemory");
            VkImageViewCreateInfo view{};
            view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image = out.image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = format;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            check(d.vkd.vkCreateImageView(d.device, &view, nullptr, &out.view), "vkCreateImageView");
        } catch (...) {
            destroy(out);
            throw;
        }
        return out;
    }

    Buffer Renderer::buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool mapped) {
        const auto& d = gpu;
        Buffer out;
        out.size = size;
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        try {
            check(d.vkd.vkCreateBuffer(d.device, &info, nullptr, &out.buffer), "vkCreateBuffer");
            VkMemoryRequirements needs{};
            d.vkd.vkGetBufferMemoryRequirements(d.device, out.buffer, &needs);
            VkMemoryAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocate.allocationSize = needs.size;
            allocate.memoryTypeIndex =
                d.memory_type(needs.memoryTypeBits,
                              mapped ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                     : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            check(d.vkd.vkAllocateMemory(d.device, &allocate, nullptr, &out.memory), "vkAllocateMemory buffer");
            check(d.vkd.vkBindBufferMemory(d.device, out.buffer, out.memory, 0), "vkBindBufferMemory");
            if (mapped) {
                check(d.vkd.vkMapMemory(d.device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped), "vkMapMemory");
            }
        } catch (...) {
            destroy(out);
            throw;
        }
        return out;
    }

    void Renderer::destroy(Image& image) {
        const auto& d = gpu;
        if (image.framebuffer) {
            d.vkd.vkDestroyFramebuffer(d.device, image.framebuffer, nullptr);
        }
        if (image.view) {
            d.vkd.vkDestroyImageView(d.device, image.view, nullptr);
        }
        if (image.image) {
            d.vkd.vkDestroyImage(d.device, image.image, nullptr);
        }
        if (image.memory) {
            d.vkd.vkFreeMemory(d.device, image.memory, nullptr);
        }
        image = {};
    }

    void Renderer::destroy(Buffer& buffer) {
        const auto& d = gpu;
        if (buffer.mapped) {
            d.vkd.vkUnmapMemory(d.device, buffer.memory);
        }
        if (buffer.buffer) {
            d.vkd.vkDestroyBuffer(d.device, buffer.buffer, nullptr);
        }
        if (buffer.memory) {
            d.vkd.vkFreeMemory(d.device, buffer.memory, nullptr);
        }
        buffer = {};
    }

    Renderer::Passes Renderer::passes(VkFormat format) {
        const auto& d = gpu;
        Passes out;
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
            // Earlier writes of the target, of uploads and of group layers come first; later reads of the target as an
            // image or a copy source wait for its writes.
            const VkSubpassDependency dependencies[]{
                {VK_SUBPASS_EXTERNAL, 0,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
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

        const VkPipelineShaderStageCreateInfo stages[]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                                        VK_SHADER_STAGE_VERTEX_BIT, vertex_, "vs_main", nullptr},
                                                       {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                                        VK_SHADER_STAGE_FRAGMENT_BIT, fragment_, "ps_main", nullptr}};
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
        // Premultiplied source-over, as the D3D11 renderer blends.
        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
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
        info.layout = layout_;
        info.renderPass = out.clear;
        check(d.vkd.vkCreateGraphicsPipelines(d.device, VK_NULL_HANDLE, 1, &info, nullptr, &out.pipeline),
              "vkCreateGraphicsPipelines");
        return out;
    }

    void Renderer::framebuffer(Image& target, const Passes& passes) {
        const auto& d = gpu;
        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = passes.clear;
        info.attachmentCount = 1;
        info.pAttachments = &target.view;
        info.width = target.width;
        info.height = target.height;
        info.layers = 1;
        check(d.vkd.vkCreateFramebuffer(d.device, &info, nullptr, &target.framebuffer), "vkCreateFramebuffer");
    }

    std::vector<Pixel> Renderer::draw(const paint::DisplayList& list) {
        const auto& d = gpu;
        ensure(bool(paint::check(list, weft::constants::render::scene::max_group_depth)) && !list.external_textures,
               "a well-formed display list without external textures");
        for (const auto& item : list.items) {
            ensure(item.kind != paint::Item::Kind::Text && item.kind != paint::Item::Kind::Layer,
                   "the display list pass draws no scene rasters");
        }

        // Resources of this frame, freed once it completed or failed.
        struct Frame {
            Renderer& r;
            std::vector<Image> textures;
            Buffer staging;
            Buffer records;
            Buffer clips;
            Buffer view;
            VkDescriptorPool pool{};

            ~Frame() {
                for (auto& t : textures) {
                    r.destroy(t);
                }
                r.destroy(staging);
                r.destroy(records);
                r.destroy(clips);
                r.destroy(view);
                if (pool) {
                    r.gpu.vkd.vkDestroyDescriptorPool(r.gpu.device, pool, nullptr);
                }
            }
        } frame{*this};

        // Textures and their staged texels.
        std::vector<Texels> sources;
        std::vector<VkDeviceSize> offsets;
        VkDeviceSize staged{};
        for (const auto& asset : list.assets) {
            sources.push_back(texels(*asset));
            offsets.push_back(staged);
            staged += (sources.back().bytes.size() + staging_alignment - 1) / staging_alignment * staging_alignment;
        }
        if (staged) {
            frame.staging = buffer(staged, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
            for (std::size_t i = 0; i < sources.size(); ++i) {
                std::memcpy(static_cast<std::uint8_t*>(frame.staging.mapped) + offsets[i], sources[i].bytes.data(),
                            sources[i].bytes.size());
            }
        }
        for (std::size_t i = 0; i < sources.size(); ++i) {
            frame.textures.push_back(image(sources[i].format, list.assets[i]->width, list.assets[i]->height,
                                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT));
        }

        // The pass; images of one asset draw together.
        std::vector<std::uint64_t> names(list.assets.size());
        for (std::size_t i = 0; i < names.size(); ++i) {
            names[i] = i;
        }
        auto planned = plan_scene_pass(list, 1, width, height, std::nullopt, Space::Linear, &names);
        ensure(bool(planned), "display list pass plan");
        const auto& pass = *planned;
        if (!pass.records.empty()) {
            frame.records = buffer(pass.records.size() * sizeof(SceneRecord), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
            std::memcpy(frame.records.mapped, pass.records.data(), pass.records.size() * sizeof(SceneRecord));
        }
        if (!pass.clips.empty()) {
            frame.clips = buffer(pass.clips.size() * sizeof(SceneClip), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
            std::memcpy(frame.clips.mapped, pass.clips.data(), pass.clips.size() * sizeof(SceneClip));
        }
        // The View constants: target size, scale and the base instance, which Vulkan's instance index already holds.
        const std::array<float, 4> view{float(width), float(height), 1, 0};
        frame.view = buffer(sizeof(view), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        std::memcpy(frame.view.mapped, view.data(), sizeof(view));
        std::uint32_t sets{};
        for (const auto& batch : pass.batches) {
            sets += batch.step != ScenePass::Step::BeginGroup;
        }
        ensure(sets <= unsigned(limits::max_pass_descriptor_sets), "descriptor sets of one pass");
        if (sets) {
            const VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, sets},
                                               {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * sets},
                                               {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sets},
                                               {VK_DESCRIPTOR_TYPE_SAMPLER, sets}};
            VkDescriptorPoolCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            info.maxSets = sets;
            info.poolSizeCount = std::uint32_t(std::size(sizes));
            info.pPoolSizes = sizes;
            check(d.vkd.vkCreateDescriptorPool(d.device, &info, nullptr, &frame.pool), "vkCreateDescriptorPool");
        }

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(d.vkd.vkBeginCommandBuffer(command_, &begin_info), "vkBeginCommandBuffer");
        for (std::size_t i = 0; i < frame.textures.size(); ++i) {
            const auto& t = frame.textures[i];
            transition(d, command_, t.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{};
            copy.bufferOffset = offsets[i];
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {t.width, t.height, 1};
            d.vkd.vkCmdCopyBufferToImage(command_, frame.staging.buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         1, &copy);
            transition(d, command_, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }

        auto start = [&](Image& target, bool clear, const paint::Color& color) {
            const auto& p = target.format == output_format ? output_passes_ : layer_passes_;
            VkClearValue value{};
            std::copy(color.begin(), color.end(), value.color.float32);
            VkRenderPassBeginInfo info{};
            info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            info.renderPass = clear ? p.clear : p.load;
            info.framebuffer = target.framebuffer;
            info.renderArea = {{0, 0}, {target.width, target.height}};
            info.clearValueCount = clear ? 1u : 0u;
            info.pClearValues = &value;
            d.vkd.vkCmdBeginRenderPass(command_, &info, VK_SUBPASS_CONTENTS_INLINE);
            d.vkd.vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
            const VkViewport viewport{0, 0, float(target.width), float(target.height), 0, 1};
            d.vkd.vkCmdSetViewport(command_, 0, 1, &viewport);
            const VkRect2D scissor{{0, 0}, {target.width, target.height}};
            d.vkd.vkCmdSetScissor(command_, 0, 1, &scissor);
        };
        auto draw_records = [&](std::uint32_t first, std::uint32_t count, VkImageView image) {
            VkDescriptorSetAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocate.descriptorPool = frame.pool;
            allocate.descriptorSetCount = 1;
            allocate.pSetLayouts = &set_layout_;
            VkDescriptorSet set{};
            check(d.vkd.vkAllocateDescriptorSets(d.device, &allocate, &set), "vkAllocateDescriptorSets");
            const VkDescriptorBufferInfo view_info{frame.view.buffer, 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo record_info{frame.records.buffer, 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo clip_info{frame.clips.buffer ? frame.clips.buffer : blank_buffer_.buffer, 0,
                                                   VK_WHOLE_SIZE};
            const VkDescriptorImageInfo image_info{VK_NULL_HANDLE, image ? image : blank_image_.view,
                                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            const VkDescriptorImageInfo sampler_info{sampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
            VkWriteDescriptorSet writes[5]{};
            const std::uint32_t bindings[]{std::uint32_t(limits::binding_view), std::uint32_t(limits::binding_records),
                                           std::uint32_t(limits::binding_image), std::uint32_t(limits::binding_clips),
                                           std::uint32_t(limits::binding_sampler)};
            const VkDescriptorType types[]{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                           VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                           VK_DESCRIPTOR_TYPE_SAMPLER};
            for (unsigned i = 0; i < std::size(writes); ++i) {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = set;
                writes[i].dstBinding = bindings[i];
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = types[i];
            }
            writes[0].pBufferInfo = &view_info;
            writes[1].pBufferInfo = &record_info;
            writes[2].pImageInfo = &image_info;
            writes[3].pBufferInfo = &clip_info;
            writes[4].pImageInfo = &sampler_info;
            d.vkd.vkUpdateDescriptorSets(d.device, std::uint32_t(std::size(writes)), writes, 0, nullptr);
            d.vkd.vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0, nullptr);
            d.vkd.vkCmdDraw(command_, std::uint32_t(limits::quad_vertex_count), count, 0, first);
            ++draws;
            instances += count;
        };

        // The target, then one layer per open group. Every layer the pass nests exists before targets point at them.
        while (layers_.size() < pass.group_depth) {
            layers_.push_back(
                image(layer_format_, width, height, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));
            framebuffer(layers_.back(), layer_passes_);
        }
        std::vector<Image*> targets{&output_};
        start(output_, true, pass.clear);
        for (const auto& batch : pass.batches) {
            switch (batch.step) {
                case ScenePass::Step::Boxes:
                    draw_records(batch.first, batch.count, VK_NULL_HANDLE);
                    break;
                case ScenePass::Step::Image:
                    draw_records(batch.first, batch.count, frame.textures.at(batch.source).view);
                    break;
                case ScenePass::Step::BeginGroup:
                    d.vkd.vkCmdEndRenderPass(command_);
                    targets.push_back(&layers_.at(targets.size() - 1));
                    start(*targets.back(), true, paint::Color{});
                    break;
                case ScenePass::Step::EndGroup: {
                    d.vkd.vkCmdEndRenderPass(command_);
                    auto* layer = targets.back();
                    targets.pop_back();
                    transition(d, command_, layer->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                               VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                    start(*targets.back(), false, paint::Color{});
                    draw_records(batch.first, 1, layer->view);
                    break;
                }
            }
        }
        d.vkd.vkCmdEndRenderPass(command_);
        transition(d, command_, output_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        d.vkd.vkCmdCopyImageToBuffer(command_, output_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.buffer, 1,
                                     &copy);
        check(d.vkd.vkEndCommandBuffer(command_), "vkEndCommandBuffer");
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_;
        check(d.vkd.vkQueueSubmit(d.queue, 1, &submit, fence_), "vkQueueSubmit");
        d.wait(fence_);
        check(d.vkd.vkResetFences(d.device, 1, &fence_), "vkResetFences");
        std::vector<Pixel> pixels(std::size_t(width) * height);
        std::memcpy(pixels.data(), readback_.mapped, pixels.size() * sizeof(Pixel));
        return pixels;
    }
} // namespace weft::render::vulkan
