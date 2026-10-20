#include "render/backends/vulkan/view_backend.hpp"

#include "constants/render.hpp"
#include "render/backends/vulkan/renderer.hpp"
#include "render/srgb.hpp"
#include "view/view_abi.hpp"

#include <algorithm>
#include <exception>
#include <string>
#include <cmath>
#include <cstddef>
#include <new>
#include <optional>
#include <stdexcept>

namespace weft::render::vulkan {
    namespace {
        // A linear premultiplied color as the encoded premultiplied value a scene pass writes.
        paint::Color encoded(const float* linear) {
            const float a = linear[3];
            if (a <= 0) {
                return {};
            }
            return {linear_to_srgb(linear[0] / a) * a, linear_to_srgb(linear[1] / a) * a,
                    linear_to_srgb(linear[2] / a) * a, a};
        }

    } // namespace

    ViewBackend::ViewBackend(const HostDevice& host, std::uint64_t queue)
        : device_(Device::adopt(host)), imports_(std::size_t(constants::render::view_external_textures)) {
        BackendDescription description;
        description.api = WEFT_RENDER_API_VULKAN;
        description.device_epoch = 1;
        description.queue = queue;
        description.capabilities = {sizeof(weft_render_capabilities),
                                    WEFT_RENDER_FEATURE_SCENE,
                                    1u << WEFT_RENDER_TARGET_LINEAR_BGRA8,
                                    std::uint32_t(constants::render::vulkan::max_surface_dimension),
                                    std::uint32_t(constants::render::vulkan::max_surface_dimension),
                                    0};
        table_ = make_c_backend(*this, description);
    }

    ViewBackend::~ViewBackend() {
        shutdown();
    }

    weft_render_external_backend ViewBackend::external() {
        // Every device samples and filters 8-bit RGBA and BGRA, linear or sRGB, and RGBA16F (Vulkan's required
        // formats); RGBA32F where it says so.
        std::uint32_t formats = (1u << WEFT_RENDER_TARGET_LINEAR_RGBA8) | (1u << WEFT_RENDER_TARGET_SRGB_RGBA8) |
                                (1u << WEFT_RENDER_TARGET_LINEAR_BGRA8) | (1u << WEFT_RENDER_TARGET_SRGB_BGRA8) |
                                (1u << WEFT_RENDER_TARGET_LINEAR_RGBA16F);
        if (device_->filters(VK_FORMAT_R32G32B32A32_SFLOAT)) {
            formats |= 1u << WEFT_RENDER_TARGET_LINEAR_RGBA32F;
        }
        return make_c_external_backend(
            *this, {formats, std::uint32_t(imports_.size()), constants::render::view_external_texture_bytes});
    }

    weft_render_result ViewBackend::import_texture(const weft_render_external_texture& description,
                                                   std::uint64_t& out) {
        out = 0;
        Import imported;
        if (const auto v = check_external_texture(description, device_->device, imported.view, imported.image);
            v != WEFT_RENDER_OK) {
            return v;
        }
        // One import of a view at a time, live or frozen.
        const auto viewing = [](VkImageView view) {
            return [view](const Import& i) {
                return i.view == view;
            };
        };
        if (std::find_if(imports_.begin(), imports_.end(), viewing(imported.view)) != imports_.end()) {
            return WEFT_RENDER_INVALID_STATE;
        }
        const auto free = std::find_if(imports_.begin(), imports_.end(), viewing(VkImageView{}));
        if (free == imports_.end()) {
            return WEFT_RENDER_BUSY;
        }
        *free = imported;
        out = std::uint64_t(free - imports_.begin()) + 1;
        return WEFT_RENDER_OK;
    }

    void ViewBackend::release_texture(std::uint64_t native) {
        if (!native || native > imports_.size() || !imports_[native - 1].view) {
            std::terminate();
        }
        imports_[native - 1] = {};
    }

    weft_render_result ViewBackend::prepare_external(const weft_render_frame&, const weft_render_external_asset*,
                                                     std::uint32_t, std::uint64_t& out) {
        out = 0;
        failure_ = "quad frames are not a View backend's";
        return WEFT_RENDER_UNSUPPORTED_FEATURE;
    }

    weft_render_result ViewBackend::prepare(const weft_render_frame&, std::uint64_t& out) {
        out = 0;
        failure_ = "quad frames are not a View backend's";
        return WEFT_RENDER_UNSUPPORTED_FEATURE;
    }

    weft_render_result ViewBackend::prepare_scene(const weft_render_scene_frame& frame,
                                                  const weft_render_external_asset* textures, std::uint32_t count,
                                                  std::uint64_t& out) {
        out = 0;
        const auto& held = frame.frame->frame;
        if (!held || !held->scene || !held->fonts || !held->anchors) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        if (!fonts_) {
            fonts_ = held->fonts;
            text_ = std::make_unique<SceneText>(*fonts_);
            renderer_ = std::make_unique<SceneRenderer>(*device_, *text_);
        } else if (held->fonts != fonts_) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        // Views that are gone: the renderer forgets them.
        for (auto it = views_.begin(); it != views_.end();) {
            if (it->first.expired()) {
                renderer_->forget(it->second);
                it = views_.erase(it);
            } else {
                ++it;
            }
        }
        Prepared prepared{held, frame.scale ? frame.scale : held->scale, {}, {}};
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto token = textures[i].native_texture;
            if (!token || token > imports_.size() || !imports_[token - 1].view) {
                return WEFT_RENDER_STALE_HANDLE;
            }
            const auto& bound = frame.textures[i];
            const auto& imported = imports_[token - 1];
            prepared.textures.push_back({std::string(bound.name, bound.name_size), abi_handle(imported.view),
                                         textures[i].description.origin == WEFT_RENDER_EXTERNAL_BOTTOM_LEFT});
            if (imported.image) {
                prepared.images.push_back(imported.image);
            }
        }
        out = next_++;
        prepared_.emplace(out, std::move(prepared));
        return WEFT_RENDER_OK;
    }

    void ViewBackend::release_prepared(std::uint64_t native) {
        prepared_.erase(native);
    }

    weft_render_result ViewBackend::encode(std::uint64_t native, const weft_render_pass& pass, std::uint64_t& out) {
        out = 0;
        constexpr auto v1 = offsetof(weft_render_vulkan_pass, target_image);
        const auto found = prepared_.find(native);
        if (found == prepared_.end() || !pass.native_pass || pass.native_pass_size < v1 || pass.sample_count > 1) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        const auto& p = *static_cast<const weft_render_vulkan_pass*>(pass.native_pass);
        if (p.struct_size < v1 || p.abi_version != WEFT_RENDER_ABI_VERSION || p.reserved || p.clear_target > 1 ||
            !p.device || !p.command_buffer || !p.target_view) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        if (p.clear_target) {
            for (std::size_t i = 0; i < 4; ++i) {
                const float c = p.clear_linear_premultiplied[i];
                if (!std::isfinite(c) || c < 0 || c > 1 || (i < 3 && c > p.clear_linear_premultiplied[3])) {
                    return WEFT_RENDER_INVALID_ARGUMENT;
                }
            }
        }
        if (native_handle<VkDevice>(p.device) != device_->device) {
            return WEFT_RENDER_WRONG_BACKEND;
        }
        failure_.clear();
        if (pass.target_format != WEFT_RENDER_TARGET_LINEAR_BGRA8) {
            failure_ = "a target format other than linear BGRA8";
            return WEFT_RENDER_UNSUPPORTED_FEATURE;
        }
        if (!pass.width || !pass.height) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        const auto& prepared = found->second;
        const auto target_image =
            p.struct_size >= sizeof(p) && p.target_image ? native_handle<VkImage>(p.target_image) : VkImage{};
        // A pass reads no image it draws into, as on D3D11: the host named both.
        if (target_image &&
            std::find(prepared.images.begin(), prepared.images.end(), target_image) != prepared.images.end()) {
            return WEFT_RENDER_INVALID_STATE;
        }
        const auto& frame = *prepared.frame;
        auto& index = views_[std::weak_ptr<SceneAnchorChannel>(frame.anchors)];
        if (!index) {
            index = ++next_view_;
        }
        const auto* anchors = &frame.anchors->take(SceneAnchorChannel::Reader::Render);
        SceneTargetStart start{true, std::nullopt};
        if (p.clear_target) {
            start.clear = encoded(p.clear_linear_premultiplied);
        }
        const SceneRenderer::Target target{native_handle<VkImageView>(p.target_view), pass.width, pass.height,
                                           target_image};
        std::unique_ptr<SceneRenderer::Slot> slot;
        if (!idle_.empty()) {
            slot = std::move(idle_.back());
            idle_.pop_back();
        } else {
            slot = std::make_unique<SceneRenderer::Slot>();
        }
        bool drawn{};
        try {
            drawn = renderer_->encode(*slot, *frame.scene, native_handle<VkCommandBuffer>(p.command_buffer), target,
                                      prepared.scale, std::nullopt, index, {}, anchors, start, prepared.textures);
        } catch (const Busy&) {
            idle_.push_back(std::move(slot));
            return WEFT_RENDER_BUSY;
        } catch (const std::exception& e) {
            // The host's log says why (last_failure); the thunk makes it WEFT_RENDER_BACKEND_ERROR.
            failure_ = std::string("the native pass failed: ") + e.what();
            renderer_->discard(*slot);
            idle_.push_back(std::move(slot));
            throw;
        } catch (...) {
            renderer_->discard(*slot);
            idle_.push_back(std::move(slot));
            throw;
        }
        if (!drawn) {
            idle_.push_back(std::move(slot));
            failure_ = "the native pass cannot draw the frame: " + renderer_->last_failure();
            return WEFT_RENDER_UNSUPPORTED_FEATURE;
        }
        out = next_++;
        uses_.emplace(out, Use{std::move(slot)});
        return WEFT_RENDER_OK;
    }

    void ViewBackend::discard_encoded(std::uint64_t native) {
        if (const auto found = uses_.find(native); found != uses_.end()) {
            renderer_->discard(*found->second.slot);
            idle_.push_back(std::move(found->second.slot));
            uses_.erase(found);
        }
    }

    void ViewBackend::submitted(std::uint64_t native, std::uint64_t serial) {
        if (const auto found = uses_.find(native); found != uses_.end()) {
            renderer_->submitted(*found->second.slot);
            found->second.serial = serial;
            found->second.submitted = true;
        }
    }

    void ViewBackend::completed(std::uint64_t serial) {
        for (auto it = uses_.begin(); it != uses_.end();) {
            if (it->second.submitted && it->second.serial <= serial) {
                renderer_->retire(*it->second.slot);
                idle_.push_back(std::move(it->second.slot));
                it = uses_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void ViewBackend::abandon() {
        // The host vouches that the device runs none of the recordings any more.
        for (auto& [native, use] : uses_) {
            if (use.submitted) {
                renderer_->retire(*use.slot);
            } else {
                renderer_->discard(*use.slot);
            }
            idle_.push_back(std::move(use.slot));
        }
        uses_.clear();
    }

    weft_render_result ViewBackend::shutdown() {
        if (!uses_.empty()) {
            return WEFT_RENDER_BUSY;
        }
        prepared_.clear();
        idle_.clear();
        if (renderer_) {
            for (const auto& [channel, index] : views_) {
                renderer_->forget(index);
            }
        }
        views_.clear();
        return WEFT_RENDER_OK;
    }
} // namespace weft::render::vulkan

struct weft_render_vulkan {
    weft::render::vulkan::ViewBackend backend;

    weft_render_vulkan(const weft::render::vulkan::HostDevice& host, std::uint64_t queue) : backend(host, queue) {
    }
};

extern "C" weft_render_result weft_render_vulkan_create(const weft_render_vulkan_desc* desc, weft_render_vulkan** out,
                                                        weft_render_backend* table) noexcept {
    if (!out || !table) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    *out = nullptr;
    *table = {};
    if (!desc || desc->struct_size < sizeof(*desc) || !desc->instance || !desc->physical_device || !desc->device ||
        !desc->queue || !desc->get_instance_proc_addr || !desc->queue_identity || desc->reserved) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    if (desc->abi_version != WEFT_RENDER_ABI_VERSION) {
        return WEFT_RENDER_UNSUPPORTED_ABI;
    }
    try {
        weft::render::vulkan::HostDevice host;
        host.instance = reinterpret_cast<VkInstance>(static_cast<std::uintptr_t>(desc->instance));
        host.physical = reinterpret_cast<VkPhysicalDevice>(static_cast<std::uintptr_t>(desc->physical_device));
        host.device = reinterpret_cast<VkDevice>(static_cast<std::uintptr_t>(desc->device));
        host.queue = reinterpret_cast<VkQueue>(static_cast<std::uintptr_t>(desc->queue));
        host.queue_family = desc->queue_family;
        host.get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(desc->get_instance_proc_addr);
        auto* created = new weft_render_vulkan(host, desc->queue_identity);
        *table = created->backend.table();
        *out = created;
        return WEFT_RENDER_OK;
    } catch (const std::bad_alloc&) {
        return WEFT_RENDER_OUT_OF_MEMORY;
    } catch (const std::invalid_argument&) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    } catch (...) {
        return WEFT_RENDER_DEVICE_UNAVAILABLE;
    }
}

extern "C" weft_render_result weft_render_vulkan_external(weft_render_vulkan* backend,
                                                          weft_render_external_backend* out) noexcept {
    if (!out) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    *out = {};
    if (!backend) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    *out = backend->backend.external();
    return WEFT_RENDER_OK;
}

extern "C" void weft_render_vulkan_destroy(weft_render_vulkan* backend) noexcept {
    delete backend;
}
