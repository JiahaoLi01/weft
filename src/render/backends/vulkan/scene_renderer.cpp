#include "render/backends/vulkan/scene_renderer.hpp"

#include "constants/common.hpp"
#include "constants/render.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <unordered_set>
#include <utility>

namespace weft::render::vulkan {
    namespace {
        namespace limits = weft::constants::render::vulkan;

        // The shared scene shader's encoded pass as SPIR-V (cmake/WeftSpirv.cmake).
        constexpr unsigned char vertex_spirv[] = {
#include "shaders/scene_encoded.vs.spv.inc"
        };
        constexpr unsigned char fragment_spirv[] = {
#include "shaders/scene_encoded.ps.spv.inc"
        };

        // Encoded premultiplied BGRA, as the D3D11 scene targets.
        constexpr VkFormat target_format = VK_FORMAT_B8G8R8A8_UNORM;
        // Staging offsets keep every texel aligned for vkCmdCopyBufferToImage.
        constexpr VkDeviceSize staging_alignment = 16;
    } // namespace

    SceneRenderer::SceneRenderer(Device& device, SceneText& text)
        : gpu(device), preparer_(text, *this, std::uint32_t(limits::max_surface_dimension)) {
        const auto& d = gpu;
        try {
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
            sampler_ = create_linear_sampler(d);
            vertex_ = shader_module(d, vertex_spirv, sizeof(vertex_spirv));
            fragment_ = shader_module(d, fragment_spirv, sizeof(fragment_spirv));
            set_layout_ = create_scene_set_layout(d);
            VkPipelineLayoutCreateInfo layout{};
            layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            layout.setLayoutCount = 1;
            layout.pSetLayouts = &set_layout_;
            check(d.vkd.vkCreatePipelineLayout(d.device, &layout, nullptr, &layout_), "vkCreatePipelineLayout");
            ensure(d.blends(target_format), "encoded BGRA8 targets that blend");
            passes_ = create_scene_passes(d, target_format, layout_, vertex_, fragment_);
            blank_image_ =
                d.image(VK_FORMAT_R8_UNORM, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            blank_buffer_ = d.buffer(sizeof(SceneClip), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
            // The blank image is only bound, never sampled for a visible result; it needs a shader-readable layout.
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check(d.vkd.vkBeginCommandBuffer(command_, &begin), "vkBeginCommandBuffer");
            transition(d, command_, blank_image_.image, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT,
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            check(d.vkd.vkEndCommandBuffer(command_), "vkEndCommandBuffer");
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &command_;
            check(d.vkd.vkQueueSubmit(d.queue, 1, &submit, fence_), "vkQueueSubmit");
            d.wait(fence_);
            check(d.vkd.vkResetFences(d.device, 1, &fence_), "vkResetFences");
        } catch (...) {
            destroy_all();
            throw;
        }
    }

    SceneRenderer::~SceneRenderer() {
        destroy_all();
    }

    void SceneRenderer::destroy_all() noexcept {
        const auto& d = gpu;
        d.vkd.vkDeviceWaitIdle(d.device);
        for (auto& [id, texture] : textures_) {
            d.destroy(texture.image);
        }
        textures_.clear();
        d.destroy(readback_);
        d.destroy(target_);
        d.destroy(blank_buffer_);
        d.destroy(blank_image_);
        destroy(d, passes_);
        d.vkd.vkDestroyPipelineLayout(d.device, layout_, nullptr);
        d.vkd.vkDestroyDescriptorSetLayout(d.device, set_layout_, nullptr);
        d.vkd.vkDestroyShaderModule(d.device, fragment_, nullptr);
        d.vkd.vkDestroyShaderModule(d.device, vertex_, nullptr);
        d.vkd.vkDestroySampler(d.device, sampler_, nullptr);
        d.vkd.vkDestroyFence(d.device, fence_, nullptr);
        d.vkd.vkDestroyCommandPool(d.device, pool_, nullptr);
        layout_ = {};
        set_layout_ = {};
        fragment_ = vertex_ = {};
        sampler_ = {};
        fence_ = {};
        pool_ = {};
    }

    Image SceneRenderer::target(Frame& frame, std::uint32_t width, std::uint32_t height) {
        const auto& d = gpu;
        auto image =
            d.image(target_format, width, height, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        frame.images.push_back(image);
        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = passes_.clear;
        info.attachmentCount = 1;
        info.pAttachments = &image.view;
        info.width = width;
        info.height = height;
        info.layers = 1;
        check(d.vkd.vkCreateFramebuffer(d.device, &info, nullptr, &frame.images.back().framebuffer),
              "vkCreateFramebuffer");
        return frame.images.back();
    }

    void SceneRenderer::release(Frame& frame) noexcept {
        const auto& d = gpu;
        for (auto& b : frame.buffers) {
            d.destroy(b);
        }
        for (auto& image : frame.images) {
            d.destroy(image);
        }
        for (auto pool : frame.pools) {
            d.vkd.vkDestroyDescriptorPool(d.device, pool, nullptr);
        }
        frame = {};
    }

    void SceneRenderer::upload(Frame& frame, const ScenePrepared& prepared) {
        const auto& d = gpu;
        // The images not on the device yet, each once: in this pass, its planes' passes, and the contents of the
        // layers that are drawn in this frame.
        std::vector<std::shared_ptr<const SceneImage>> pending;
        std::unordered_set<std::uint64_t> seen;
        const auto walk = [&](auto&& self, const ScenePrepared& p) -> void {
            for (std::size_t i = 0; i < p.images.size(); ++i) {
                if (p.images[i] && !textures_.contains(p.images[i]->id) && seen.insert(p.images[i]->id).second) {
                    pending.push_back(p.images[i]);
                }
                if (p.planes[i]) {
                    self(self, *p.planes[i]);
                }
                if (p.layers[i] && !textures_.contains(p.layers[i]->id)) {
                    self(self, *p.layers[i]->content);
                }
            }
        };
        walk(walk, prepared);
        if (pending.empty()) {
            return;
        }
        VkDeviceSize staged{};
        std::vector<VkBufferImageCopy> copies;
        for (const auto& image : pending) {
            ensure(image->width && image->height &&
                       image->bgra.size() ==
                           std::size_t(image->width) * image->height * weft::constants::rgba8_bytes_per_pixel,
                   "scene image size");
            VkBufferImageCopy copy{};
            copy.bufferOffset = staged;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {image->width, image->height, 1};
            copies.push_back(copy);
            staged += (image->bgra.size() + staging_alignment - 1) / staging_alignment * staging_alignment;
        }
        frame.buffers.push_back(d.buffer(staged, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
        const auto& staging = frame.buffers.back();
        std::vector<VkImageMemoryBarrier> to_transfer;
        std::vector<VkImageMemoryBarrier> to_read;
        for (std::size_t i = 0; i < pending.size(); ++i) {
            const auto& image = *pending[i];
            std::memcpy(static_cast<std::uint8_t*>(staging.mapped) + copies[i].bufferOffset, image.bgra.data(),
                        image.bgra.size());
            Texture texture{pending[i], d.image(target_format, image.width, image.height,
                                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)};
            const auto handle = texture.image.image;
            textures_.emplace(image.id, std::move(texture));
            frame.created.push_back(image.id);
            to_transfer.push_back(image_barrier(handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                0, VK_ACCESS_TRANSFER_WRITE_BIT));
            to_read.push_back(image_barrier(handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                            VK_ACCESS_SHADER_READ_BIT));
        }
        d.vkd.vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                   nullptr, 0, nullptr, std::uint32_t(to_transfer.size()), to_transfer.data());
        for (std::size_t i = 0; i < pending.size(); ++i) {
            d.vkd.vkCmdCopyBufferToImage(command_, staging.buffer, to_transfer[i].image,
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copies[i]);
        }
        d.vkd.vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                                   0, nullptr, 0, nullptr, std::uint32_t(to_read.size()), to_read.data());
        uploads += pending.size();
    }

    VkImageView SceneRenderer::layer(Frame& frame, const std::shared_ptr<const SceneLayerImage>& layer, float scale) {
        if (const auto it = textures_.find(layer->id); it != textures_.end()) {
            return it->second.image.view;
        }
        // Drawn into a target of the frame's, which the cache then keeps.
        auto image = target(frame, layer->width, layer->height);
        frame.images.pop_back();
        Texture texture{layer, image};
        textures_.emplace(layer->id, texture);
        frame.created.push_back(layer->id);
        record(frame, *layer->content, image, scale);
        transition(gpu, command_, image.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        return image.view;
    }

    void SceneRenderer::record(Frame& frame, const ScenePrepared& prepared, const Image& out, float scale) {
        const auto& d = gpu;
        const auto& pass = prepared.pass;
        // Nested passes first, each ending before this one begins: plane contents, and layers not drawn before.
        std::vector<VkImageView> views(prepared.images.size());
        for (std::size_t i = 0; i < prepared.images.size(); ++i) {
            if (prepared.planes[i]) {
                const auto image = target(frame, prepared.planes[i]->width, prepared.planes[i]->height);
                record(frame, *prepared.planes[i], image, scale);
                transition(d, command_, image.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                views[i] = image.view;
                ++plane_passes;
            } else if (prepared.layers[i]) {
                views[i] = layer(frame, prepared.layers[i], scale);
            } else if (prepared.images[i]) {
                views[i] = textures_.at(prepared.images[i]->id).image.view;
            }
        }
        // The pass's records, clips and View constants, and a group image of its size for each depth.
        frame.buffers.push_back(d.buffer(std::max<std::size_t>(pass.records.size(), 1) * sizeof(SceneRecord),
                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true));
        const auto records = frame.buffers.back().buffer;
        std::memcpy(frame.buffers.back().mapped, pass.records.data(), pass.records.size() * sizeof(SceneRecord));
        VkBuffer clips = blank_buffer_.buffer;
        if (!pass.clips.empty()) {
            frame.buffers.push_back(
                d.buffer(pass.clips.size() * sizeof(SceneClip), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true));
            clips = frame.buffers.back().buffer;
            std::memcpy(frame.buffers.back().mapped, pass.clips.data(), pass.clips.size() * sizeof(SceneClip));
        }
        // The vertex stage adds no base instance: Vulkan's instance index already counts the draw's first instance.
        const std::array<float, 4> constants{float(prepared.width), float(prepared.height), scale, 0};
        frame.buffers.push_back(d.buffer(sizeof(constants), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true));
        const auto view = frame.buffers.back().buffer;
        std::memcpy(frame.buffers.back().mapped, constants.data(), sizeof(constants));
        std::vector<Image> groups;
        for (std::size_t depth = 0; depth < pass.group_depth; ++depth) {
            groups.push_back(target(frame, prepared.width, prepared.height));
        }

        // One descriptor set per draw: the image it samples, the group it composites, or none.
        std::vector<VkImageView> images;
        std::size_t open{};
        for (const auto& batch : pass.batches) {
            switch (batch.step) {
                case ScenePass::Step::Boxes:
                    images.push_back(blank_image_.view);
                    break;
                case ScenePass::Step::Image:
                    // A path with nothing inside its clip has no image and draws nothing.
                    if (views[batch.first]) {
                        images.push_back(views[batch.first]);
                    }
                    break;
                case ScenePass::Step::BeginGroup:
                    ++open;
                    break;
                case ScenePass::Step::EndGroup:
                    images.push_back(groups.at(--open).view);
                    break;
            }
        }
        std::vector<VkDescriptorSet> sets(images.size());
        ensure(sets.size() <= unsigned(limits::max_pass_descriptor_sets), "descriptor sets of one pass");
        if (!sets.empty()) {
            frame.pools.push_back(create_scene_pool(d, std::uint32_t(sets.size())));
            const std::vector<VkDescriptorSetLayout> layouts(sets.size(), set_layout_);
            VkDescriptorSetAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocate.descriptorPool = frame.pools.back();
            allocate.descriptorSetCount = std::uint32_t(sets.size());
            allocate.pSetLayouts = layouts.data();
            check(d.vkd.vkAllocateDescriptorSets(d.device, &allocate, sets.data()), "vkAllocateDescriptorSets");
            const VkDescriptorBufferInfo view_info{view, 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo record_info{records, 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo clip_info{clips, 0, VK_WHOLE_SIZE};
            const VkDescriptorImageInfo sampler_info{sampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
            const std::uint32_t bindings[]{std::uint32_t(limits::binding_view), std::uint32_t(limits::binding_records),
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

        // The pass: cleared to its clear color, groups in their images, each composited into the target below.
        const auto begin = [&](VkFramebuffer framebuffer, bool clear, const paint::Color& color) {
            VkClearValue value{};
            std::copy(color.begin(), color.end(), value.color.float32);
            VkRenderPassBeginInfo info{};
            info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            info.renderPass = clear ? passes_.clear : passes_.load;
            info.framebuffer = framebuffer;
            info.renderArea = {{0, 0}, {prepared.width, prepared.height}};
            info.clearValueCount = clear ? 1u : 0u;
            info.pClearValues = &value;
            d.vkd.vkCmdBeginRenderPass(command_, &info, VK_SUBPASS_CONTENTS_INLINE);
            d.vkd.vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_GRAPHICS, passes_.pipeline);
            const VkViewport viewport{0, 0, float(prepared.width), float(prepared.height), 0, 1};
            d.vkd.vkCmdSetViewport(command_, 0, 1, &viewport);
            const VkRect2D scissor{{0, 0}, {prepared.width, prepared.height}};
            d.vkd.vkCmdSetScissor(command_, 0, 1, &scissor);
        };
        std::size_t next_set{};
        const auto instances = [&](std::uint32_t first, std::uint32_t count) {
            d.vkd.vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &sets[next_set++],
                                          0, nullptr);
            d.vkd.vkCmdDraw(command_, std::uint32_t(limits::quad_vertex_count), count, 0, first);
            ++draws;
        };
        begin(out.framebuffer, true, pass.clear);
        std::size_t depth{};
        for (const auto& batch : pass.batches) {
            switch (batch.step) {
                case ScenePass::Step::Boxes:
                    instances(batch.first, batch.count);
                    break;
                case ScenePass::Step::Image:
                    if (views[batch.first]) {
                        instances(batch.first, 1);
                    }
                    break;
                case ScenePass::Step::BeginGroup:
                    d.vkd.vkCmdEndRenderPass(command_);
                    begin(groups[depth++].framebuffer, true, paint::Color{});
                    break;
                case ScenePass::Step::EndGroup:
                    d.vkd.vkCmdEndRenderPass(command_);
                    transition(d, command_, groups[--depth].image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                               VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                    begin(depth ? groups[depth - 1].framebuffer : out.framebuffer, false, paint::Color{});
                    instances(batch.first, 1);
                    break;
            }
        }
        d.vkd.vkCmdEndRenderPass(command_);
    }

    std::optional<std::vector<std::uint8_t>> SceneRenderer::draw(const SceneFrame& frame, float scale, unsigned width,
                                                                 unsigned height,
                                                                 std::optional<std::array<float, 4>> crop,
                                                                 unsigned view_index,
                                                                 const std::vector<NodeIdentity>& layers) {
        const auto& d = gpu;
        ensure(width && height && width <= unsigned(limits::max_surface_dimension) &&
                   height <= unsigned(limits::max_surface_dimension),
               "bounded Vulkan scene target");
        // The device is idle between frames: images of images and layers that neither the preparer nor a pass holds
        // any more go now.
        for (auto it = textures_.begin(); it != textures_.end();) {
            if (it->second.source.expired()) {
                d.destroy(it->second.image);
                it = textures_.erase(it);
            } else {
                ++it;
            }
        }
        const auto prepared = preparer_.prepare(frame, scale, width, height, crop, view_index, layers);
        if (!prepared) {
            return std::nullopt;
        }
        if (target_.width != width || target_.height != height) {
            d.destroy(target_);
            d.destroy(readback_);
            target_ = d.image(target_format, width, height,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            VkFramebufferCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            info.renderPass = passes_.clear;
            info.attachmentCount = 1;
            info.pAttachments = &target_.view;
            info.width = width;
            info.height = height;
            info.layers = 1;
            check(d.vkd.vkCreateFramebuffer(d.device, &info, nullptr, &target_.framebuffer), "vkCreateFramebuffer");
            readback_ = d.buffer(VkDeviceSize(width) * height * weft::constants::rgba8_bytes_per_pixel,
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        }
        Frame f;
        bool submitted = false;
        try {
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check(d.vkd.vkBeginCommandBuffer(command_, &begin), "vkBeginCommandBuffer");
            upload(f, *prepared);
            record(f, *prepared, target_, scale);
            transition(d, command_, target_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {width, height, 1};
            d.vkd.vkCmdCopyImageToBuffer(command_, target_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                         readback_.buffer, 1, &copy);
            VkBufferMemoryBarrier host{};
            host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host.buffer = readback_.buffer;
            host.size = VK_WHOLE_SIZE;
            d.vkd.vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                                       nullptr, 1, &host, 0, nullptr);
            check(d.vkd.vkEndCommandBuffer(command_), "vkEndCommandBuffer");
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &command_;
            check(d.vkd.vkQueueSubmit(d.queue, 1, &submit, fence_), "vkQueueSubmit");
            submitted = true;
            d.wait(fence_);
            check(d.vkd.vkResetFences(d.device, 1, &fence_), "vkResetFences");
        } catch (...) {
            if (submitted) {
                d.vkd.vkDeviceWaitIdle(d.device);
            }
            d.vkd.vkResetCommandBuffer(command_, 0);
            // Images created for this frame hold nothing usable.
            for (const auto id : f.created) {
                if (const auto it = textures_.find(id); it != textures_.end()) {
                    d.destroy(it->second.image);
                    textures_.erase(it);
                }
            }
            release(f);
            throw;
        }
        release(f);
        ++passes;
        ++submissions;
        std::vector<std::uint8_t> out(std::size_t(width) * height * weft::constants::rgba8_bytes_per_pixel);
        std::memcpy(out.data(), readback_.mapped, out.size());
        return out;
    }
} // namespace weft::render::vulkan
