#include "render/backends/vulkan/renderer.hpp"

#include "constants/common.hpp"
#include "constants/render.hpp"
#include "render/srgb.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

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

        // The renderer's own target, which draw() reads back.
        constexpr VkFormat output_format = VK_FORMAT_R8G8B8A8_SRGB;
        // Staging offsets keep every texel aligned for vkCmdCopyBufferToImage.
        constexpr VkDeviceSize staging_alignment = 16;

        void lifetime(LifetimeResult result, const char* operation) {
            if (result != LifetimeResult::Ok) {
                throw std::runtime_error(std::string(operation) + " lifetime=" + std::to_string(int(result)));
            }
        }

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

        VkImageMemoryBarrier barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags written,
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
            const auto b = barrier(image, from, to, written, read);
            d.vkd.vkCmdPipelineBarrier(command, before, after, 0, 0, nullptr, 0, nullptr, 1, &b);
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

    FrameResources::FrameResources(Renderer& renderer) : renderer_(&renderer) {
        ++renderer.frames_created;
    }

    FrameResources::FrameResources(FrameResources&& other) noexcept
        : staging(std::exchange(other.staging, {})),
          records(std::exchange(other.records, {})),
          clips(std::exchange(other.clips, {})),
          view(std::exchange(other.view, {})),
          pool(std::exchange(other.pool, {})),
          framebuffer(std::exchange(other.framebuffer, {})),
          renderer_(std::exchange(other.renderer_, nullptr)) {
    }

    FrameResources& FrameResources::operator=(FrameResources&& other) noexcept {
        if (this != &other) {
            release();
            staging = std::exchange(other.staging, {});
            records = std::exchange(other.records, {});
            clips = std::exchange(other.clips, {});
            view = std::exchange(other.view, {});
            pool = std::exchange(other.pool, {});
            framebuffer = std::exchange(other.framebuffer, {});
            renderer_ = std::exchange(other.renderer_, nullptr);
        }
        return *this;
    }

    FrameResources::~FrameResources() {
        release();
    }

    void FrameResources::release() noexcept {
        if (!renderer_) {
            return;
        }
        const auto& d = renderer_->gpu;
        d.destroy(staging);
        d.destroy(records);
        d.destroy(clips);
        d.destroy(view);
        if (pool) {
            d.vkd.vkDestroyDescriptorPool(d.device, pool, nullptr);
            pool = {};
        }
        if (framebuffer) {
            d.vkd.vkDestroyFramebuffer(d.device, framebuffer, nullptr);
            framebuffer = {};
        }
        ++renderer_->frames_released;
        renderer_ = nullptr;
    }

    Renderer::Renderer(Device& device, std::uint32_t w, std::uint32_t h)
        : gpu(device),
          width(w),
          height(h),
          tracker({std::uint64_t(limits::device_epoch), std::uint64_t(limits::queue)}) {
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
        layer_format_ =
            d.blends(VK_FORMAT_R32G32B32A32_SFLOAT) ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R16G16B16A16_SFLOAT;
        passes(output_format);
        passes(layer_format_);

        output_ = d.image(output_format, w, h, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        blank_image_ = d.image(VK_FORMAT_R8_UNORM, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        blank_buffer_ = d.buffer(sizeof(SceneClip), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        readback_ = d.buffer(VkDeviceSize(w) * h * sizeof(Pixel), VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        // The blank image is only bound, never sampled for a visible result; it needs a shader-readable layout. This one
        // submission precedes every frame on the device's queue.
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
        for (auto& a : assets_) {
            d.destroy(a->image);
        }
        d.destroy(readback_);
        d.destroy(blank_buffer_);
        d.destroy(blank_image_);
        for (auto& layer : layers_) {
            d.destroy(layer);
        }
        d.destroy(output_);
        for (auto& [format, p] : passes_) {
            d.vkd.vkDestroyPipeline(d.device, p.pipeline, nullptr);
            d.vkd.vkDestroyRenderPass(d.device, p.clear, nullptr);
            d.vkd.vkDestroyRenderPass(d.device, p.load, nullptr);
        }
        d.vkd.vkDestroyPipelineLayout(d.device, layout_, nullptr);
        d.vkd.vkDestroyDescriptorSetLayout(d.device, set_layout_, nullptr);
        d.vkd.vkDestroyShaderModule(d.device, fragment_, nullptr);
        d.vkd.vkDestroyShaderModule(d.device, vertex_, nullptr);
        d.vkd.vkDestroySampler(d.device, sampler_, nullptr);
        d.vkd.vkDestroyFence(d.device, fence_, nullptr);
        d.vkd.vkDestroyCommandPool(d.device, pool_, nullptr);
    }

    const Renderer::Passes& Renderer::passes(VkFormat format) {
        if (const auto found = passes_.find(format); found != passes_.end()) {
            return found->second;
        }
        ensure(gpu.blends(format), "a target format the device blends");
        auto created = create_passes(format);
        try {
            return passes_.emplace(format, created).first->second;
        } catch (...) {
            const auto& d = gpu;
            d.vkd.vkDestroyPipeline(d.device, created.pipeline, nullptr);
            d.vkd.vkDestroyRenderPass(d.device, created.clear, nullptr);
            d.vkd.vkDestroyRenderPass(d.device, created.load, nullptr);
            throw;
        }
    }

    Renderer::Passes Renderer::create_passes(VkFormat format) {
        const auto& d = gpu;
        Passes out;
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

            const VkPipelineShaderStageCreateInfo stages[]{
                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertex_,
                 "vs_main", nullptr},
                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
                 fragment_, "ps_main", nullptr}};
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
            blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                                   VK_COLOR_COMPONENT_A_BIT;
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
        } catch (...) {
            d.vkd.vkDestroyPipeline(d.device, out.pipeline, nullptr);
            d.vkd.vkDestroyRenderPass(d.device, out.clear, nullptr);
            d.vkd.vkDestroyRenderPass(d.device, out.load, nullptr);
            throw;
        }
        return out;
    }

    VkFramebuffer Renderer::framebuffer(VkImageView view, VkFormat format) {
        const auto& d = gpu;
        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = passes(format).clear;
        info.attachmentCount = 1;
        info.pAttachments = &view;
        info.width = width;
        info.height = height;
        info.layers = 1;
        VkFramebuffer out{};
        check(d.vkd.vkCreateFramebuffer(d.device, &info, nullptr, &out), "vkCreateFramebuffer");
        return out;
    }

    PreparedFrame Renderer::prepare(std::shared_ptr<const paint::DisplayList> list) {
        ensure(list && bool(paint::check(*list, weft::constants::render::scene::max_group_depth)) &&
                   !list->external_textures,
               "a well-formed display list without external textures");
        for (const auto& item : list->items) {
            ensure(item.kind != paint::Item::Kind::Text && item.kind != paint::Item::Kind::Layer,
                   "the display list pass draws no scene rasters");
            if (item.kind == paint::Item::Kind::Image) {
                const auto& image = list->images[item.payload];
                ensure(image.stroke >= 0 && (image.asset >= list->assets.size() ||
                                             list->assets[image.asset]->encoding != paint::Encoding::SignedDistance ||
                                             image.stroke < list->assets[image.asset]->spread - 1),
                       "SDF stroke exceeds distance padding");
            }
        }
        PreparedFrame frame;
        frame.list = std::move(list);
        const auto& sources = frame.list->assets;
        // Every texture exists and is leased before the frame pins them; a failure destroys the ones created.
        std::vector<std::unique_ptr<NativeAsset>> created;
        try {
            created.reserve(sources.size());
            frame.assets.reserve(sources.size());
            assets_.reserve(assets_.size() + sources.size());
            std::vector<ResourceId> dependencies;
            dependencies.reserve(sources.size());
            for (const auto& source : sources) {
                const auto& a = *source;
                const bool r8 = a.encoding != paint::Encoding::SrgbStraight;
                ensure(a.width && a.height && a.width <= unsigned(limits::max_surface_dimension) &&
                           a.height <= unsigned(limits::max_surface_dimension) &&
                           a.bytes.size() ==
                               std::size_t(a.width) * a.height * (r8 ? 1 : weft::constants::rgba8_bytes_per_pixel),
                       "asset dimensions/encoding");
                if (a.encoding == paint::Encoding::SignedDistance) {
                    ensure(a.spread >= weft::constants::render::min_sdf_spread &&
                               a.spread <= weft::constants::render::max_sdf_spread,
                           "distance spread metadata");
                }
                auto source_texels = texels(a);
                auto native = std::make_unique<NativeAsset>();
                native->image = gpu.image(source_texels.format, a.width, a.height,
                                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
                native->texels = std::move(source_texels.bytes);
                ++native_creations;
                const auto result = tracker.CreateResource(
                    {next_asset_id_++, 1, native->texels.size(), ResourceKind::Texture}, native->id);
                if (result != LifetimeResult::Ok) {
                    gpu.destroy(native->image);
                    ++native_releases;
                    lifetime(result, "CreateResource");
                }
                dependencies.push_back(native->id);
                created.push_back(std::move(native));
            }
            lifetime(tracker.PrepareFrame(dependencies, frame.id), "PrepareFrame");
        } catch (...) {
            for (auto& a : created) {
                if (tracker.ReleaseResource(a->id) != LifetimeResult::Ok) {
                    std::terminate();
                }
                gpu.destroy(a->image);
                ++native_releases;
            }
            throw;
        }
        for (auto& a : created) {
            frame.assets.push_back(a.get());
            assets_.push_back(std::move(a));
        }
        return frame;
    }

    void Renderer::release(const PreparedFrame& frame) {
        lifetime(tracker.ReleasePreparedFrame(frame.id), "ReleasePreparedFrame");
        for (const auto* a : frame.assets) {
            lifetime(tracker.ReleaseResource(a->id), "release a texture's frame lease");
        }
        collect();
    }

    void Renderer::collect() {
        std::erase_if(assets_, [&](const std::unique_ptr<NativeAsset>& a) {
            ResourceInfo info;
            const auto state = tracker.InspectResource(a->id, info);
            if (state == LifetimeResult::StaleHandle) {
                gpu.destroy(a->image);
                ++native_releases;
                return true;
            }
            if (state == LifetimeResult::Ok &&
                (info.phase == ResourcePhase::Submitted || info.phase == ResourcePhase::Ready)) {
                std::vector<std::uint8_t>{}.swap(a->texels);
            }
            return false;
        });
    }

    Encoded Renderer::encode(const PreparedFrame& frame, VkCommandBuffer command, const Target& target, bool clear,
                             const paint::Color& clear_color) {
        const auto& d = gpu;
        Encoded encoded;
        lifetime(tracker.EncodeFrame(frame.id, encoded.token), "EncodeFrame before recording");
        try {
            encoded.resources = FrameResources(*this);
            auto& resources = encoded.resources;
            const auto& list = *frame.list;

            // Uploads of the textures that no submitted use uploaded, staged in this frame's buffer.
            std::vector<VkImageMemoryBarrier> to_transfer;
            std::vector<VkImageMemoryBarrier> to_read;
            std::vector<VkBufferImageCopy> copies;
            std::vector<const NativeAsset*> pending;
            VkDeviceSize staged{};
            for (const auto* a : frame.assets) {
                ResourceInfo info;
                lifetime(tracker.InspectResource(a->id, info), "inspect upload readiness");
                if (info.phase == ResourcePhase::Ready || info.phase == ResourcePhase::Submitted) {
                    continue;
                }
                ensure(!a->texels.empty(), "an unsubmitted texture keeps its texels");
                VkBufferImageCopy copy{};
                copy.bufferOffset = staged;
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageExtent = {a->image.width, a->image.height, 1};
                copies.push_back(copy);
                pending.push_back(a);
                // Another use of this frame, encoded earlier and submitted before this one, may still read or write the
                // texture: the upload waits for those accesses.
                to_transfer.push_back(barrier(a->image.image, VK_IMAGE_LAYOUT_UNDEFINED,
                                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                              VK_ACCESS_TRANSFER_WRITE_BIT));
                to_read.push_back(barrier(a->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                          VK_ACCESS_SHADER_READ_BIT));
                staged += (a->texels.size() + staging_alignment - 1) / staging_alignment * staging_alignment;
            }
            if (staged) {
                resources.staging = d.buffer(staged, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
                for (std::size_t i = 0; i < pending.size(); ++i) {
                    std::memcpy(static_cast<std::uint8_t*>(resources.staging.mapped) + copies[i].bufferOffset,
                                pending[i]->texels.data(), pending[i]->texels.size());
                }
            }

            // The pass; images of one texture draw together.
            std::vector<std::uint64_t> names(frame.assets.size());
            for (std::size_t i = 0; i < names.size(); ++i) {
                names[i] = i;
            }
            auto planned = plan_scene_pass(list, 1, width, height, std::nullopt, Space::Linear, &names);
            ensure(bool(planned), "display list pass plan");
            const auto& pass = *planned;
            if (!pass.records.empty()) {
                resources.records =
                    d.buffer(pass.records.size() * sizeof(SceneRecord), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
                std::memcpy(resources.records.mapped, pass.records.data(), pass.records.size() * sizeof(SceneRecord));
            }
            if (!pass.clips.empty()) {
                resources.clips =
                    d.buffer(pass.clips.size() * sizeof(SceneClip), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
                std::memcpy(resources.clips.mapped, pass.clips.data(), pass.clips.size() * sizeof(SceneClip));
            }

            // Targets: the frame's, then one layer per open group.
            const auto& target_passes = passes(target.format);
            const auto& layer_passes = passes(layer_format_);
            resources.framebuffer = framebuffer(target.view, target.format);
            layers_.reserve(pass.group_depth);
            while (layers_.size() < pass.group_depth) {
                auto layer = d.image(layer_format_, width, height,
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
                try {
                    layer.framebuffer = framebuffer(layer.view, layer_format_);
                } catch (...) {
                    d.destroy(layer);
                    throw;
                }
                layers_.push_back(layer);
            }

            // One descriptor set per draw: the image it samples, or the layer a group composites.
            std::vector<VkImageView> images;
            std::size_t open{};
            for (const auto& batch : pass.batches) {
                switch (batch.step) {
                    case ScenePass::Step::Boxes:
                        images.push_back(blank_image_.view);
                        break;
                    case ScenePass::Step::Image:
                        images.push_back(frame.assets.at(batch.source)->image.view);
                        break;
                    case ScenePass::Step::BeginGroup:
                        ++open;
                        break;
                    case ScenePass::Step::EndGroup:
                        images.push_back(layers_.at(--open).view);
                        break;
                }
            }
            std::vector<VkDescriptorSet> sets(images.size());
            ensure(sets.size() <= unsigned(limits::max_pass_descriptor_sets), "descriptor sets of one pass");
            if (!sets.empty()) {
                // The View constants: target size, scale and the base instance, which Vulkan's instance index holds.
                const std::array<float, 4> view{float(width), float(height), 1, 0};
                resources.view = d.buffer(sizeof(view), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
                std::memcpy(resources.view.mapped, view.data(), sizeof(view));
                const auto count = std::uint32_t(sets.size());
                const VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, count},
                                                   {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * count},
                                                   {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, count},
                                                   {VK_DESCRIPTOR_TYPE_SAMPLER, count}};
                VkDescriptorPoolCreateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
                info.maxSets = count;
                info.poolSizeCount = std::uint32_t(std::size(sizes));
                info.pPoolSizes = sizes;
                check(d.vkd.vkCreateDescriptorPool(d.device, &info, nullptr, &resources.pool),
                      "vkCreateDescriptorPool");
                const std::vector<VkDescriptorSetLayout> layouts(sets.size(), set_layout_);
                VkDescriptorSetAllocateInfo allocate{};
                allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                allocate.descriptorPool = resources.pool;
                allocate.descriptorSetCount = count;
                allocate.pSetLayouts = layouts.data();
                check(d.vkd.vkAllocateDescriptorSets(d.device, &allocate, sets.data()), "vkAllocateDescriptorSets");
                const VkDescriptorBufferInfo view_info{resources.view.buffer, 0, VK_WHOLE_SIZE};
                const VkDescriptorBufferInfo record_info{resources.records.buffer, 0, VK_WHOLE_SIZE};
                const VkDescriptorBufferInfo clip_info{
                    resources.clips.buffer ? resources.clips.buffer : blank_buffer_.buffer, 0, VK_WHOLE_SIZE};
                const VkDescriptorImageInfo sampler_info{sampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
                const std::uint32_t bindings[]{
                    std::uint32_t(limits::binding_view), std::uint32_t(limits::binding_records),
                    std::uint32_t(limits::binding_image), std::uint32_t(limits::binding_clips),
                    std::uint32_t(limits::binding_sampler)};
                const VkDescriptorType types[]{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                               VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                               VK_DESCRIPTOR_TYPE_SAMPLER};
                for (std::size_t s = 0; s < sets.size(); ++s) {
                    const VkDescriptorImageInfo image_info{VK_NULL_HANDLE, images[s],
                                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                    VkWriteDescriptorSet writes[5]{};
                    for (unsigned i = 0; i < std::size(writes); ++i) {
                        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                        writes[i].dstSet = sets[s];
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
                }
            }
            if (inject_encode_failure) {
                throw std::runtime_error("injected encode failure");
            }

            // Recording: everything it uses exists, and nothing below fails.
            const bool clear_target = clear || pass.clear != paint::Color{};
            const auto& first_color = pass.clear != paint::Color{} ? pass.clear : clear_color;
            [&]() noexcept {
                if (!copies.empty()) {
                    const auto n = std::uint32_t(copies.size());
                    d.vkd.vkCmdPipelineBarrier(
                        command, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, n, to_transfer.data());
                    for (std::size_t i = 0; i < copies.size(); ++i) {
                        d.vkd.vkCmdCopyBufferToImage(command, resources.staging.buffer, pending[i]->image.image,
                                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copies[i]);
                    }
                    d.vkd.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, n,
                                               to_read.data());
                    uploads += copies.size();
                }
                auto begin = [&](VkFramebuffer target_framebuffer, const Passes& p, bool clear_it,
                                 const paint::Color& color) {
                    VkClearValue value{};
                    std::copy(color.begin(), color.end(), value.color.float32);
                    VkRenderPassBeginInfo info{};
                    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                    info.renderPass = clear_it ? p.clear : p.load;
                    info.framebuffer = target_framebuffer;
                    info.renderArea = {{0, 0}, {width, height}};
                    info.clearValueCount = clear_it ? 1u : 0u;
                    info.pClearValues = &value;
                    d.vkd.vkCmdBeginRenderPass(command, &info, VK_SUBPASS_CONTENTS_INLINE);
                    d.vkd.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
                    const VkViewport viewport{0, 0, float(width), float(height), 0, 1};
                    d.vkd.vkCmdSetViewport(command, 0, 1, &viewport);
                    const VkRect2D scissor{{0, 0}, {width, height}};
                    d.vkd.vkCmdSetScissor(command, 0, 1, &scissor);
                };
                std::size_t next_set{};
                auto draw_records = [&](std::uint32_t first, std::uint32_t count) {
                    d.vkd.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1,
                                                  &sets[next_set++], 0, nullptr);
                    d.vkd.vkCmdDraw(command, std::uint32_t(limits::quad_vertex_count), count, 0, first);
                    ++draws;
                    instances += count;
                };
                begin(resources.framebuffer, target_passes, clear_target, first_color);
                std::size_t depth{};
                for (const auto& batch : pass.batches) {
                    switch (batch.step) {
                        case ScenePass::Step::Boxes:
                        case ScenePass::Step::Image:
                            draw_records(batch.first, batch.count);
                            break;
                        case ScenePass::Step::BeginGroup:
                            d.vkd.vkCmdEndRenderPass(command);
                            begin(layers_[depth++].framebuffer, layer_passes, true, paint::Color{});
                            break;
                        case ScenePass::Step::EndGroup:
                            d.vkd.vkCmdEndRenderPass(command);
                            transition(d, command, layers_[--depth].image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                       VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                            begin(depth ? layers_[depth - 1].framebuffer : resources.framebuffer,
                                  depth ? layer_passes : target_passes, false, paint::Color{});
                            draw_records(batch.first, 1);
                            break;
                    }
                }
                d.vkd.vkCmdEndRenderPass(command);
            }();
            ++recordings;
            return encoded;
        } catch (...) {
            // Nothing was recorded: the use ends here, and its resources with encoded.
            if (tracker.DiscardEncoded(encoded.token) != LifetimeResult::Ok) {
                std::terminate();
            }
            throw;
        }
    }

    std::vector<Pixel> Renderer::draw(const paint::DisplayList& list) {
        const auto& d = gpu;
        const auto stats = tracker.Stats();
        ensure(!stats.prepared_frames && !stats.encoded_tokens && !stats.submitted_tokens,
               "draw() numbers the renderer's submissions itself");
        auto frame = prepare(std::make_shared<const paint::DisplayList>(list));
        Encoded encoded;
        try {
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check(d.vkd.vkBeginCommandBuffer(command_, &begin), "vkBeginCommandBuffer");
            // The previous frame's readback copied the target.
            transition(d, command_, output_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       0, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            encoded = encode(frame, command_, {output_.view, output_format}, true, paint::Color{});
        } catch (...) {
            d.vkd.vkResetCommandBuffer(command_, 0);
            release(frame);
            throw;
        }
        release(frame);
        transition(d, command_, output_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        d.vkd.vkCmdCopyImageToBuffer(command_, output_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.buffer, 1,
                                     &copy);
        VkBufferMemoryBarrier host{};
        host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = readback_.buffer;
        host.size = VK_WHOLE_SIZE;
        d.vkd.vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr,
                                   1, &host, 0, nullptr);
        const auto serial = std::max(stats.last_submitted_serial, stats.completed_serial) + 1;
        try {
            check(d.vkd.vkEndCommandBuffer(command_), "vkEndCommandBuffer");
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &command_;
            check(d.vkd.vkQueueSubmit(d.queue, 1, &submit, fence_), "vkQueueSubmit");
        } catch (...) {
            d.vkd.vkResetCommandBuffer(command_, 0);
            lifetime(tracker.DiscardEncoded(encoded.token), "discard an unsubmitted frame");
            encoded.resources = {};
            collect();
            throw;
        }
        lifetime(tracker.NotifySubmitted(encoded.token, std::uint64_t(limits::queue), serial), "NotifySubmitted");
        try {
            d.wait(fence_);
        } catch (...) {
            // The frame's resources may still be in use.
            d.vkd.vkDeviceWaitIdle(d.device);
            throw;
        }
        check(d.vkd.vkResetFences(d.device, 1, &fence_), "vkResetFences");
        lifetime(
            tracker.NotifyCompletedSerial(std::uint64_t(limits::device_epoch), std::uint64_t(limits::queue), serial),
            "NotifyCompletedSerial");
        encoded.resources = {};
        collect();
        std::vector<Pixel> pixels(std::size_t(width) * height);
        std::memcpy(pixels.data(), readback_.mapped, pixels.size() * sizeof(Pixel));
        return pixels;
    }
} // namespace weft::render::vulkan
