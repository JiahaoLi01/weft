#include "render/backends/vulkan/scene_renderer.hpp"

#include "constants/common.hpp"
#include "constants/render.hpp"
#include "scene/scene_schedule.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <tuple>
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
        // Its backdrop passes: the fullscreen triangle, and the area downsample, the Gaussian pass and the color matrix.
        constexpr unsigned char backdrop_vertex_spirv[] = {
#include "shaders/backdrop_blur.vs.spv.inc"
        };
        constexpr unsigned char area_fragment_spirv[] = {
#include "shaders/backdrop_area.ps.spv.inc"
        };
        constexpr unsigned char blur_fragment_spirv[] = {
#include "shaders/backdrop_blur.ps.spv.inc"
        };
        constexpr unsigned char matrix_fragment_spirv[] = {
#include "shaders/backdrop_matrix.ps.spv.inc"
        };
        // Its particle pass.
        constexpr unsigned char particle_vertex_spirv[] = {
#include "shaders/scene_particles_encoded.vs.spv.inc"
        };
        constexpr unsigned char particle_fragment_spirv[] = {
#include "shaders/scene_particles_encoded.ps.spv.inc"
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
        device->destroy(kept_records);
        device->destroy(kept_clips);
    }

    SceneRenderer::SceneRenderer(Device& device, SceneText& text, SceneCacheBounds bounds)
        : gpu(device), preparer_(text, *this, std::uint32_t(limits::max_surface_dimension), {}, bounds) {
        const auto& d = gpu;
        VkPhysicalDeviceProperties properties{};
        d.vki.vkGetPhysicalDeviceProperties(d.physical, &properties);
        const auto alignment = std::max<VkDeviceSize>(properties.limits.minUniformBufferOffsetAlignment, 1);
        backdrop_stride_ = (sizeof(BackdropConstants) + alignment - 1) / alignment * alignment;
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
            check(d.vkd.vkAllocateCommandBuffers(d.device, &allocate, &setup_), "vkAllocateCommandBuffers setup");
            VkFenceCreateInfo fence{};
            fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            check(d.vkd.vkCreateFence(d.device, &fence, nullptr, &fence_), "vkCreateFence");
            check(d.vkd.vkCreateFence(d.device, &fence, nullptr, &setup_fence_), "vkCreateFence setup");
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
            particle_vertex_ = shader_module(d, particle_vertex_spirv, sizeof(particle_vertex_spirv));
            particle_fragment_ = shader_module(d, particle_fragment_spirv, sizeof(particle_fragment_spirv));
            particle_pipeline_ = create_scene_pipeline(d, passes_.clear, layout_, particle_vertex_, particle_fragment_);
            under_pipeline_ = create_scene_pipeline(d, passes_.clear, layout_, vertex_, fragment_, true);
            mirror_sampler_ = create_mirror_sampler(d);
            backdrop_set_layout_ = create_backdrop_set_layout(d);
            VkPipelineLayoutCreateInfo backdrop_layout{};
            backdrop_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            backdrop_layout.setLayoutCount = 1;
            backdrop_layout.pSetLayouts = &backdrop_set_layout_;
            check(d.vkd.vkCreatePipelineLayout(d.device, &backdrop_layout, nullptr, &backdrop_layout_),
                  "vkCreatePipelineLayout backdrop");
            backdrop_vertex_ = shader_module(d, backdrop_vertex_spirv, sizeof(backdrop_vertex_spirv));
            const std::array<std::pair<const unsigned char*, std::size_t>, 3> fragments{
                {{area_fragment_spirv, sizeof(area_fragment_spirv)},
                 {blur_fragment_spirv, sizeof(blur_fragment_spirv)},
                 {matrix_fragment_spirv, sizeof(matrix_fragment_spirv)}}};
            for (std::size_t k = 0; k < fragments.size(); ++k) {
                backdrop_fragments_[k] = shader_module(d, fragments[k].first, fragments[k].second);
                // Over a cleared target, source-over writes the pass's color.
                backdrop_pipelines_[k] =
                    create_scene_pipeline(d, passes_.clear, backdrop_layout_, backdrop_vertex_, backdrop_fragments_[k]);
            }
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
        for (auto& [key, page] : pages_) {
            d.destroy(page);
        }
        pages_.clear();
        d.vkd.vkDestroyFence(d.device, setup_fence_, nullptr);
        setup_fence_ = {};
        for (auto& [program, e] : effect_pipelines_) {
            d.vkd.vkDestroyPipeline(d.device, e.pipeline, nullptr);
            d.vkd.vkDestroyShaderModule(d.device, e.fragment, nullptr);
        }
        effect_pipelines_.clear();
        for (std::size_t k = 0; k < backdrop_pipelines_.size(); ++k) {
            d.vkd.vkDestroyPipeline(d.device, backdrop_pipelines_[k], nullptr);
            d.vkd.vkDestroyShaderModule(d.device, backdrop_fragments_[k], nullptr);
        }
        backdrop_pipelines_ = {};
        backdrop_fragments_ = {};
        d.vkd.vkDestroyShaderModule(d.device, backdrop_vertex_, nullptr);
        d.vkd.vkDestroyPipelineLayout(d.device, backdrop_layout_, nullptr);
        d.vkd.vkDestroyDescriptorSetLayout(d.device, backdrop_set_layout_, nullptr);
        d.vkd.vkDestroySampler(d.device, mirror_sampler_, nullptr);
        d.vkd.vkDestroyPipeline(d.device, under_pipeline_, nullptr);
        backdrop_vertex_ = {};
        backdrop_layout_ = {};
        backdrop_set_layout_ = {};
        mirror_sampler_ = {};
        under_pipeline_ = {};
        d.vkd.vkDestroyPipeline(d.device, particle_pipeline_, nullptr);
        d.vkd.vkDestroyShaderModule(d.device, particle_fragment_, nullptr);
        d.vkd.vkDestroyShaderModule(d.device, particle_vertex_, nullptr);
        particle_pipeline_ = {};
        particle_fragment_ = particle_vertex_ = {};
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

    const Image& SceneRenderer::page(const SceneAtlasPage& key) {
        const auto& d = gpu;
        auto& page = pages_[{std::uint32_t(key.format), key.page}];
        if (page.image) {
            return page;
        }
        const auto size = scene_atlas_page_size(key.format);
        page = d.image(key.format == SceneImage::Format::Data ? VK_FORMAT_R8_UNORM : target_format, size, size,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        // Shader-readable before any recording that copies into it or samples it, which then keep it so: a recording
        // that is never submitted leaves it as it was.
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(d.vkd.vkBeginCommandBuffer(setup_, &begin), "vkBeginCommandBuffer setup");
        transition(d, setup_, page.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
                   VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        check(d.vkd.vkEndCommandBuffer(setup_), "vkEndCommandBuffer setup");
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &setup_;
        check(d.vkd.vkQueueSubmit(d.queue, 1, &submit, setup_fence_), "vkQueueSubmit setup");
        d.wait(setup_fence_);
        check(d.vkd.vkResetFences(d.device, 1, &setup_fence_), "vkResetFences setup");
        return page;
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
        // A backdrop copies from a pass's targets and into its own.
        auto image = d.image(target_format, width, height,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
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
        // Scratch images: those this recording drew with, whatever their bytes (the next one most likely draws the
        // same, and a pass's groups at a large target pass any fixed bound alone), then the spares it left within a
        // count and their bytes.
        std::size_t image_bytes{};
        for (const auto& image : slot.images) {
            image_bytes += std::size_t(image.width) * image.height * 4;
        }
        for (auto& image : slot.spare_images) {
            const auto bytes = std::size_t(image.width) * image.height * 4;
            if (slot.images.size() < std::size_t(limits::scene_slot_spare_images) &&
                image_bytes + bytes <= std::size_t(limits::scene_slot_spare_image_bytes)) {
                slot.images.push_back(image);
                image_bytes += bytes;
            } else {
                d.destroy(image);
            }
        }
        slot.spare_images = std::move(slot.images);
        slot.images.clear();
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
                (prepared.layers[i] && pending(prepared.layers[i]->id)) ||
                (prepared.filters[i] && pending(prepared.filters[i]->id))) {
                throw Busy("a scene samples what another recording that is not submitted uploads or draws");
            }
            if (prepared.planes[i]) {
                check_pending(*prepared.planes[i]);
            }
            if (prepared.layers[i] && !textures_.contains(prepared.layers[i]->id)) {
                check_pending(*prepared.layers[i]->content);
            }
            if (prepared.filters[i] && !textures_.contains(prepared.filters[i]->id)) {
                check_pending(*prepared.filters[i]->content);
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
                if (p.filters[i] && !textures_.contains(p.filters[i]->id)) {
                    self(self, *p.filters[i]->content);
                }
            }
            for (const auto& effect : p.effects) {
                for (const auto& texture : effect.textures) {
                    if (!textures_.contains(texture->id) && seen.insert(texture->id).second) {
                        pending.push_back(texture);
                    }
                }
            }
            for (const auto& sprite : p.sprites) {
                if (sprite && !textures_.contains(sprite->id) && seen.insert(sprite->id).second) {
                    pending.push_back(sprite);
                }
            }
        };
        walk(walk, prepared);
        if (pending.empty()) {
            return;
        }
        // An image on an atlas page goes into its region, inside its border; others into images of their own.
        constexpr auto border = std::uint32_t(weft::constants::render::scene::atlas_border);
        VkDeviceSize staged{};
        std::vector<VkBufferImageCopy> copies;
        std::vector<std::vector<std::uint8_t>> bordered(pending.size());
        for (std::size_t i = 0; i < pending.size(); ++i) {
            const auto& image = *pending[i];
            ensure(image.width && image.height &&
                       image.pixels.size() ==
                           std::size_t(image.width) * image.height * scene_image_texel_bytes(image.format),
                   "scene image size");
            VkBufferImageCopy copy{};
            copy.bufferOffset = staged;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {image.width, image.height, 1};
            if (image.atlas) {
                bordered[i] = scene_atlas_pixels(image);
                copy.imageOffset = {std::int32_t(image.atlas->x - border), std::int32_t(image.atlas->y - border), 0};
                copy.imageExtent = {image.width + 2 * border, image.height + 2 * border, 1};
            }
            copies.push_back(copy);
            const auto bytes = image.atlas ? bordered[i].size() : image.pixels.size();
            staged += (bytes + staging_alignment - 1) / staging_alignment * staging_alignment;
        }
        const auto& staging = buffer(staged);
        std::vector<VkImageMemoryBarrier> to_transfer;
        std::vector<VkImageMemoryBarrier> to_read;
        std::vector<VkImage> destinations;
        bool to_pages{};
        for (std::size_t i = 0; i < pending.size(); ++i) {
            const auto& image = *pending[i];
            const auto& pixels = image.atlas ? bordered[i] : image.pixels;
            std::memcpy(static_cast<std::uint8_t*>(staging.mapped) + copies[i].bufferOffset, pixels.data(),
                        pixels.size());
            if (image.atlas) {
                // A page keeps what earlier recordings put on it: it leaves the shader-readable layout once these
                // copies follow the reads of earlier submitted recordings, and returns to it.
                const auto handle = page({image.format, image.atlas->page}).image;
                destinations.push_back(handle);
                textures_.emplace(image.id, Texture{pending[i], {}, slot_->recording});
                slot_->created.push_back(image.id);
                if (std::none_of(to_transfer.begin(), to_transfer.end(), [&](const VkImageMemoryBarrier& b) {
                        return b.image == handle;
                    })) {
                    to_transfer.push_back(image_barrier(handle, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                                                        VK_ACCESS_TRANSFER_WRITE_BIT));
                    to_read.push_back(image_barrier(handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT));
                    to_pages = true;
                }
                continue;
            }
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
            destinations.push_back(handle);
            textures_.emplace(image.id, std::move(texture));
            slot_->created.push_back(image.id);
            to_transfer.push_back(image_barrier(handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                0, VK_ACCESS_TRANSFER_WRITE_BIT));
            to_read.push_back(image_barrier(handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                            VK_ACCESS_SHADER_READ_BIT));
        }
        d.vkd.vkCmdPipelineBarrier(command_buffer_,
                                   to_pages ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                                   std::uint32_t(to_transfer.size()), to_transfer.data());
        for (std::size_t i = 0; i < pending.size(); ++i) {
            d.vkd.vkCmdCopyBufferToImage(command_buffer_, staging.buffer, destinations[i],
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
        record(*layer->content, image.framebuffer, image.image, scale);
        transition(gpu, command_buffer_, image.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        return image.view;
    }

    VkImageView SceneRenderer::filter(const std::shared_ptr<const SceneFilterImage>& filter) {
        if (const auto it = textures_.find(filter->id); it != textures_.end()) {
            return it->second.image.view;
        }
        const auto& d = gpu;
        const auto command = command_buffer_;
        // The chain's images: its result an image that the cache then keeps, the others the slot's.
        const auto& chain = filter->chain;
        std::vector<Image> images(chain.images.size());
        for (std::size_t k = 0; k < images.size(); ++k) {
            images[k] = target(chain.images[k][0], chain.images[k][1], k == chain.result);
        }
        textures_.emplace(filter->id, Texture{filter, images[chain.result], slot_->recording});
        slot_->created.push_back(filter->id);
        const auto readable = [&](const Image& done) {
            transition(d, command, done.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        };
        // The content into the first, then each pass into its image.
        record(*filter->content, images[0].framebuffer, images[0].image, filter->scale);
        readable(images[0]);
        if (!chain.steps.empty()) {
            // Each pass's set: its constants, the image it reads and the mirroring sampler.
            std::vector<VkDescriptorSet> sets(chain.steps.size());
            const auto descriptors = pool(std::uint32_t(sets.size()));
            const std::vector<VkDescriptorSetLayout> layouts(sets.size(), backdrop_set_layout_);
            VkDescriptorSetAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocate.descriptorPool = descriptors;
            allocate.descriptorSetCount = std::uint32_t(sets.size());
            allocate.pSetLayouts = layouts.data();
            check(d.vkd.vkAllocateDescriptorSets(d.device, &allocate, sets.data()), "vkAllocateDescriptorSets filter");
            const auto& values = buffer(backdrop_stride_ * sets.size());
            for (std::size_t k = 0; k < sets.size(); ++k) {
                std::memcpy(static_cast<std::byte*>(values.mapped) + backdrop_stride_ * k, &chain.steps[k].constants,
                            sizeof(BackdropConstants));
                const VkDescriptorBufferInfo constants_info{values.buffer, backdrop_stride_ * k,
                                                            sizeof(BackdropConstants)};
                const VkDescriptorImageInfo source_info{VK_NULL_HANDLE, images[chain.steps[k].source].view,
                                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                const VkDescriptorImageInfo mirror_info{mirror_sampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
                VkWriteDescriptorSet writes[3]{};
                const std::uint32_t bindings[]{std::uint32_t(limits::binding_view),
                                               std::uint32_t(limits::binding_records),
                                               std::uint32_t(limits::binding_sampler)};
                const VkDescriptorType types[]{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                               VK_DESCRIPTOR_TYPE_SAMPLER};
                for (unsigned i = 0; i < std::size(writes); ++i) {
                    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writes[i].dstSet = sets[k];
                    writes[i].dstBinding = bindings[i];
                    writes[i].descriptorCount = 1;
                    writes[i].descriptorType = types[i];
                }
                writes[0].pBufferInfo = &constants_info;
                writes[1].pImageInfo = &source_info;
                writes[2].pImageInfo = &mirror_info;
                d.vkd.vkUpdateDescriptorSets(d.device, std::uint32_t(std::size(writes)), writes, 0, nullptr);
            }
            for (std::size_t k = 0; k < chain.steps.size(); ++k) {
                const auto& step = chain.steps[k];
                const auto& over = images[step.target];
                if (step.over) {
                    // Drawn over what it holds: an attachment again.
                    transition(d, command, over.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                               VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                }
                VkClearValue value{};
                VkRenderPassBeginInfo info{};
                info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                info.renderPass = step.over ? passes_.load : passes_.clear;
                info.framebuffer = over.framebuffer;
                info.renderArea = {{0, 0}, {over.width, over.height}};
                info.clearValueCount = step.over ? 0u : 1u;
                info.pClearValues = &value;
                d.vkd.vkCmdBeginRenderPass(command, &info, VK_SUBPASS_CONTENTS_INLINE);
                const VkViewport viewport{0, 0, float(over.width), float(over.height), 0, 1};
                d.vkd.vkCmdSetViewport(command, 0, 1, &viewport);
                const VkRect2D scissor{{0, 0}, {over.width, over.height}};
                d.vkd.vkCmdSetScissor(command, 0, 1, &scissor);
                // The backdrop pipelines draw source-over: over a cleared image, the pass's color.
                d.vkd.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        backdrop_pipelines_.at(step.shader == render::FilterStep::Shader::Area   ? 0
                                                               : step.shader == render::FilterStep::Shader::Blur ? 1
                                                                                                                 : 2));
                d.vkd.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, backdrop_layout_, 0, 1,
                                              &sets[k], 0, nullptr);
                d.vkd.vkCmdDraw(command, 3, 1, 0, 0);
                d.vkd.vkCmdEndRenderPass(command);
                readable(over);
            }
        }
        ++filter_passes;
        return images[chain.result].view;
    }

    void SceneRenderer::record(const ScenePrepared& prepared, VkFramebuffer out, VkImage image, float scale,
                               const SceneAnchorFrame* anchors, std::uint64_t serial, const paint::Color* clear,
                               std::span<const SceneTexture> host_textures, const ScenePatchHistory* history) {
        const auto& d = gpu;
        const auto& pass = prepared.pass;
        const auto command = command_buffer_;
        // What the pass draws, in order (scene/scene_schedule.hpp); a backdrop of a target the pass may not read is
        // transparent.
        const auto schedule = schedule_scene_pass(prepared, host_textures, clear, image != VkImage{});
        // Nested passes first, each ending before this one begins: plane contents, and layers not drawn before. The
        // slot leases every image and layer the pass samples.
        std::vector<VkImageView> views(prepared.images.size());
        for (std::size_t i = 0; i < prepared.images.size(); ++i) {
            if (prepared.planes[i]) {
                const auto plane = target(prepared.planes[i]->width, prepared.planes[i]->height);
                record(*prepared.planes[i], plane.framebuffer, plane.image, scale, nullptr, 0, nullptr, host_textures);
                transition(d, command, plane.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                views[i] = plane.view;
                ++plane_passes;
            } else if (prepared.layers[i]) {
                views[i] = layer(prepared.layers[i], scale);
                slot_->leases.push_back(prepared.layers[i]);
            } else if (prepared.filters[i]) {
                views[i] = filter(prepared.filters[i]);
                slot_->leases.push_back(prepared.filters[i]);
            } else if (const auto& drawn = prepared.images[i]) {
                views[i] =
                    drawn->atlas ? page({drawn->format, drawn->atlas->page}).view : textures_.at(drawn->id).image.view;
                slot_->leases.push_back(drawn);
            }
        }
        // The host texture each texture item draws.
        for (std::size_t i = 0; i < schedule.textures.size(); ++i) {
            if (const auto* texture = schedule.textures[i]) {
                views[i] = native_handle<VkImageView>(texture->view);
            }
        }
        texture_draws += schedule.bound_textures;
        unbound_textures += schedule.unbound_textures;
        // The records of host textures whose rows start at the bottom sample them flipped.
        const SceneRecord* record_data = pass.records.data();
        std::vector<SceneRecord> flipped;
        if (const auto flips = scene_texture_flips(prepared, host_textures);
            std::find(flips.begin(), flips.end(), true) != flips.end()) {
            flipped = pass.records;
            for (std::size_t i = 0; i < flipped.size(); ++i) {
                if (prepared.textures[i] && flips[prepared.textures[i] - 1]) {
                    flip_rows(flipped[i]);
                }
            }
            record_data = flipped.data();
        }
        // The pass's records, clips and View constants, and a group image of its size for each depth. A kept pass's
        // records and clips stay in the slot's own buffers while the slot draws that pass.
        VkBuffer records{};
        VkBuffer clips = blank_buffer_.buffer;
        if (serial) {
            const auto put = [&](Buffer& kept, const void* data, std::size_t bytes) {
                if (kept.size < std::max<std::size_t>(bytes, 1)) {
                    d.destroy(kept);
                    VkDeviceSize capacity = limits::spare_buffer_min_bytes;
                    while (capacity < bytes) {
                        capacity *= 2;
                    }
                    kept = gpu.buffer(capacity, slot_buffer_usage, true);
                    ++slot_objects;
                }
                std::memcpy(kept.mapped, data, bytes);
            };
            // A slot holding an earlier version of the kept pass writes only the records and rounded clips patched
            // since (none of them draws a host texture, so none is flipped).
            ScenePatch since;
            if (slot_->kept != serial && slot_->kept && history && history->since(slot_->kept, since) &&
                slot_->kept_records.size >= std::max<std::size_t>(pass.records.size() * sizeof(SceneRecord), 1) &&
                slot_->kept_clips.size >= std::max<std::size_t>(pass.clips.size() * sizeof(SceneClip), 1)) {
                for (const auto& [begin, end] : since.records) {
                    std::memcpy(static_cast<SceneRecord*>(slot_->kept_records.mapped) + begin,
                                pass.records.data() + begin, (end - begin) * sizeof(SceneRecord));
                }
                for (const auto& [begin, end] : since.clips) {
                    std::memcpy(static_cast<SceneClip*>(slot_->kept_clips.mapped) + begin, pass.clips.data() + begin,
                                (end - begin) * sizeof(SceneClip));
                }
                slot_->kept = serial;
                ++partial_uploads;
            } else if (slot_->kept != serial) {
                put(slot_->kept_records, record_data, pass.records.size() * sizeof(SceneRecord));
                put(slot_->kept_clips, pass.clips.data(), pass.clips.size() * sizeof(SceneClip));
                slot_->kept = serial;
            }
            records = slot_->kept_records.buffer;
            if (!pass.clips.empty()) {
                clips = slot_->kept_clips.buffer;
            }
        } else {
            auto& record_buffer = buffer(std::max<std::size_t>(pass.records.size(), 1) * sizeof(SceneRecord));
            records = record_buffer.buffer;
            std::memcpy(record_buffer.mapped, record_data, pass.records.size() * sizeof(SceneRecord));
            if (!pass.clips.empty()) {
                auto& clip_buffer = buffer(pass.clips.size() * sizeof(SceneClip));
                clips = clip_buffer.buffer;
                std::memcpy(clip_buffer.mapped, pass.clips.data(), pass.clips.size() * sizeof(SceneClip));
            }
        }
        // The record each draw position takes, and the frame's anchors.
        const auto* order_values = &in_order_;
        if (anchors && !anchors->order.empty()) {
            ensure(anchors->order.size() == pass.records.size(), "a record order of the pass's records");
            order_values = &anchors->order;
        } else if (in_order_.size() < pass.records.size()) {
            in_order_.resize(pass.records.size());
            for (std::size_t i = 0; i < in_order_.size(); ++i) {
                in_order_[i] = std::uint32_t(i);
            }
        }
        auto& order_buffer = buffer(std::max<std::size_t>(pass.records.size(), 1) * sizeof(std::uint32_t));
        const auto order = order_buffer.buffer;
        std::memcpy(order_buffer.mapped, order_values->data(), pass.records.size() * sizeof(std::uint32_t));
        VkBuffer anchor_values = blank_buffer_.buffer;
        if (anchors && !anchors->anchors.empty()) {
            ensure(anchors->anchors.size() == prepared.anchors.size(), "the anchors of the pass's anchored elements");
            auto& anchor_buffer = buffer(anchors->anchors.size() * sizeof(anchors->anchors[0]));
            anchor_values = anchor_buffer.buffer;
            std::memcpy(anchor_buffer.mapped, anchors->anchors.data(),
                        anchors->anchors.size() * sizeof(anchors->anchors[0]));
        } else {
            ensure(prepared.anchors.empty(), "the anchors of the pass's anchored elements");
        }
        // The vertex stage adds no base instance: Vulkan's instance index already counts the draw's first instance.
        const std::array<float, 4> constants{float(prepared.width), float(prepared.height), scale, 0};
        auto& view_buffer = buffer(sizeof(constants));
        const auto view = view_buffer.buffer;
        std::memcpy(view_buffer.mapped, constants.data(), sizeof(constants));
        std::vector<Image> groups;
        for (std::size_t depth = 0; depth < schedule.groups; ++depth) {
            groups.push_back(target(prepared.width, prepared.height));
        }
        // Each backdrop's chain of images: its copy, then one target per filter pass; the backdrop draws the last.
        std::vector<std::vector<Image>> chains;
        for (const auto& chain : schedule.chains) {
            auto& images = chains.emplace_back();
            for (const auto& [w, h] : chain.images) {
                images.push_back(target(w, h));
            }
        }

        // A descriptor set per distinct sampling of the pass's draws: the image a draw samples (a texture, a page, the
        // group it composites, a backdrop's filtered copy, or none) with an effect's textures (or a sprite); per draw
        // in order, its set; and per effect draw in order, its pipeline. Draws that sample alike share a set.
        using Textures = std::array<VkImageView, weft::constants::max_effect_textures>;
        std::vector<VkImageView> images;
        std::vector<std::optional<Textures>> effect_views;
        std::vector<std::size_t> draw_sets;
        std::map<std::tuple<VkImageView, bool, Textures>, std::size_t> distinct;
        const auto use = [&](VkImageView image, const std::optional<Textures>& textures) {
            const auto [at, added] =
                distinct.try_emplace({image, textures.has_value(), textures.value_or(Textures{})}, images.size());
            if (added) {
                images.push_back(image);
                effect_views.push_back(textures);
            }
            draw_sets.push_back(at->second);
        };
        std::vector<VkPipeline> effects;
        for (const auto& step : schedule.commands) {
            const auto* draw = std::get_if<SceneSchedule::Draw>(&step);
            if (!draw) {
                continue;
            }
            VkImageView sampled = blank_image_.view;
            switch (draw->image.kind) {
                case SceneSchedule::Image::Kind::Blank:
                    break;
                case SceneSchedule::Image::Kind::Item:
                    sampled = views.at(draw->image.index);
                    ensure(sampled != VkImageView{}, "an image item's view");
                    break;
                case SceneSchedule::Image::Kind::Page:
                    sampled = page(prepared.pages.at(draw->image.index)).view;
                    break;
                case SceneSchedule::Image::Kind::Group:
                    sampled = groups.at(draw->image.index).view;
                    break;
                case SceneSchedule::Image::Kind::Backdrop:
                    sampled = chains.at(draw->image.index).back().view;
                    break;
            }
            if (draw->extra == SceneSchedule::Extra::None) {
                use(sampled, std::nullopt);
                continue;
            }
            Textures textures;
            textures.fill(blank_image_.view);
            if (draw->extra == SceneSchedule::Extra::Effect) {
                const auto& effect = prepared.effects.at(draw->source);
                ensure(effect.textures.size() <= weft::constants::max_effect_textures, "effect textures");
                for (std::size_t t = 0; t < effect.textures.size(); ++t) {
                    textures[t] = textures_.at(effect.textures[t]->id).image.view;
                    slot_->leases.push_back(effect.textures[t]);
                }
                effects.push_back(effect_pipeline(effect.program));
            } else if (const auto& sprite = prepared.sprites.at(draw->source)) {
                // The emission's sprite where an effect's first texture goes.
                textures[0] = textures_.at(sprite->id).image.view;
                slot_->leases.push_back(sprite);
            }
            use(sampled, textures);
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
            // The repeating sampler, which host textures take too (scene.hlsl, linearRepeat).
            const VkDescriptorImageInfo repeat_info{effect_sampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
            const VkDescriptorBufferInfo order_info{order, 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo anchor_info{anchor_values, 0, VK_WHOLE_SIZE};
            const std::uint32_t bindings[]{
                std::uint32_t(limits::binding_view),    std::uint32_t(limits::binding_records),
                std::uint32_t(limits::binding_image),   std::uint32_t(limits::binding_clips),
                std::uint32_t(limits::binding_sampler), std::uint32_t(limits::binding_order),
                std::uint32_t(limits::binding_anchors), std::uint32_t(limits::binding_effect_sampler)};
            const VkDescriptorType types[]{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                           VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                           VK_DESCRIPTOR_TYPE_SAMPLER,        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                           VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLER};
            for (std::size_t s = 0; s < sets.size(); ++s) {
                const VkDescriptorImageInfo image_info{VK_NULL_HANDLE, images[s],
                                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                VkWriteDescriptorSet writes[8]{};
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
                writes[5].pBufferInfo = &order_info;
                writes[6].pBufferInfo = &anchor_info;
                writes[7].pImageInfo = &repeat_info;
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

        // The backdrop passes' sets: each pass's constants, the image it reads and the mirroring sampler; a chain's
        // first is at its first_sets.
        std::vector<std::size_t> first_sets;
        std::size_t backdrop_set_count{};
        for (const auto& chain : schedule.chains) {
            first_sets.push_back(backdrop_set_count);
            backdrop_set_count += chain.passes.size();
        }
        std::vector<VkDescriptorSet> backdrop_sets(backdrop_set_count);
        if (!backdrop_sets.empty()) {
            const auto descriptors = pool(std::uint32_t(backdrop_sets.size()));
            const std::vector<VkDescriptorSetLayout> layouts(backdrop_sets.size(), backdrop_set_layout_);
            VkDescriptorSetAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocate.descriptorPool = descriptors;
            allocate.descriptorSetCount = std::uint32_t(backdrop_sets.size());
            allocate.pSetLayouts = layouts.data();
            check(d.vkd.vkAllocateDescriptorSets(d.device, &allocate, backdrop_sets.data()),
                  "vkAllocateDescriptorSets backdrop");
            // Every pass's constants in one buffer, each at its stride.
            const auto& values = buffer(backdrop_stride_ * backdrop_sets.size());
            std::size_t s{};
            for (std::size_t c = 0; c < schedule.chains.size(); ++c) {
                const auto& filters = schedule.chains[c].passes;
                for (std::size_t k = 0; k < filters.size(); ++k, ++s) {
                    std::memcpy(static_cast<std::byte*>(values.mapped) + backdrop_stride_ * s, &filters[k].second,
                                sizeof(BackdropConstants));
                    const VkDescriptorBufferInfo constants_info{values.buffer, backdrop_stride_ * s,
                                                                sizeof(BackdropConstants)};
                    const VkDescriptorImageInfo source_info{VK_NULL_HANDLE, chains[c][k].view,
                                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                    const VkDescriptorImageInfo mirror_info{mirror_sampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
                    VkWriteDescriptorSet writes[3]{};
                    const std::uint32_t bindings[]{std::uint32_t(limits::binding_view),
                                                   std::uint32_t(limits::binding_records),
                                                   std::uint32_t(limits::binding_sampler)};
                    const VkDescriptorType types[]{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                                   VK_DESCRIPTOR_TYPE_SAMPLER};
                    for (unsigned i = 0; i < std::size(writes); ++i) {
                        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                        writes[i].dstSet = backdrop_sets[s];
                        writes[i].dstBinding = bindings[i];
                        writes[i].descriptorCount = 1;
                        writes[i].descriptorType = types[i];
                    }
                    writes[0].pBufferInfo = &constants_info;
                    writes[1].pImageInfo = &source_info;
                    writes[2].pImageInfo = &mirror_info;
                    d.vkd.vkUpdateDescriptorSets(d.device, std::uint32_t(std::size(writes)), writes, 0, nullptr);
                }
            }
        }

        // The commands: each target begun cleared or kept as a render pass with the scene pipeline, groups in their
        // images, each composited into the target below.
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
        std::size_t next_draw{};
        // The set bound for the scene's layout: none yet, or after a backdrop's passes bound theirs.
        constexpr auto unbound = std::numeric_limits<std::size_t>::max();
        auto bound = unbound;
        const auto instances = [&](std::uint32_t first, std::uint32_t count, std::uint32_t vertices) {
            if (const auto set = draw_sets.at(next_draw++); set != bound) {
                d.vkd.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &sets[set], 0,
                                              nullptr);
                bound = set;
            }
            d.vkd.vkCmdDraw(command, vertices, count, 0, first);
            ++draws;
        };
        // A backdrop chain's render pass over the whole of its image, cleared (transparent unless given).
        const auto begin_over = [&](const Image& over, const paint::Color& color = {}) {
            VkClearValue value{};
            std::copy(color.begin(), color.end(), value.color.float32);
            VkRenderPassBeginInfo info{};
            info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            info.renderPass = passes_.clear;
            info.framebuffer = over.framebuffer;
            info.renderArea = {{0, 0}, {over.width, over.height}};
            info.clearValueCount = 1;
            info.pClearValues = &value;
            d.vkd.vkCmdBeginRenderPass(command, &info, VK_SUBPASS_CONTENTS_INLINE);
            const VkViewport viewport{0, 0, float(over.width), float(over.height), 0, 1};
            d.vkd.vkCmdSetViewport(command, 0, 1, &viewport);
            const VkRect2D scissor{{0, 0}, {over.width, over.height}};
            d.vkd.vkCmdSetScissor(command, 0, 1, &scissor);
        };
        const auto readable = [&](VkImage done) {
            transition(d, command, done, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        };
        // The image of a target of the schedule: the pass's own (none when the host gave none), a group's, or a
        // chain's.
        const auto image_of = [&](const SceneSchedule::Target& t) {
            switch (t.kind) {
                case SceneSchedule::Target::Kind::Group:
                    return groups.at(t.index).image;
                case SceneSchedule::Target::Kind::Chain:
                    return chains.at(t.index).at(t.image).image;
                case SceneSchedule::Target::Kind::Out:
                    break;
            }
            return image;
        };
        std::size_t next_effect{};
        for (const auto& step : schedule.commands) {
            if (const auto* b = std::get_if<SceneSchedule::Begin>(&step)) {
                if (b->target.kind == SceneSchedule::Target::Kind::Chain) {
                    begin_over(chains.at(b->target.index).at(b->target.image), b->clear.value_or(paint::Color{}));
                } else {
                    begin(b->target.kind == SceneSchedule::Target::Kind::Out ? out
                                                                             : groups.at(b->target.index).framebuffer,
                          b->clear.has_value(), b->clear.value_or(paint::Color{}));
                }
            } else if (const auto* e = std::get_if<SceneSchedule::End>(&step)) {
                d.vkd.vkCmdEndRenderPass(command);
                if (e->sampled) {
                    readable(image_of(e->target));
                }
            } else if (const auto* draw = std::get_if<SceneSchedule::Draw>(&step)) {
                VkPipeline pipeline{};
                switch (draw->pipeline) {
                    case SceneSchedule::Pipeline::Scene:
                        break;
                    case SceneSchedule::Pipeline::Under:
                        pipeline = under_pipeline_;
                        break;
                    case SceneSchedule::Pipeline::Particles:
                        pipeline = particle_pipeline_;
                        break;
                    case SceneSchedule::Pipeline::Effect:
                        pipeline = effects.at(next_effect++);
                        break;
                }
                if (pipeline) {
                    d.vkd.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                }
                instances(draw->first, draw->count, draw->vertices);
                if (pipeline) {
                    d.vkd.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, passes_.pipeline);
                }
            } else if (const auto* copy = std::get_if<SceneSchedule::Copy>(&step)) {
                // The region of the target the backdrop reads, into its chain's first image.
                const auto from = image_of(copy->from);
                ensure(from != VkImage{}, "a backdrop copy from a readable target");
                const auto& to = chains.at(copy->chain).front();
                transition(d, command, from, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT);
                transition(d, command, to.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                           VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkImageCopy region{};
                region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.srcOffset = {copy->region[0], copy->region[1], 0};
                region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.extent = {to.width, to.height, 1};
                d.vkd.vkCmdCopyImage(command, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to.image,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                transition(d, command, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                           VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                transition(d, command, to.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            } else if (const auto* filter = std::get_if<SceneSchedule::Filter>(&step)) {
                const auto& over = chains.at(filter->chain).at(filter->pass + 1);
                begin_over(over);
                const auto kind = schedule.chains.at(filter->chain).passes.at(filter->pass).first;
                d.vkd.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        backdrop_pipelines_.at(std::size_t(kind)));
                d.vkd.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, backdrop_layout_, 0, 1,
                                              &backdrop_sets.at(first_sets.at(filter->chain) + filter->pass), 0,
                                              nullptr);
                bound = unbound;
                d.vkd.vkCmdDraw(command, 3, 1, 0, 0);
                d.vkd.vkCmdEndRenderPass(command);
                readable(over.image);
            }
        }
        backdrop_passes += schedule.backdrops;
        backdrop_copies += schedule.copies;
        if (const auto unreadable = schedule.unreadable_backdrops + schedule.unreadable_copies) {
            reasons["backdrop of an unreadable target"] += unreadable;
        }
    }

    bool SceneRenderer::encode(Slot& slot, const SceneFrame& frame, VkCommandBuffer command, const Target& target,
                               float scale, std::optional<std::array<float, 4>> crop, unsigned view_index,
                               const std::vector<NodeIdentity>& layers, const SceneAnchorTable* anchors,
                               const SceneTargetStart& start, std::span<const SceneTexture> textures) {
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
        // A frame of the revision the view's kept pass was prepared for records it again with the frame's anchors; a
        // frame of another revision patches it where the pass stands (ScenePreparer::patch).
        const bool keep = frame.revision && !crop;
        auto& kept = kept_[view_index];
        std::shared_ptr<ScenePrepared> prepared;
        if (keep && kept.prepared && kept.scale == scale && kept.width == target.width &&
            kept.height == target.height && kept.layers == layers && kept.prepared->over == start.over &&
            scene_texture_flips(*kept.prepared, textures) == kept.flips) {
            bool drawable = kept.revision == frame.revision;
            if (!drawable) {
                if (const auto patched = preparer_.patch(*kept.prepared, frame, view_index, anchors)) {
                    const auto from = kept.serial;
                    kept.serial = ++serial_;
                    kept.revision = frame.revision;
                    kept.history.add(from, kept.serial, *patched);
                    ++kept_patches;
                    for (const auto& [begin, end] : patched->records) {
                        patched_records += end - begin;
                    }
                    drawable = true;
                }
            }
            if (drawable && preparer_.place(*kept.prepared, view_index, anchors, kept.frame)) {
                prepared = kept.prepared;
                ++kept_passes;
            } else if (drawable) {
                ++kept_rebuilds;
            }
        }
        if (!prepared) {
            kept = {};
            prepared = preparer_.prepare(frame, scale, target.width, target.height, crop, view_index, layers, anchors,
                                         keep, start.over);
            if (!prepared) {
                last_failure_ = preparer_.last_fallback();
                return false;
            }
            if (prepared->keepable) {
                kept = {frame.revision,
                        scale,
                        target.width,
                        target.height,
                        layers,
                        prepared,
                        ++serial_,
                        {},
                        scene_texture_flips(*prepared, textures),
                        {}};
                ensure(preparer_.place(*prepared, view_index, anchors, kept.frame), "a pass placed as it was planned");
            }
        }
        const auto* placed = kept.prepared == prepared ? &kept.frame : nullptr;
        const auto serial = kept.prepared == prepared ? kept.serial : 0;
        // An effect's or a url() image's translucent color texture is linear floats, which not every device filters.
        const auto linear = [](auto&& self, const ScenePrepared& p) -> bool {
            for (const auto& effect : p.effects) {
                for (const auto& texture : effect.textures) {
                    if (texture->format == SceneImage::Format::Linear) {
                        return true;
                    }
                }
            }
            for (const auto& image : p.images) {
                if (image && image->format == SceneImage::Format::Linear) {
                    return true;
                }
            }
            for (std::size_t i = 0; i < p.planes.size(); ++i) {
                if ((p.planes[i] && self(self, *p.planes[i])) || (p.layers[i] && self(self, *p.layers[i]->content)) ||
                    (p.filters[i] && self(self, *p.filters[i]->content))) {
                    return true;
                }
            }
            return false;
        };
        if (!d.filters(VK_FORMAT_R32G32B32A32_SFLOAT) && linear(linear, *prepared)) {
            ++fallbacks;
            ++reasons["effect float texture filtering"];
            last_failure_ = "effect float texture filtering";
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
            record(*prepared, slot.framebuffer, target.image, scale, placed, serial,
                   start.clear ? &*start.clear : nullptr, textures, serial ? &kept.history : nullptr);
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

    std::optional<std::vector<std::uint8_t>>
    SceneRenderer::draw(const SceneFrame& frame, float scale, unsigned width, unsigned height,
                        std::optional<std::array<float, 4>> crop, unsigned view_index,
                        const std::vector<NodeIdentity>& layers, bool readable, const SceneAnchorTable* anchors) {
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
            drawn = encode(slot, frame, command_, {target_.view, width, height, readable ? target_.image : VkImage{}},
                           scale, crop, view_index, layers, anchors);
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
