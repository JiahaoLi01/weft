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
        // A slot's buffers serve any of a pass's uses.
        constexpr VkBufferUsageFlags slot_buffer_usage =
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    } // namespace

    SceneRenderer::Slot::~Slot() {
        if (!device) {
            return;
        }
        for (auto* list : {&buffers, &spare_buffers}) {
            for (auto& b : *list) {
                device->destroy(b);
            }
        }
        for (auto* list : {&images, &spare_images}) {
            for (auto& image : *list) {
                device->destroy(image);
            }
        }
        for (auto* list : {&pools, &spare_pools}) {
            for (auto& [pool, sets] : *list) {
                device->vkd.vkDestroyDescriptorPool(device->device, pool, nullptr);
            }
        }
        if (framebuffer) {
            device->vkd.vkDestroyFramebuffer(device->device, framebuffer, nullptr);
        }
    }

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
            effect_sampler_ = create_linear_sampler(d, true);
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
            // The blank image is only bound, never sampled for a visible result; it needs a shader-readable layout. This
            // one submission precedes every recording.
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
        for (auto& [program, e] : effect_pipelines_) {
            d.vkd.vkDestroyPipeline(d.device, e.pipeline, nullptr);
            d.vkd.vkDestroyShaderModule(d.device, e.fragment, nullptr);
        }
        effect_pipelines_.clear();
        d.destroy(readback_);
        d.destroy(target_);
        d.destroy(blank_buffer_);
        d.destroy(blank_image_);
        destroy(d, passes_);
        d.vkd.vkDestroyPipelineLayout(d.device, layout_, nullptr);
        d.vkd.vkDestroyDescriptorSetLayout(d.device, set_layout_, nullptr);
        d.vkd.vkDestroyShaderModule(d.device, fragment_, nullptr);
        d.vkd.vkDestroyShaderModule(d.device, vertex_, nullptr);
        d.vkd.vkDestroySampler(d.device, effect_sampler_, nullptr);
        d.vkd.vkDestroySampler(d.device, sampler_, nullptr);
        d.vkd.vkDestroyFence(d.device, fence_, nullptr);
        d.vkd.vkDestroyCommandPool(d.device, pool_, nullptr);
        layout_ = {};
        set_layout_ = {};
        fragment_ = vertex_ = {};
        sampler_ = effect_sampler_ = {};
        fence_ = {};
        pool_ = {};
    }

    VkFramebuffer SceneRenderer::framebuffer(VkImageView view, std::uint32_t width, std::uint32_t height) {
        const auto& d = gpu;
        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = passes_.clear;
        info.attachmentCount = 1;
        info.pAttachments = &view;
        info.width = width;
        info.height = height;
        info.layers = 1;
        VkFramebuffer out{};
        check(d.vkd.vkCreateFramebuffer(d.device, &info, nullptr, &out), "vkCreateFramebuffer");
        return out;
    }

    Image SceneRenderer::target(std::uint32_t width, std::uint32_t height, bool kept) {
        const auto& d = gpu;
        if (!kept) {
            auto& spares = slot_->spare_images;
            const auto spare = std::find_if(spares.begin(), spares.end(), [&](const Image& image) {
                return image.width == width && image.height == height;
            });
            if (spare != spares.end()) {
                slot_->images.push_back(*spare);
                spares.erase(spare);
                return slot_->images.back();
            }
        }
        auto image =
            d.image(target_format, width, height, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        try {
            image.framebuffer = framebuffer(image.view, width, height);
        } catch (...) {
            d.destroy(image);
            throw;
        }
        if (!kept) {
            slot_->images.push_back(image);
            ++slot_objects;
        }
        return image;
    }

    Buffer& SceneRenderer::buffer(VkDeviceSize size) {
        auto& spares = slot_->spare_buffers;
        auto best = spares.end();
        for (auto i = spares.begin(); i != spares.end(); ++i) {
            if (i->size >= size && (best == spares.end() || i->size < best->size)) {
                best = i;
            }
        }
        if (best != spares.end()) {
            slot_->buffers.push_back(*best);
            spares.erase(best);
            return slot_->buffers.back();
        }
        VkDeviceSize capacity = limits::spare_buffer_min_bytes;
        while (capacity < size) {
            capacity *= 2;
        }
        slot_->buffers.push_back(gpu.buffer(capacity, slot_buffer_usage, true));
        ++slot_objects;
        return slot_->buffers.back();
    }

    VkDescriptorPool SceneRenderer::pool(std::uint32_t sets) {
        const auto& d = gpu;
        auto& spares = slot_->spare_pools;
        auto best = spares.end();
        for (auto i = spares.begin(); i != spares.end(); ++i) {
            if (i->second >= sets && (best == spares.end() || i->second < best->second)) {
                best = i;
            }
        }
        if (best != spares.end()) {
            slot_->pools.push_back(*best);
            spares.erase(best);
            check(d.vkd.vkResetDescriptorPool(d.device, slot_->pools.back().first, 0), "vkResetDescriptorPool");
            return slot_->pools.back().first;
        }
        auto capacity = std::uint32_t(limits::descriptor_pool_min_sets);
        while (capacity < sets) {
            capacity *= 2;
        }
        slot_->pools.emplace_back(create_scene_pool(d, capacity), capacity);
        ++slot_objects;
        return slot_->pools.back().first;
    }

    void SceneRenderer::release(Slot& slot) noexcept {
        const auto& d = gpu;
        // Spares for the slot's next recordings, as many as it keeps; the rest go.
        const auto keep = [&](auto& from, auto& to, std::size_t bound, auto destroy) {
            for (auto& value : from) {
                if (to.size() < bound) {
                    to.push_back(value);
                } else {
                    destroy(value);
                }
            }
            from.clear();
        };
        keep(slot.buffers, slot.spare_buffers, std::size_t(limits::scene_slot_spare_buffers), [&](Buffer& b) {
            d.destroy(b);
        });
        keep(slot.images, slot.spare_images, std::size_t(limits::scene_slot_spare_images), [&](Image& image) {
            d.destroy(image);
        });
        keep(slot.pools, slot.spare_pools, std::size_t(limits::scene_slot_spare_pools),
             [&](std::pair<VkDescriptorPool, std::uint32_t>& pool) {
                 d.vkd.vkDestroyDescriptorPool(d.device, pool.first, nullptr);
             });
        if (slot.framebuffer) {
            d.vkd.vkDestroyFramebuffer(d.device, slot.framebuffer, nullptr);
        }
        slot.framebuffer = {};
        slot.leases.clear();
        slot.created.clear();
        slot.state = Slot::State::Idle;
    }

    void SceneRenderer::check_pending(const ScenePrepared& prepared) const {
        const auto pending = [&](std::uint64_t id) {
            const auto it = textures_.find(id);
            return it != textures_.end() && it->second.pending && it->second.pending != slot_->recording;
        };
        for (std::size_t i = 0; i < prepared.images.size(); ++i) {
            if ((prepared.images[i] && pending(prepared.images[i]->id)) ||
                (prepared.layers[i] && pending(prepared.layers[i]->id))) {
                throw Busy("a scene samples what another recording that is not submitted uploads or draws");
            }
            if (prepared.planes[i]) {
                check_pending(*prepared.planes[i]);
            }
            if (prepared.layers[i] && !textures_.contains(prepared.layers[i]->id)) {
                check_pending(*prepared.layers[i]->content);
            }
        }
        for (const auto& effect : prepared.effects) {
            for (const auto& texture : effect.textures) {
                if (pending(texture->id)) {
                    throw Busy("a scene samples what another recording that is not submitted uploads");
                }
            }
        }
    }

    void SceneRenderer::upload(const ScenePrepared& prepared) {
        const auto& d = gpu;
        // The images not on the device yet, each once: in this pass, its planes' passes, and the contents of the
        // layers that are drawn in this recording.
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
            for (const auto& effect : p.effects) {
                for (const auto& texture : effect.textures) {
                    if (!textures_.contains(texture->id) && seen.insert(texture->id).second) {
                        pending.push_back(texture);
                    }
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
                       image->pixels.size() ==
                           std::size_t(image->width) * image->height * scene_image_texel_bytes(image->format),
                   "scene image size");
            VkBufferImageCopy copy{};
            copy.bufferOffset = staged;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {image->width, image->height, 1};
            copies.push_back(copy);
            staged += (image->pixels.size() + staging_alignment - 1) / staging_alignment * staging_alignment;
        }
        const auto& staging = buffer(staged);
        std::vector<VkImageMemoryBarrier> to_transfer;
        std::vector<VkImageMemoryBarrier> to_read;
        for (std::size_t i = 0; i < pending.size(); ++i) {
            const auto& image = *pending[i];
            std::memcpy(static_cast<std::uint8_t*>(staging.mapped) + copies[i].bufferOffset, image.pixels.data(),
                        image.pixels.size());
            // Each format as the shader samples it: encoded premultiplied bytes as they are, data in r, sRGB decoded
            // to linear light before filtering, linear floats.
            const auto format = image.format == SceneImage::Format::Bgra   ? target_format
                                : image.format == SceneImage::Format::Data ? VK_FORMAT_R8_UNORM
                                : image.format == SceneImage::Format::Srgb ? VK_FORMAT_R8G8B8A8_SRGB
                                                                           : VK_FORMAT_R32G32B32A32_SFLOAT;
            Texture texture{pending[i],
                            d.image(format, image.width, image.height,
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT),
                            slot_->recording};
            const auto handle = texture.image.image;
            textures_.emplace(image.id, std::move(texture));
            slot_->created.push_back(image.id);
            to_transfer.push_back(image_barrier(handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                0, VK_ACCESS_TRANSFER_WRITE_BIT));
            to_read.push_back(image_barrier(handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                            VK_ACCESS_SHADER_READ_BIT));
        }
        d.vkd.vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   0, 0, nullptr, 0, nullptr, std::uint32_t(to_transfer.size()), to_transfer.data());
        for (std::size_t i = 0; i < pending.size(); ++i) {
            d.vkd.vkCmdCopyBufferToImage(command_buffer_, staging.buffer, to_transfer[i].image,
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copies[i]);
        }
        d.vkd.vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                                   std::uint32_t(to_read.size()), to_read.data());
        uploads += pending.size();
    }

    VkPipeline SceneRenderer::effect_pipeline(const std::shared_ptr<const paint::EffectProgram>& program) {
        if (const auto found = effect_pipelines_.find(program.get()); found != effect_pipelines_.end()) {
            return found->second.pipeline;
        }
        ensure(!program->spirv_encoded.empty(), "an effect built for Vulkan");
        EffectPipeline made{
            program, shader_module(gpu, program->spirv_encoded.data(), program->spirv_encoded.size()), {}};
        try {
            made.pipeline = create_scene_pipeline(gpu, passes_.clear, layout_, vertex_, made.fragment);
        } catch (...) {
            gpu.vkd.vkDestroyShaderModule(gpu.device, made.fragment, nullptr);
            throw;
        }
        return effect_pipelines_.emplace(program.get(), std::move(made)).first->second.pipeline;
    }

    VkImageView SceneRenderer::layer(const std::shared_ptr<const SceneLayerImage>& layer, float scale) {
        if (const auto it = textures_.find(layer->id); it != textures_.end()) {
            return it->second.image.view;
        }
        // Drawn here into an image that the cache then keeps.
        auto image = target(layer->width, layer->height, true);
        textures_.emplace(layer->id, Texture{layer, image, slot_->recording});
        slot_->created.push_back(layer->id);
        record(*layer->content, image.framebuffer, scale);
        transition(gpu, command_buffer_, image.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        return image.view;
    }

    void SceneRenderer::record(const ScenePrepared& prepared, VkFramebuffer out, float scale) {
        const auto& d = gpu;
        const auto& pass = prepared.pass;
        const auto command = command_buffer_;
        // Nested passes first, each ending before this one begins: plane contents, and layers not drawn before. The
        // slot leases every image and layer the pass samples.
        std::vector<VkImageView> views(prepared.images.size());
        for (std::size_t i = 0; i < prepared.images.size(); ++i) {
            if (prepared.planes[i]) {
                const auto image = target(prepared.planes[i]->width, prepared.planes[i]->height);
                record(*prepared.planes[i], image.framebuffer, scale);
                transition(d, command, image.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                views[i] = image.view;
                ++plane_passes;
            } else if (prepared.layers[i]) {
                views[i] = layer(prepared.layers[i], scale);
                slot_->leases.push_back(prepared.layers[i]);
            } else if (prepared.images[i]) {
                views[i] = textures_.at(prepared.images[i]->id).image.view;
                slot_->leases.push_back(prepared.images[i]);
            }
        }
        // The pass's records, clips and View constants, and a group image of its size for each depth.
        auto& record_buffer = buffer(std::max<std::size_t>(pass.records.size(), 1) * sizeof(SceneRecord));
        const auto records = record_buffer.buffer;
        std::memcpy(record_buffer.mapped, pass.records.data(), pass.records.size() * sizeof(SceneRecord));
        VkBuffer clips = blank_buffer_.buffer;
        if (!pass.clips.empty()) {
            auto& clip_buffer = buffer(pass.clips.size() * sizeof(SceneClip));
            clips = clip_buffer.buffer;
            std::memcpy(clip_buffer.mapped, pass.clips.data(), pass.clips.size() * sizeof(SceneClip));
        }
        // The vertex stage adds no base instance: Vulkan's instance index already counts the draw's first instance.
        const std::array<float, 4> constants{float(prepared.width), float(prepared.height), scale, 0};
        auto& view_buffer = buffer(sizeof(constants));
        const auto view = view_buffer.buffer;
        std::memcpy(view_buffer.mapped, constants.data(), sizeof(constants));
        std::vector<Image> groups;
        for (std::size_t depth = 0; depth < pass.group_depth; ++depth) {
            groups.push_back(target(prepared.width, prepared.height));
        }

        // One descriptor set per draw: the image it samples, the group it composites, or none; an effect's textures and,
        // in batch order, its pipeline.
        std::vector<VkImageView> images;
        std::vector<std::optional<std::array<VkImageView, weft::constants::max_effect_textures>>> effect_views;
        std::vector<VkPipeline> effects;
        std::size_t open{};
        for (const auto& batch : pass.batches) {
            switch (batch.step) {
                case ScenePass::Step::Boxes:
                    images.push_back(blank_image_.view);
                    effect_views.emplace_back();
                    break;
                case ScenePass::Step::Image:
                    // A path with nothing inside its clip has no image and draws nothing.
                    if (views[batch.first]) {
                        images.push_back(views[batch.first]);
                        effect_views.emplace_back();
                    }
                    break;
                case ScenePass::Step::BeginGroup:
                    ++open;
                    break;
                case ScenePass::Step::EndGroup:
                    images.push_back(groups.at(--open).view);
                    effect_views.emplace_back();
                    break;
                case ScenePass::Step::Effect: {
                    const auto& effect = prepared.effects.at(batch.source);
                    ensure(effect.textures.size() <= weft::constants::max_effect_textures, "effect textures");
                    std::array<VkImageView, weft::constants::max_effect_textures> textures;
                    textures.fill(blank_image_.view);
                    for (std::size_t t = 0; t < effect.textures.size(); ++t) {
                        textures[t] = textures_.at(effect.textures[t]->id).image.view;
                        slot_->leases.push_back(effect.textures[t]);
                    }
                    images.push_back(blank_image_.view);
                    effect_views.emplace_back(textures);
                    effects.push_back(effect_pipeline(effect.program));
                    break;
                }
            }
        }
        std::vector<VkDescriptorSet> sets(images.size());
        ensure(sets.size() <= unsigned(limits::max_pass_descriptor_sets), "descriptor sets of one pass");
        if (!sets.empty()) {
            const auto descriptors = pool(std::uint32_t(sets.size()));
            const std::vector<VkDescriptorSetLayout> layouts(sets.size(), set_layout_);
            VkDescriptorSetAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocate.descriptorPool = descriptors;
            allocate.descriptorSetCount = std::uint32_t(sets.size());
            allocate.pSetLayouts = layouts.data();
            check(d.vkd.vkAllocateDescriptorSets(d.device, &allocate, sets.data()), "vkAllocateDescriptorSets");
            // The View constants' own range: a reused buffer can be larger than a uniform buffer range may be.
            const VkDescriptorBufferInfo view_info{view, 0, sizeof(constants)};
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
                if (!effect_views[s]) {
                    continue;
                }
                // An effect's textures and its repeating sampler.
                std::array<VkDescriptorImageInfo, weft::constants::max_effect_textures + 1> effect_info{};
                std::array<VkWriteDescriptorSet, weft::constants::max_effect_textures + 1> effect_writes{};
                for (std::size_t i = 0; i < effect_writes.size(); ++i) {
                    const bool repeat = i == weft::constants::max_effect_textures;
                    effect_info[i] =
                        repeat ? VkDescriptorImageInfo{effect_sampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED}
                               : VkDescriptorImageInfo{VK_NULL_HANDLE, (*effect_views[s])[i],
                                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                    auto& w = effect_writes[i];
                    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    w.dstSet = sets[s];
                    w.dstBinding = repeat ? std::uint32_t(limits::binding_effect_sampler)
                                          : std::uint32_t(limits::binding_effect_textures + i);
                    w.descriptorCount = 1;
                    w.descriptorType = repeat ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                    w.pImageInfo = &effect_info[i];
                }
                d.vkd.vkUpdateDescriptorSets(d.device, std::uint32_t(effect_writes.size()), effect_writes.data(), 0,
                                             nullptr);
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
            d.vkd.vkCmdBeginRenderPass(command, &info, VK_SUBPASS_CONTENTS_INLINE);
            d.vkd.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, passes_.pipeline);
            const VkViewport viewport{0, 0, float(prepared.width), float(prepared.height), 0, 1};
            d.vkd.vkCmdSetViewport(command, 0, 1, &viewport);
            const VkRect2D scissor{{0, 0}, {prepared.width, prepared.height}};
            d.vkd.vkCmdSetScissor(command, 0, 1, &scissor);
        };
        std::size_t next_set{};
        const auto instances = [&](std::uint32_t first, std::uint32_t count) {
            d.vkd.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &sets[next_set++], 0,
                                          nullptr);
            d.vkd.vkCmdDraw(command, std::uint32_t(limits::quad_vertex_count), count, 0, first);
            ++draws;
        };
        begin(out, true, pass.clear);
        std::size_t depth{};
        std::size_t next_effect{};
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
                    d.vkd.vkCmdEndRenderPass(command);
                    begin(groups[depth++].framebuffer, true, paint::Color{});
                    break;
                case ScenePass::Step::EndGroup:
                    d.vkd.vkCmdEndRenderPass(command);
                    transition(d, command, groups[--depth].image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                               VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                    begin(depth ? groups[depth - 1].framebuffer : out, false, paint::Color{});
                    instances(batch.first, 1);
                    break;
                case ScenePass::Step::Effect:
                    d.vkd.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, effects[next_effect++]);
                    instances(batch.first, batch.count);
                    d.vkd.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, passes_.pipeline);
                    break;
            }
        }
        d.vkd.vkCmdEndRenderPass(command);
    }

    bool SceneRenderer::encode(Slot& slot, const SceneFrame& frame, VkCommandBuffer command, const Target& target,
                               float scale, std::optional<std::array<float, 4>> crop, unsigned view_index,
                               const std::vector<NodeIdentity>& layers) {
        const auto& d = gpu;
        if (slot.state == Slot::State::Submitted) {
            retire(slot);
        }
        ensure(slot.state == Slot::State::Idle, "an idle or completed scene slot");
        ensure(target.view && target.width && target.height &&
                   target.width <= unsigned(limits::max_surface_dimension) &&
                   target.height <= unsigned(limits::max_surface_dimension),
               "bounded Vulkan scene target");
        // Images of images and layers that neither the preparer nor a slot holds any more: every recording that
        // sampled them completed.
        for (auto it = textures_.begin(); it != textures_.end();) {
            if (it->second.source.expired()) {
                d.destroy(it->second.image);
                it = textures_.erase(it);
            } else {
                ++it;
            }
        }
        const auto prepared = preparer_.prepare(frame, scale, target.width, target.height, crop, view_index, layers);
        if (!prepared) {
            return false;
        }
        // An effect's translucent color texture is linear floats, which not every device filters.
        const auto linear = [](auto&& self, const ScenePrepared& p) -> bool {
            for (const auto& effect : p.effects) {
                for (const auto& texture : effect.textures) {
                    if (texture->format == SceneImage::Format::Linear) {
                        return true;
                    }
                }
            }
            for (std::size_t i = 0; i < p.planes.size(); ++i) {
                if ((p.planes[i] && self(self, *p.planes[i])) || (p.layers[i] && self(self, *p.layers[i]->content))) {
                    return true;
                }
            }
            return false;
        };
        if (!d.filters(VK_FORMAT_R32G32B32A32_SFLOAT) && linear(linear, *prepared)) {
            ++fallbacks;
            ++reasons["effect float texture filtering"];
            return false;
        }
        slot_ = &slot;
        command_buffer_ = command;
        slot.recording = ++next_recording_;
        slot.device = &gpu;
        try {
            check_pending(*prepared);
            upload(*prepared);
            slot.framebuffer = framebuffer(target.view, target.width, target.height);
            record(*prepared, slot.framebuffer, scale);
        } catch (...) {
            // The host discards the commands: what they would upload or draw leaves the cache.
            for (const auto id : slot.created) {
                if (const auto it = textures_.find(id); it != textures_.end()) {
                    d.destroy(it->second.image);
                    textures_.erase(it);
                }
            }
            release(slot);
            slot_ = nullptr;
            throw;
        }
        slot_ = nullptr;
        slot.state = Slot::State::Recorded;
        ++passes;
        ++recordings;
        return true;
    }

    void SceneRenderer::submitted(Slot& slot) {
        ensure(slot.state == Slot::State::Recorded, "a recorded scene slot");
        for (const auto id : slot.created) {
            if (const auto it = textures_.find(id); it != textures_.end() && it->second.pending == slot.recording) {
                it->second.pending = 0;
            }
        }
        slot.state = Slot::State::Submitted;
    }

    void SceneRenderer::discard(Slot& slot) noexcept {
        if (slot.state != Slot::State::Recorded) {
            return;
        }
        // No other recording samples these: one that would have was refused as Busy.
        const auto& d = gpu;
        for (const auto id : slot.created) {
            if (const auto it = textures_.find(id); it != textures_.end() && it->second.pending == slot.recording) {
                d.destroy(it->second.image);
                textures_.erase(it);
            }
        }
        release(slot);
    }

    void SceneRenderer::retire(Slot& slot) noexcept {
        if (slot.state == Slot::State::Recorded) {
            return;
        }
        release(slot);
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
        if (target_.width != width || target_.height != height) {
            d.destroy(target_);
            d.destroy(readback_);
            target_ = d.image(target_format, width, height,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            readback_ = d.buffer(VkDeviceSize(width) * height * weft::constants::rgba8_bytes_per_pixel,
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        }
        auto& slot = draw_slot_;
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(d.vkd.vkBeginCommandBuffer(command_, &begin), "vkBeginCommandBuffer");
        bool drawn{};
        try {
            drawn = encode(slot, frame, command_, {target_.view, width, height}, scale, crop, view_index, layers);
        } catch (...) {
            d.vkd.vkResetCommandBuffer(command_, 0);
            throw;
        }
        if (!drawn) {
            d.vkd.vkResetCommandBuffer(command_, 0);
            return std::nullopt;
        }
        bool submitted_frame = false;
        try {
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
            submitted_frame = true;
            submitted(slot);
            d.wait(fence_);
            check(d.vkd.vkResetFences(d.device, 1, &fence_), "vkResetFences");
        } catch (...) {
            if (submitted_frame) {
                // The frame may still run: wait for it before its slot goes.
                d.vkd.vkDeviceWaitIdle(d.device);
                retire(slot);
            } else {
                d.vkd.vkResetCommandBuffer(command_, 0);
                discard(slot);
            }
            throw;
        }
        retire(slot);
        std::vector<std::uint8_t> out(std::size_t(width) * height * weft::constants::rgba8_bytes_per_pixel);
        std::memcpy(out.data(), readback_.mapped, out.size());
        return out;
    }
} // namespace weft::render::vulkan
