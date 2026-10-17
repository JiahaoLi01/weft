#include "render/backends/vulkan/renderer.hpp"

#include "constants/common.hpp"
#include "render/backends/vulkan/pipeline.hpp"
#include "constants/render.hpp"
#include "render/srgb.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <functional>
#include <stdexcept>
#include <string>
#include <tuple>
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

        // The bytes of the texture that the asset needs: its bordered image on the atlas, else its texels.
        std::size_t texture_bytes(const paint::Asset& a, bool atlas) {
            constexpr auto border = std::size_t(weft::constants::render::texture_cache::atlas_border);
            const auto pixels = std::size_t(a.width) * a.height;
            if (atlas) {
                return (a.width + 2 * border) * (a.height + 2 * border);
            }
            if (a.encoding != paint::Encoding::SrgbStraight) {
                return pixels;
            }
            for (std::size_t p = 3; p < a.bytes.size(); p += weft::constants::rgba8_bytes_per_pixel) {
                if (a.bytes[p] != weft::constants::color_channel_max) {
                    return pixels * weft::constants::rgba8_bytes_per_pixel * sizeof(float);
                }
            }
            return pixels * weft::constants::rgba8_bytes_per_pixel;
        }

        void validate(const paint::Asset& a) {
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
        }

        // Each element once, in order of address.
        template <class T>
        std::vector<T> distinct(std::vector<T> values) {
            std::sort(values.begin(), values.end(), std::less<>());
            values.erase(std::unique(values.begin(), values.end()), values.end());
            return values;
        }
    } // namespace

    FrameResources::FrameResources(Renderer& renderer) : renderer_(&renderer) {
        ++renderer.frames_created;
    }

    FrameResources::FrameResources(FrameResources&& other) noexcept
        : staging(std::exchange(other.staging, {})),
          records(std::exchange(other.records, {})),
          clips(std::exchange(other.clips, {})),
          pool(std::exchange(other.pool, {})),
          pool_sets(std::exchange(other.pool_sets, 0)),
          framebuffer(std::exchange(other.framebuffer, {})),
          renderer_(std::exchange(other.renderer_, nullptr)) {
    }

    FrameResources& FrameResources::operator=(FrameResources&& other) noexcept {
        if (this != &other) {
            release();
            staging = std::exchange(other.staging, {});
            records = std::exchange(other.records, {});
            clips = std::exchange(other.clips, {});
            pool = std::exchange(other.pool, {});
            pool_sets = std::exchange(other.pool_sets, 0);
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
        renderer_->recycle(*this);
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
        // Group layers are linear floats as in the D3D11 renderer; Vulkan only requires blending on 16-bit floats.
        layer_format_ =
            d.blends(VK_FORMAT_R32G32B32A32_SFLOAT) ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R16G16B16A16_SFLOAT;
        passes(output_format);
        passes(layer_format_);

        output_ = d.image(output_format, w, h, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        blank_image_ = d.image(VK_FORMAT_R8_UNORM, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        blank_buffer_ = d.buffer(sizeof(SceneClip), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        const std::array<float, 4> view{float(w), float(h), 1, 0};
        view_ = d.buffer(sizeof(view), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        std::memcpy(view_.mapped, view.data(), sizeof(view));
        spare_staging_.reserve(limits::spare_frame_resources);
        spare_records_.reserve(limits::spare_frame_resources);
        spare_clips_.reserve(limits::spare_frame_resources);
        spare_pools_.reserve(limits::spare_frame_resources);
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
        for (auto& page : atlas_pages) {
            d.destroy(page.image);
        }
        d.destroy(readback_);
        d.destroy(view_);
        for (auto* spares : {&spare_staging_, &spare_records_, &spare_clips_}) {
            for (auto& b : *spares) {
                d.destroy(b);
            }
        }
        for (auto& [pool, sets] : spare_pools_) {
            d.vkd.vkDestroyDescriptorPool(d.device, pool, nullptr);
        }
        d.destroy(blank_buffer_);
        d.destroy(blank_image_);
        for (auto& layer : layers_) {
            d.destroy(layer);
        }
        d.destroy(output_);
        for (auto& [key, e] : effect_pipelines_) {
            d.vkd.vkDestroyPipeline(d.device, e.pipeline, nullptr);
            d.vkd.vkDestroyShaderModule(d.device, e.fragment, nullptr);
        }
        for (auto& [format, p] : passes_) {
            d.vkd.vkDestroyPipeline(d.device, p.pipeline, nullptr);
            d.vkd.vkDestroyRenderPass(d.device, p.clear, nullptr);
            d.vkd.vkDestroyRenderPass(d.device, p.load, nullptr);
        }
        d.vkd.vkDestroyPipelineLayout(d.device, layout_, nullptr);
        d.vkd.vkDestroyDescriptorSetLayout(d.device, set_layout_, nullptr);
        d.vkd.vkDestroyShaderModule(d.device, fragment_, nullptr);
        d.vkd.vkDestroyShaderModule(d.device, vertex_, nullptr);
        d.vkd.vkDestroySampler(d.device, effect_sampler_, nullptr);
        d.vkd.vkDestroySampler(d.device, sampler_, nullptr);
        d.vkd.vkDestroyFence(d.device, fence_, nullptr);
        d.vkd.vkDestroyCommandPool(d.device, pool_, nullptr);
    }

    VkPipeline Renderer::effect_pipeline(const std::shared_ptr<const paint::EffectProgram>& program, VkFormat format) {
        const auto key = std::make_pair(program.get(), format);
        if (const auto found = effect_pipelines_.find(key); found != effect_pipelines_.end()) {
            return found->second.pipeline;
        }
        ensure(!program->spirv_linear.empty(), "an effect built for Vulkan");
        const auto& p = passes(format);
        EffectPipeline made{
            program, shader_module(gpu, program->spirv_linear.data(), program->spirv_linear.size()), {}};
        try {
            made.pipeline = create_scene_pipeline(gpu, p.clear, layout_, vertex_, made.fragment);
            return effect_pipelines_.emplace(key, made).first->second.pipeline;
        } catch (...) {
            gpu.vkd.vkDestroyPipeline(gpu.device, made.pipeline, nullptr);
            gpu.vkd.vkDestroyShaderModule(gpu.device, made.fragment, nullptr);
            throw;
        }
    }

    const Renderer::Passes& Renderer::passes(VkFormat format) {
        if (const auto found = passes_.find(format); found != passes_.end()) {
            return found->second;
        }
        ensure(gpu.blends(format), "a target format the device blends");
        auto created = create_scene_passes(gpu, format, layout_, vertex_, fragment_);
        try {
            return passes_.emplace(format, Passes{created.clear, created.load, created.pipeline}).first->second;
        } catch (...) {
            destroy(gpu, created);
            throw;
        }
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

    PreparedFrame Renderer::prepare(std::shared_ptr<const paint::DisplayList> list,
                                    std::vector<ExternalView> external) {
        ensure(list && bool(paint::check(*list, weft::constants::render::scene::max_group_depth)) &&
                   list->external_textures == external.size(),
               "a well-formed display list with a view of each external texture");
        for (const auto& e : external) {
            ensure(e.view != VK_NULL_HANDLE, "an external texture view");
        }
        for (const auto& item : list->items) {
            ensure(item.kind != paint::Item::Kind::Text && item.kind != paint::Item::Kind::Layer &&
                       item.kind != paint::Item::Kind::Shadow && item.kind != paint::Item::Kind::Path &&
                       item.kind != paint::Item::Kind::Plane && item.kind != paint::Item::Kind::Particles &&
                       item.kind != paint::Item::Kind::Backdrop && item.kind != paint::Item::Kind::Filter,
                   "the display list pass draws no scene rasters, shadows, paths, planes, particles, backdrops or "
                   "filters");
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
        frame.external = std::move(external);
        const auto& sources = frame.list->assets;

        // The frame's leases, one per asset; a failure releases them, and textures that nothing else holds retire.
        struct Leases {
            Renderer& r;
            std::vector<ResourceId> ids;
            bool kept{};

            ~Leases() {
                if (kept) {
                    return;
                }
                for (const auto& id : ids) {
                    if (r.tracker.ReleaseResource(id) != LifetimeResult::Ok) {
                        std::terminate();
                    }
                }
                r.collect();
            }
        } leases{*this, {}};

        leases.ids.reserve(sources.size());
        frame.assets.reserve(sources.size());
        assets_.reserve(assets_.size() + sources.size());
        std::vector<ResourceId> dependencies;
        dependencies.reserve(2 * sources.size());
        // An asset an effect samples has a texture of its own, never an atlas region, under a key of its own: an image
        // of the same asset on the atlas stays apart.
        std::vector<bool> standalone(sources.size());
        for (const auto& effect : frame.list->effects) {
            for (const auto t : effect.textures) {
                standalone[t] = true;
            }
        }
        std::vector<TextureCache<CachedTexture>::Key> keys;
        // Leases of the cached textures, taken before anything is created: eviction for an earlier miss cannot take a
        // later hit.
        std::vector<bool> held(sources.size());
        if (cache_assets) {
            keys.reserve(sources.size());
            for (std::size_t i = 0; i < sources.size(); ++i) {
                keys.emplace_back(asset_identities.identify(sources[i]),
                                  standalone[i] ? 1 | weft::constants::render::texture_cache::standalone_key_bit : 1);
            }
            for (std::size_t i = 0; i < keys.size(); ++i) {
                if (const auto found = cache.entries.find(keys[i]); found != cache.entries.end()) {
                    lifetime(tracker.RetainResource(found->second.native.id), "pin all incoming cached dependencies");
                    leases.ids.push_back(found->second.native.id);
                    held[i] = true;
                }
            }
        }
        for (std::size_t i = 0; i < sources.size(); ++i) {
            NativeAsset* texture{};
            if (!cache_assets) {
                validate(*sources[i]);
                texture = &create(*sources[i], next_asset_id_++, 1, standalone[i]);
                leases.ids.push_back(texture->id);
            } else if (auto* entry = cache.use(keys[i])) {
                texture = entry->native.texture;
                if (!held[i]) {
                    // Created earlier in this frame for the same asset.
                    lifetime(tracker.RetainResource(texture->id), "lease a texture of this frame again");
                    leases.ids.push_back(texture->id);
                }
            } else {
                const auto& a = *sources[i];
                validate(a);
                cache.evict(tracker, texture_bytes(a, atlas_candidate(a) && !standalone[i]), false,
                            [&](const TextureCache<CachedTexture>::Entry& evicted) {
                                if (evicted.native.texture->atlas) {
                                    atlas.release(*evicted.native.texture->atlas);
                                }
                            });
                texture = &create(a, keys[i].first, keys[i].second, standalone[i]);
                try {
                    cache.add(keys[i], CachedTexture{texture->id, texture}, texture->texels.size());
                } catch (...) {
                    if (texture->atlas) {
                        atlas.release(*texture->atlas);
                    }
                    lifetime(tracker.ReleaseResource(texture->id), "failed cache insertion rollback");
                    collect();
                    throw;
                }
                lifetime(tracker.RetainResource(texture->id), "new cached frame lease");
                leases.ids.push_back(texture->id);
            }
            frame.assets.push_back(texture);
            dependencies.push_back(texture->id);
            if (texture->atlas) {
                dependencies.push_back(atlas_pages.at(texture->atlas->page).id);
            }
        }
        lifetime(tracker.PrepareFrame(dependencies, frame.id), "PrepareFrame");
        leases.kept = true;
        return frame;
    }

    NativeAsset& Renderer::create(const paint::Asset& a, std::uint64_t asset_id, std::uint64_t version,
                                  bool standalone) {
        auto native = std::make_unique<NativeAsset>();
        if (cache_assets && atlas_candidate(a) && !standalone) {
            native->atlas = place_on_atlas(a);
        }
        try {
            if (native->atlas) {
                native->texels = bordered(a);
            } else {
                auto source = texels(a);
                native->image = gpu.image(source.format, a.width, a.height,
                                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
                native->texels = std::move(source.bytes);
            }
            lifetime(
                tracker.CreateResource({asset_id, version, native->texels.size(), ResourceKind::Texture}, native->id),
                "CreateResource");
        } catch (...) {
            gpu.destroy(native->image);
            if (native->atlas) {
                atlas.release(*native->atlas);
            }
            throw;
        }
        ++native_creations;
        // prepare() reserved room for the frame's textures.
        assets_.push_back(std::move(native));
        return *assets_.back();
    }

    std::optional<AtlasPacker::Region> Renderer::place_on_atlas(const paint::Asset& a) {
        constexpr auto border = std::uint32_t(weft::constants::render::texture_cache::atlas_border);
        auto region = atlas.allocate(a.width + 2 * border, a.height + 2 * border);
        if (!region) {
            return std::nullopt;
        }
        // Pages exist up to the region's; one that failed to appear earlier is created now.
        try {
            atlas_pages.reserve(region->page + 1);
            while (atlas_pages.size() <= region->page) {
                AtlasPage page;
                page.image = gpu.image(VK_FORMAT_R8_UNORM, atlas.page_size(), atlas.page_size(),
                                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
                const auto created = tracker.CreateResource(
                    {next_asset_id_++, 1, std::size_t(atlas.page_size()) * atlas.page_size(), ResourceKind::AtlasPage},
                    page.id);
                if (created != LifetimeResult::Ok) {
                    gpu.destroy(page.image);
                    lifetime(created, "CreateResource atlas page");
                }
                atlas_pages.push_back(page);
            }
        } catch (...) {
            atlas.release(*region);
            throw;
        }
        ++atlas_placements;
        return region;
    }

    VkImageView Renderer::view(const NativeAsset& texture) const {
        return texture.atlas ? atlas_pages[texture.atlas->page].image.view : texture.image.view;
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

    void Renderer::drain() {
        cache.evict(tracker, 0, true, [&](const TextureCache<CachedTexture>::Entry& evicted) {
            if (evicted.native.texture->atlas) {
                atlas.release(*evicted.native.texture->atlas);
            }
        });
        for (auto& page : atlas_pages) {
            lifetime(tracker.ReleaseResource(page.id), "release an atlas page");
            ResourceInfo info;
            ensure(tracker.InspectResource(page.id, info) == LifetimeResult::StaleHandle, "an unused atlas page");
            gpu.destroy(page.image);
        }
        atlas_pages.clear();
        atlas = AtlasPacker(weft::constants::render::texture_cache::atlas_page_size,
                            weft::constants::render::texture_cache::atlas_max_pages);
        asset_identities.clear();
        collect();
    }

    Encoded Renderer::encode(const PreparedFrame& frame, VkCommandBuffer command, const Target& target, bool clear,
                             const paint::Color& clear_color) {
        const auto& d = gpu;
        // The atlas pages the frame samples. A page that no submitted recording wrote is first written here, which
        // discards its contents; while another recording that first writes it is not submitted, the frame waits.
        std::vector<std::uint32_t> pages;
        for (const auto* a : frame.assets) {
            if (a->atlas) {
                pages.push_back(a->atlas->page);
            }
        }
        pages = distinct(std::move(pages));
        std::vector<std::uint32_t> first_writes;
        for (const auto p : pages) {
            ResourceInfo info;
            lifetime(tracker.InspectResource(atlas_pages.at(p).id, info), "inspect an atlas page");
            if (info.phase == ResourcePhase::Encoded) {
                throw Busy("an atlas page is first written by another recording that is not submitted");
            }
            if (info.phase == ResourcePhase::Created) {
                first_writes.push_back(p);
            }
        }
        Encoded encoded;
        lifetime(tracker.EncodeFrame(frame.id, encoded.token), "EncodeFrame before recording");
        try {
            encoded.resources = FrameResources(*this);
            auto& resources = encoded.resources;
            const auto& list = *frame.list;

            // Uploads of the textures that no submitted use uploaded, staged in this frame's buffer: whole images, and
            // regions of atlas pages. Another use encoded earlier and submitted before this one may still read or write
            // them: the uploads wait for those accesses.
            std::vector<VkImageMemoryBarrier> to_transfer;
            std::vector<VkImageMemoryBarrier> to_read;
            std::vector<VkBufferImageCopy> copies;
            std::vector<VkImage> destinations;
            std::vector<const NativeAsset*> pending;
            std::vector<std::uint32_t> written = first_writes;
            VkDeviceSize staged{};
            for (const auto* a : distinct(std::vector<const NativeAsset*>(frame.assets.begin(), frame.assets.end()))) {
                ResourceInfo info;
                lifetime(tracker.InspectResource(a->id, info), "inspect upload readiness");
                if (info.phase == ResourcePhase::Ready || info.phase == ResourcePhase::Submitted) {
                    continue;
                }
                ensure(!a->texels.empty(), "an unsubmitted texture keeps its texels");
                VkBufferImageCopy copy{};
                copy.bufferOffset = staged;
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                if (a->atlas) {
                    const auto& r = *a->atlas;
                    copy.imageOffset = {std::int32_t(r.x), std::int32_t(r.y), 0};
                    copy.imageExtent = {r.width, r.height, 1};
                    destinations.push_back(atlas_pages.at(r.page).image.image);
                    written.push_back(r.page);
                } else {
                    copy.imageExtent = {a->image.width, a->image.height, 1};
                    destinations.push_back(a->image.image);
                    to_transfer.push_back(image_barrier(a->image.image, VK_IMAGE_LAYOUT_UNDEFINED,
                                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT));
                    to_read.push_back(image_barrier(a->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT));
                }
                copies.push_back(copy);
                pending.push_back(a);
                staged += (a->texels.size() + staging_alignment - 1) / staging_alignment * staging_alignment;
            }
            // A written page keeps the regions on it, unless it is first written here.
            for (const auto p : distinct(std::move(written))) {
                const auto& page = atlas_pages.at(p).image.image;
                const bool first = std::binary_search(first_writes.begin(), first_writes.end(), p);
                to_transfer.push_back(image_barrier(
                    page, first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT));
                to_read.push_back(image_barrier(page, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                                VK_ACCESS_SHADER_READ_BIT));
            }
            if (staged) {
                resources.staging = take(spare_staging_, staged, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
                for (std::size_t i = 0; i < pending.size(); ++i) {
                    std::memcpy(static_cast<std::uint8_t*>(resources.staging.mapped) + copies[i].bufferOffset,
                                pending[i]->texels.data(), pending[i]->texels.size());
                }
            }

            // The pass; images of one texture (an atlas page) draw together, from their regions there.
            std::vector<std::uint64_t> names(frame.assets.size() + frame.external.size());
            for (std::size_t i = 0; i < frame.assets.size(); ++i) {
                names[i] = abi_handle(view(*frame.assets[i]));
            }
            for (std::size_t i = 0; i < frame.external.size(); ++i) {
                names[frame.assets.size() + i] = abi_handle(frame.external[i].view);
            }
            auto planned = plan_scene_pass(list, 1, width, height, std::nullopt, Space::Linear, &names);
            ensure(bool(planned), "display list pass plan");
            auto& pass = *planned;
            for (std::size_t i = 0; i < list.items.size(); ++i) {
                if (list.items[i].kind != paint::Item::Kind::Image) {
                    continue;
                }
                const auto source = list.images[list.items[i].payload].asset;
                if (source >= frame.assets.size()) {
                    if (frame.external.at(source - frame.assets.size()).bottom_left) {
                        flip_rows(pass.records[i]);
                    }
                } else if (const auto* a = frame.assets[source]; a->atlas) {
                    place_in(pass.records[i], region_uv(*a->atlas));
                }
            }
            if (!pass.records.empty()) {
                resources.records =
                    take(spare_records_, pass.records.size() * sizeof(SceneRecord), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
                std::memcpy(resources.records.mapped, pass.records.data(), pass.records.size() * sizeof(SceneRecord));
            }
            if (!pass.clips.empty()) {
                resources.clips =
                    take(spare_clips_, pass.clips.size() * sizeof(SceneClip), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
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

            // One descriptor set per draw: the image it samples, or the layer a group composites; an effect's textures
            // and, in batch order, its pipeline for the target it draws into.
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
                        images.push_back(batch.source < frame.assets.size()
                                             ? view(*frame.assets[batch.source])
                                             : frame.external.at(batch.source - frame.assets.size()).view);
                        effect_views.emplace_back();
                        break;
                    case ScenePass::Step::BeginGroup:
                        ++open;
                        break;
                    case ScenePass::Step::EndGroup:
                        images.push_back(layers_.at(--open).view);
                        effect_views.emplace_back();
                        break;
                    case ScenePass::Step::Effect: {
                        const auto& effect = list.effects[batch.source];
                        std::array<VkImageView, weft::constants::max_effect_textures> textures;
                        textures.fill(blank_image_.view);
                        for (std::size_t t = 0; t < effect.textures.size(); ++t) {
                            textures[t] = view(*frame.assets.at(effect.textures[t]));
                        }
                        images.push_back(blank_image_.view);
                        effect_views.emplace_back(textures);
                        effects.push_back(
                            effect_pipeline(list.programs[effect.program], open ? layer_format_ : target.format));
                        break;
                    }
                    case ScenePass::Step::Atlas:
                    case ScenePass::Step::Particles:
                    case ScenePass::Step::Backdrop:
                        // prepare refuses particles and backdrops: they draw in the scene pass alone, as atlas pages
                        // do.
                        break;
                }
            }
            std::vector<VkDescriptorSet> sets(images.size());
            ensure(sets.size() <= unsigned(limits::max_pass_descriptor_sets), "descriptor sets of one pass");
            if (!sets.empty()) {
                const auto count = std::uint32_t(sets.size());
                std::tie(resources.pool, resources.pool_sets) = take_pool(count);
                const std::vector<VkDescriptorSetLayout> layouts(sets.size(), set_layout_);
                VkDescriptorSetAllocateInfo allocate{};
                allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                allocate.descriptorPool = resources.pool;
                allocate.descriptorSetCount = count;
                allocate.pSetLayouts = layouts.data();
                check(d.vkd.vkAllocateDescriptorSets(d.device, &allocate, sets.data()), "vkAllocateDescriptorSets");
                const VkDescriptorBufferInfo view_info{view_.buffer, 0, VK_WHOLE_SIZE};
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
                    if (!effect_views[s]) {
                        continue;
                    }
                    // An effect's textures and its repeating sampler.
                    std::array<VkDescriptorImageInfo, weft::constants::max_effect_textures + 1> effect_info{};
                    std::array<VkWriteDescriptorSet, weft::constants::max_effect_textures + 1> effect_writes{};
                    for (std::size_t i = 0; i < effect_writes.size(); ++i) {
                        const bool sampler = i == weft::constants::max_effect_textures;
                        effect_info[i] =
                            sampler ? VkDescriptorImageInfo{effect_sampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED}
                                    : VkDescriptorImageInfo{VK_NULL_HANDLE, (*effect_views[s])[i],
                                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                        auto& w = effect_writes[i];
                        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                        w.dstSet = sets[s];
                        w.dstBinding = sampler ? std::uint32_t(limits::binding_effect_sampler)
                                               : std::uint32_t(limits::binding_effect_textures + i);
                        w.descriptorCount = 1;
                        w.descriptorType = sampler ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                        w.pImageInfo = &effect_info[i];
                    }
                    d.vkd.vkUpdateDescriptorSets(d.device, std::uint32_t(effect_writes.size()), effect_writes.data(), 0,
                                                 nullptr);
                }
            }
            if (inject_encode_failure) {
                throw std::runtime_error("injected encode failure");
            }

            // Recording: everything it uses exists, and nothing below fails.
            const bool clear_target = clear || pass.clear != paint::Color{};
            const auto& first_color = pass.clear != paint::Color{} ? pass.clear : clear_color;
            [&]() noexcept {
                if (!to_transfer.empty()) {
                    const auto n = std::uint32_t(to_transfer.size());
                    d.vkd.vkCmdPipelineBarrier(
                        command, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, n, to_transfer.data());
                    for (std::size_t i = 0; i < copies.size(); ++i) {
                        d.vkd.vkCmdCopyBufferToImage(command, resources.staging.buffer, destinations[i],
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
                std::size_t next_effect{};
                for (const auto& batch : pass.batches) {
                    switch (batch.step) {
                        case ScenePass::Step::Boxes:
                        case ScenePass::Step::Image:
                            draw_records(batch.first, batch.count);
                            break;
                        case ScenePass::Step::Effect:
                            d.vkd.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, effects[next_effect++]);
                            draw_records(batch.first, batch.count);
                            d.vkd.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                    (depth ? layer_passes : target_passes).pipeline);
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
                        case ScenePass::Step::Atlas:
                        case ScenePass::Step::Particles:
                        case ScenePass::Step::Backdrop:
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

    Buffer Renderer::take(std::vector<Buffer>& spares, VkDeviceSize size, VkBufferUsageFlags usage) {
        auto best = spares.end();
        for (auto i = spares.begin(); i != spares.end(); ++i) {
            if (i->size >= size && (best == spares.end() || i->size < best->size)) {
                best = i;
            }
        }
        if (best != spares.end()) {
            const auto out = *best;
            spares.erase(best);
            return out;
        }
        VkDeviceSize capacity = limits::spare_buffer_min_bytes;
        while (capacity < size) {
            capacity *= 2;
        }
        return gpu.buffer(capacity, usage, true);
    }

    std::pair<VkDescriptorPool, std::uint32_t> Renderer::take_pool(std::uint32_t sets) {
        const auto& d = gpu;
        auto best = spare_pools_.end();
        for (auto i = spare_pools_.begin(); i != spare_pools_.end(); ++i) {
            if (i->second >= sets && (best == spare_pools_.end() || i->second < best->second)) {
                best = i;
            }
        }
        if (best != spare_pools_.end()) {
            const auto out = *best;
            spare_pools_.erase(best);
            check(d.vkd.vkResetDescriptorPool(d.device, out.first, 0), "vkResetDescriptorPool");
            return out;
        }
        auto capacity = std::uint32_t(limits::descriptor_pool_min_sets);
        while (capacity < sets) {
            capacity *= 2;
        }
        return {create_scene_pool(d, capacity), capacity};
    }

    VkDeviceSize Renderer::largest_spare() const {
        VkDeviceSize out{};
        for (const auto* spares : {&spare_staging_, &spare_records_, &spare_clips_}) {
            for (const auto& b : *spares) {
                out = std::max(out, b.size);
            }
        }
        return out;
    }

    void Renderer::recycle(FrameResources& r) noexcept {
        const auto& d = gpu;
        auto keep = [&](std::vector<Buffer>& spares, Buffer& b) {
            if (b.buffer && spares.size() < spares.capacity() && b.size <= limits::spare_buffer_max_bytes) {
                spares.push_back(b);
            } else {
                d.destroy(b);
            }
            b = {};
        };
        keep(spare_staging_, r.staging);
        keep(spare_records_, r.records);
        keep(spare_clips_, r.clips);
        if (r.pool && spare_pools_.size() < spare_pools_.capacity()) {
            spare_pools_.emplace_back(r.pool, r.pool_sets);
        } else if (r.pool) {
            d.vkd.vkDestroyDescriptorPool(d.device, r.pool, nullptr);
        }
        r.pool = {};
        r.pool_sets = 0;
        if (r.framebuffer) {
            d.vkd.vkDestroyFramebuffer(d.device, r.framebuffer, nullptr);
            r.framebuffer = {};
        }
    }

    std::vector<Pixel> Renderer::draw(const paint::DisplayList& list) {
        const auto& d = gpu;
        const auto stats = tracker.Stats();
        ensure(!stats.prepared_frames && !stats.encoded_tokens && !stats.submitted_tokens,
               "draw() numbers the renderer's submissions itself");
        if (!readback_.buffer) {
            readback_ = d.buffer(VkDeviceSize(width) * height * sizeof(Pixel), VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        }
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
