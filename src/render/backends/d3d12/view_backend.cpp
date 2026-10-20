#include "render/backends/d3d12/view_backend.hpp"

#include "constants/render.hpp"
#include "render/srgb.hpp"
#include "view/view_abi.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>

namespace weft::render::d3d12 {
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

        // The format a view of a host texture of target format reads it as, and the typeless one of its resource.
        DXGI_FORMAT view_format(std::uint32_t format) {
            switch (format) {
                case WEFT_RENDER_TARGET_LINEAR_RGBA8:
                    return DXGI_FORMAT_R8G8B8A8_UNORM;
                case WEFT_RENDER_TARGET_SRGB_RGBA8:
                    return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                case WEFT_RENDER_TARGET_LINEAR_BGRA8:
                    return DXGI_FORMAT_B8G8R8A8_UNORM;
                case WEFT_RENDER_TARGET_SRGB_BGRA8:
                    return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
                case WEFT_RENDER_TARGET_LINEAR_RGBA16F:
                    return DXGI_FORMAT_R16G16B16A16_FLOAT;
                case WEFT_RENDER_TARGET_LINEAR_RGBA32F:
                    return DXGI_FORMAT_R32G32B32A32_FLOAT;
                default:
                    return DXGI_FORMAT_UNKNOWN;
            }
        }

        DXGI_FORMAT typeless(DXGI_FORMAT format) {
            switch (format) {
                case DXGI_FORMAT_R8G8B8A8_UNORM:
                case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                    return DXGI_FORMAT_R8G8B8A8_TYPELESS;
                case DXGI_FORMAT_B8G8R8A8_UNORM:
                case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                    return DXGI_FORMAT_B8G8R8A8_TYPELESS;
                case DXGI_FORMAT_R16G16B16A16_FLOAT:
                    return DXGI_FORMAT_R16G16B16A16_TYPELESS;
                case DXGI_FORMAT_R32G32B32A32_FLOAT:
                    return DXGI_FORMAT_R32G32B32A32_TYPELESS;
                case DXGI_FORMAT_R10G10B10A2_UNORM:
                    return DXGI_FORMAT_R10G10B10A2_TYPELESS;
                default:
                    return DXGI_FORMAT_UNKNOWN;
            }
        }

        // Whether resource was made by device.
        bool owned_by(ID3D12DeviceChild* child, ID3D12Device* device) {
            ComPtr<ID3D12Device> owner;
            return SUCCEEDED(child->GetDevice(IID_PPV_ARGS(&owner))) && owner.Get() == device;
        }
    } // namespace

    ViewBackend::ViewBackend(ID3D12Device* device, std::uint64_t queue)
        : device_(std::make_unique<Device>(device)), imports_(std::size_t(constants::render::view_external_textures)) {
        BackendDescription description;
        description.api = WEFT_RENDER_API_D3D12;
        description.device_epoch = 1;
        description.queue = queue;
        description.capabilities = {sizeof(weft_render_capabilities),
                                    WEFT_RENDER_FEATURE_SCENE,
                                    (1u << WEFT_RENDER_TARGET_LINEAR_RGBA8) | (1u << WEFT_RENDER_TARGET_LINEAR_BGRA8) |
                                        (1u << WEFT_RENDER_TARGET_LINEAR_RGB10A2),
                                    std::uint32_t(constants::render::d3d12::max_surface_dimension),
                                    std::uint32_t(constants::render::d3d12::max_surface_dimension),
                                    0};
        table_ = make_c_backend(*this, description);
    }

    ViewBackend::~ViewBackend() {
        shutdown();
    }

    weft_render_external_backend ViewBackend::external() {
        // Every device samples and filters 8-bit RGBA and BGRA, linear or sRGB, and RGBA16F; RGBA32F where it says so.
        std::uint32_t formats = (1u << WEFT_RENDER_TARGET_LINEAR_RGBA8) | (1u << WEFT_RENDER_TARGET_SRGB_RGBA8) |
                                (1u << WEFT_RENDER_TARGET_LINEAR_BGRA8) | (1u << WEFT_RENDER_TARGET_SRGB_BGRA8) |
                                (1u << WEFT_RENDER_TARGET_LINEAR_RGBA16F);
        if (device_->filters(DXGI_FORMAT_R32G32B32A32_FLOAT)) {
            formats |= 1u << WEFT_RENDER_TARGET_LINEAR_RGBA32F;
        }
        return make_c_external_backend(
            *this, {formats, std::uint32_t(imports_.size()), constants::render::view_external_texture_bytes});
    }

    weft_render_result ViewBackend::import_texture(const weft_render_external_texture& d, std::uint64_t& out) {
        out = 0;
        if (d.native_texture_size < sizeof(weft_render_d3d12_texture)) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        const auto& p = *static_cast<const weft_render_d3d12_texture*>(d.native_texture);
        if (p.struct_size < sizeof(p) || p.abi_version != WEFT_RENDER_EXTERNAL_ABI_VERSION || p.reserved ||
            !p.resource) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        auto* resource = static_cast<ID3D12Resource*>(p.resource);
        if (!owned_by(resource, device_->device.Get())) {
            return WEFT_RENDER_WRONG_BACKEND;
        }
        const auto format = view_format(d.format);
        if (format == DXGI_FORMAT_UNKNOWN ||
            (d.format == WEFT_RENDER_TARGET_LINEAR_RGBA32F && !device_->filters(DXGI_FORMAT_R32G32B32A32_FLOAT))) {
            return WEFT_RENDER_UNSUPPORTED_FEATURE;
        }
        const auto actual = resource->GetDesc();
        const UINT levels = std::max(1u, d.mip_levels);
        if (actual.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || actual.DepthOrArraySize != 1 ||
            actual.SampleDesc.Count != 1 || actual.MipLevels < levels ||
            (actual.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
            // The view's format, or the typeless one (an sRGB view of a texture another view reads as UNORM).
            (actual.Format != format && actual.Format != typeless(format))) {
            return WEFT_RENDER_UNSUPPORTED_FEATURE;
        }
        if (actual.Width != d.width || actual.Height != d.height) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        // One import of a resource at a time, live or frozen.
        for (const auto& i : imports_) {
            if (i.resource.Get() == resource) {
                return WEFT_RENDER_INVALID_STATE;
            }
        }
        for (std::size_t i = 0; i < imports_.size(); ++i) {
            if (!imports_[i].resource) {
                imports_[i] = {resource, {resource, format, levels}};
                out = i + 1;
                return WEFT_RENDER_OK;
            }
        }
        return WEFT_RENDER_BUSY;
    }

    void ViewBackend::release_texture(std::uint64_t native) {
        if (!native || native > imports_.size() || !imports_[native - 1].resource) {
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
        // One runtime's fonts: its Views lay text out with them, the pass shapes it with them again.
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
            if (!token || token > imports_.size() || !imports_[token - 1].resource) {
                return WEFT_RENDER_STALE_HANDLE;
            }
            const auto& bound = frame.textures[i];
            const auto& imported = imports_[token - 1];
            prepared.textures.push_back({std::string(bound.name, bound.name_size),
                                         std::uint64_t(reinterpret_cast<std::uintptr_t>(&imported.view)),
                                         textures[i].description.origin == WEFT_RENDER_EXTERNAL_BOTTOM_LEFT});
            prepared.resources.push_back(imported.resource);
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
        const auto found = prepared_.find(native);
        if (found == prepared_.end() || !pass.native_pass || pass.native_pass_size < sizeof(weft_render_d3d12_pass) ||
            pass.sample_count > 1) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        const auto& p = *static_cast<const weft_render_d3d12_pass*>(pass.native_pass);
        if (p.struct_size < sizeof(p) || p.abi_version != WEFT_RENDER_ABI_VERSION || p.reserved || p.clear_target > 1 ||
            !p.command_list || !p.target) {
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
        auto* command = static_cast<ID3D12GraphicsCommandList*>(p.command_list);
        auto* target = static_cast<ID3D12Resource*>(p.target);
        if (!owned_by(command, device_->device.Get()) || !owned_by(target, device_->device.Get())) {
            return WEFT_RENDER_WRONG_BACKEND;
        }
        if (command->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        const auto format = pass.target_format == WEFT_RENDER_TARGET_LINEAR_BGRA8     ? DXGI_FORMAT_B8G8R8A8_UNORM
                            : pass.target_format == WEFT_RENDER_TARGET_LINEAR_RGBA8   ? DXGI_FORMAT_R8G8B8A8_UNORM
                            : pass.target_format == WEFT_RENDER_TARGET_LINEAR_RGB10A2 ? DXGI_FORMAT_R10G10B10A2_UNORM
                                                                                      : DXGI_FORMAT_UNKNOWN;
        failure_.clear();
        if (format == DXGI_FORMAT_UNKNOWN) {
            failure_ = "a target format other than linear BGRA8, RGBA8 or RGB10A2";
            return WEFT_RENDER_UNSUPPORTED_FEATURE;
        }
        if (!pass.width || !pass.height || pass.width > unsigned(constants::render::d3d12::max_surface_dimension) ||
            pass.height > unsigned(constants::render::d3d12::max_surface_dimension)) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        const auto actual = target->GetDesc();
        if (actual.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || actual.DepthOrArraySize != 1 ||
            actual.MipLevels != 1 || actual.SampleDesc.Count != 1 ||
            !(actual.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
            (actual.Format != format && actual.Format != typeless(format))) {
            failure_ = "a target that is no single-sample render target 2D texture of the pass's format";
            return WEFT_RENDER_UNSUPPORTED_FEATURE;
        }
        if (actual.Width < pass.width || actual.Height < pass.height) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        const auto& prepared = found->second;
        // A pass reads no texture it draws into, as on D3D11.
        for (const auto& resource : prepared.resources) {
            if (resource.Get() == target) {
                return WEFT_RENDER_INVALID_STATE;
            }
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
        std::unique_ptr<SceneRenderer::Slot> slot;
        if (!idle_.empty()) {
            slot = std::move(idle_.back());
            idle_.pop_back();
        } else {
            slot = std::make_unique<SceneRenderer::Slot>();
        }
        bool drawn{};
        try {
            drawn = renderer_->encode(*slot, *frame.scene, command, {target, format, pass.width, pass.height},
                                      prepared.scale, index, anchors, start, prepared.textures);
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
        // The host textures the recording samples live until it completes.
        for (const auto& resource : prepared.resources) {
            slot->held.push_back(resource);
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
} // namespace weft::render::d3d12

struct weft_render_d3d12 {
    weft::render::d3d12::ViewBackend backend;
    // Whether the backend initialized COM on its owner thread (its reference raster's DirectWrite and WIC).
    bool com{};

    weft_render_d3d12(ID3D12Device* device, std::uint64_t queue) : backend(device, queue) {
    }
};

extern "C" weft_render_result weft_render_d3d12_create(const weft_render_d3d12_desc* desc, weft_render_d3d12** out,
                                                       weft_render_backend* table) noexcept {
    if (!out || !table) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    *out = nullptr;
    *table = {};
    if (!desc || desc->struct_size < sizeof(*desc) || !desc->device || !desc->queue || desc->reserved) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    if (desc->abi_version != WEFT_RENDER_ABI_VERSION) {
        return WEFT_RENDER_UNSUPPORTED_ABI;
    }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        auto* created = new weft_render_d3d12(static_cast<ID3D12Device*>(desc->device), desc->queue);
        created->com = SUCCEEDED(com);
        *table = created->backend.table();
        *out = created;
        return WEFT_RENDER_OK;
    } catch (const std::bad_alloc&) {
        if (SUCCEEDED(com)) {
            CoUninitialize();
        }
        return WEFT_RENDER_OUT_OF_MEMORY;
    } catch (...) {
        if (SUCCEEDED(com)) {
            CoUninitialize();
        }
        return WEFT_RENDER_DEVICE_UNAVAILABLE;
    }
}

extern "C" weft_render_result weft_render_d3d12_external(weft_render_d3d12* backend,
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

extern "C" void weft_render_d3d12_destroy(weft_render_d3d12* backend) noexcept {
    if (!backend) {
        return;
    }
    const bool com = backend->com;
    delete backend;
    if (com) {
        CoUninitialize();
    }
}
