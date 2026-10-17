#include "render/backends/d3d11/view_backend.hpp"

#include "constants/render.hpp"
#include "render/srgb.hpp"
#include "view/view_abi.hpp"

#include <algorithm>
#include <exception>
#include <string>
#include <cmath>
#include <new>
#include <optional>
#include <stdexcept>

namespace weft::render::d3d11 {
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

    ViewBackend::ViewBackend(ID3D11Device* device, std::uint64_t queue)
        : device_(device), imports_(std::size_t(constants::render::view_external_textures)) {
        ensure(device_ != nullptr, "a D3D11 device");
        BackendDescription description;
        description.api = WEFT_RENDER_API_D3D11;
        description.device_epoch = 1;
        description.queue = queue;
        description.capabilities = {sizeof(weft_render_capabilities),
                                    WEFT_RENDER_FEATURE_SCENE,
                                    (1u << WEFT_RENDER_TARGET_LINEAR_RGBA8) | (1u << WEFT_RENDER_TARGET_LINEAR_BGRA8),
                                    std::uint32_t(constants::render::d3d11::max_surface_dimension),
                                    std::uint32_t(constants::render::d3d11::max_surface_dimension),
                                    0};
        table_ = make_c_backend(*this, description);
    }

    ViewBackend::~ViewBackend() = default;

    weft_render_external_backend ViewBackend::external() {
        // Every device samples and filters 8-bit RGBA and BGRA, linear or sRGB, and RGBA16F; RGBA32F where it says so.
        std::uint32_t formats = (1u << WEFT_RENDER_TARGET_LINEAR_RGBA8) | (1u << WEFT_RENDER_TARGET_SRGB_RGBA8) |
                                (1u << WEFT_RENDER_TARGET_LINEAR_BGRA8) | (1u << WEFT_RENDER_TARGET_SRGB_BGRA8) |
                                (1u << WEFT_RENDER_TARGET_LINEAR_RGBA16F);
        UINT support{};
        if (SUCCEEDED(device_->CheckFormatSupport(DXGI_FORMAT_R32G32B32A32_FLOAT, &support)) &&
            (support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)) {
            formats |= 1u << WEFT_RENDER_TARGET_LINEAR_RGBA32F;
        }
        return make_c_external_backend(
            *this, {formats, std::uint32_t(imports_.size()), constants::render::view_external_texture_bytes});
    }

    weft_render_result ViewBackend::import_texture(const weft_render_external_texture& description,
                                                   std::uint64_t& out) {
        out = 0;
        ComPtr<ID3D11ShaderResourceView> view;
        ComPtr<ID3D11Resource> resource;
        if (const auto v = check_external_texture(description, device_.Get(), view, resource); v != WEFT_RENDER_OK) {
            return v;
        }
        // One import of a resource at a time, live or frozen.
        for (const auto& i : imports_) {
            if (i.resource.Get() == resource.Get()) {
                return WEFT_RENDER_INVALID_STATE;
            }
        }
        for (std::size_t i = 0; i < imports_.size(); ++i) {
            if (!imports_[i].view) {
                imports_[i] = {std::move(view), std::move(resource)};
                out = i + 1;
                return WEFT_RENDER_OK;
            }
        }
        return WEFT_RENDER_BUSY;
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
        // One runtime's fonts: its Views lay text out with them, the pass shapes it with them again.
        if (!fonts_) {
            fonts_ = held->fonts;
            text_ = std::make_unique<SceneText>(*fonts_);
            renderer_ = std::make_unique<SceneRenderer>(device_.Get(), *text_);
        } else if (held->fonts != fonts_) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        // Views that are gone: their slots retire, the renderer forgets them.
        for (auto it = views_.begin(); it != views_.end();) {
            if (it->first.expired()) {
                renderer_->retire(it->second.slot);
                renderer_->forget(it->second.index);
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
            const auto& import = imports_[token - 1];
            prepared.textures.push_back({std::string(bound.name, bound.name_size),
                                         std::uint64_t(reinterpret_cast<std::uintptr_t>(import.view.Get())),
                                         textures[i].description.origin == WEFT_RENDER_EXTERNAL_BOTTOM_LEFT});
            prepared.resources.push_back(import.resource.Get());
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
        if (found == prepared_.end() || !pass.native_pass || pass.native_pass_size < sizeof(weft_render_d3d11_pass) ||
            pass.sample_count > 1) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        const auto& p = *static_cast<const weft_render_d3d11_pass*>(pass.native_pass);
        if (p.struct_size < sizeof(p) || p.abi_version != WEFT_RENDER_ABI_VERSION || p.clear_target > 1 || p.reserved) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        if (p.out_command_list) {
            *p.out_command_list = nullptr;
        }
        failure_.clear();
        // The immediate context draws; a deferred one is the display list backend's.
        if (p.deferred_context || !p.render_target_view) {
            failure_ = "a deferred context or no render target view";
            return WEFT_RENDER_UNSUPPORTED_FEATURE;
        }
        if (p.clear_target) {
            for (std::size_t i = 0; i < 4; ++i) {
                const float c = p.clear_linear_premultiplied[i];
                if (!std::isfinite(c) || c < 0 || c > 1 || (i < 3 && c > p.clear_linear_premultiplied[3])) {
                    return WEFT_RENDER_INVALID_ARGUMENT;
                }
            }
        }
        auto* target = static_cast<ID3D11RenderTargetView*>(p.render_target_view);
        ComPtr<ID3D11Device> target_device;
        target->GetDevice(&target_device);
        if (target_device.Get() != device_.Get()) {
            return WEFT_RENDER_WRONG_BACKEND;
        }
        D3D11_RENDER_TARGET_VIEW_DESC view{};
        target->GetDesc(&view);
        const auto format = pass.target_format == WEFT_RENDER_TARGET_LINEAR_BGRA8   ? DXGI_FORMAT_B8G8R8A8_UNORM
                            : pass.target_format == WEFT_RENDER_TARGET_LINEAR_RGBA8 ? DXGI_FORMAT_R8G8B8A8_UNORM
                                                                                    : DXGI_FORMAT_UNKNOWN;
        if (format == DXGI_FORMAT_UNKNOWN || view.Format != format ||
            view.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || view.Texture2D.MipSlice) {
            failure_ = "a target that is no first mip of a 2D texture of the pass's format";
            return WEFT_RENDER_UNSUPPORTED_FEATURE;
        }
        ComPtr<ID3D11Resource> resource;
        target->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(resource.As(&texture))) {
            failure_ = "a target that is no 2D texture";
            return WEFT_RENDER_UNSUPPORTED_FEATURE;
        }
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        if (!pass.width || !pass.height || description.Width != pass.width || description.Height != pass.height ||
            description.SampleDesc.Count != 1) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        const auto& prepared = found->second;
        // D3D11 reads no resource of a pass that draws into it.
        if (std::find(prepared.resources.begin(), prepared.resources.end(), resource.Get()) !=
            prepared.resources.end()) {
            return WEFT_RENDER_INVALID_STATE;
        }
        const auto& frame = *prepared.frame;
        auto& state = views_[std::weak_ptr<SceneAnchorChannel>(frame.anchors)];
        if (!state.index) {
            state.index = ++next_view_;
        }
        const auto* anchors = &frame.anchors->take(SceneAnchorChannel::Reader::Render);
        SceneTargetStart start{true, std::nullopt};
        if (p.clear_target) {
            start.clear = encoded(p.clear_linear_premultiplied);
        }
        if (!renderer_->paint(state.slot, *frame.scene, target, prepared.scale, pass.width, pass.height, std::nullopt,
                              state.index, {}, anchors, start, prepared.textures)) {
            failure_ = "the native pass cannot draw the frame: " + renderer_->last_failure();
            return WEFT_RENDER_UNSUPPORTED_FEATURE;
        }
        out = next_++;
        uses_.insert(out);
        return WEFT_RENDER_OK;
    }

    void ViewBackend::discard_encoded(std::uint64_t native) {
        // The immediate context has the commands already; nothing waits on them.
        uses_.erase(native);
    }

    void ViewBackend::submitted(std::uint64_t native, std::uint64_t) {
        uses_.erase(native);
    }

    void ViewBackend::completed(std::uint64_t) {
    }

    void ViewBackend::abandon() {
        shutdown();
    }

    weft_render_result ViewBackend::shutdown() {
        prepared_.clear();
        uses_.clear();
        for (auto& [channel, state] : views_) {
            renderer_->retire(state.slot);
            renderer_->forget(state.index);
        }
        views_.clear();
        return WEFT_RENDER_OK;
    }
} // namespace weft::render::d3d11

struct weft_render_d3d11 {
    weft::render::d3d11::ViewBackend backend;
    // This thread's COM, which the scene pass's reference raster needs, when the backend initialized it.
    bool com{};

    weft_render_d3d11(ID3D11Device* device, std::uint64_t queue) : backend(device, queue) {
    }
};

extern "C" weft_render_result weft_render_d3d11_create(const weft_render_d3d11_desc* desc, weft_render_d3d11** out,
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
        auto* created = new weft_render_d3d11(static_cast<ID3D11Device*>(desc->device), desc->queue);
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
        return WEFT_RENDER_BACKEND_ERROR;
    }
}

extern "C" weft_render_result weft_render_d3d11_external(weft_render_d3d11* backend,
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

extern "C" void weft_render_d3d11_destroy(weft_render_d3d11* backend) noexcept {
    if (!backend) {
        return;
    }
    const bool com = backend->com;
    delete backend;
    if (com) {
        CoUninitialize();
    }
}
