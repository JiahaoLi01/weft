#import "render/backends/metal/view_backend.hpp"

#import "render/backends/metal/scene_internal.hpp"

#include "constants/render.hpp"
#include "render/backends/metal/shaders.hpp"
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

namespace weft::render::metal {
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

        // The pixel format of an external texture of a weft_render_target_format the extension lists; Invalid for
        // others.
        MTLPixelFormat external_format(std::uint32_t format) {
            switch (format) {
                case WEFT_RENDER_TARGET_LINEAR_RGBA8:
                    return MTLPixelFormatRGBA8Unorm;
                case WEFT_RENDER_TARGET_SRGB_RGBA8:
                    return MTLPixelFormatRGBA8Unorm_sRGB;
                case WEFT_RENDER_TARGET_LINEAR_BGRA8:
                    return MTLPixelFormatBGRA8Unorm;
                case WEFT_RENDER_TARGET_SRGB_BGRA8:
                    return MTLPixelFormatBGRA8Unorm_sRGB;
                case WEFT_RENDER_TARGET_LINEAR_RGBA16F:
                    return MTLPixelFormatRGBA16Float;
                case WEFT_RENDER_TARGET_LINEAR_RGBA32F:
                    return MTLPixelFormatRGBA32Float;
                default:
                    break;
            }
            return MTLPixelFormatInvalid;
        }

        // The pixel format of an export image of a weft_render_target_format the export extension lists: what the
        // resolve writes linear and sampling returns linear, filtered between levels; Invalid for others.
        MTLPixelFormat export_format(std::uint32_t format) {
            switch (format) {
                case WEFT_RENDER_TARGET_SRGB_RGBA8:
                    return MTLPixelFormatRGBA8Unorm_sRGB;
                case WEFT_RENDER_TARGET_SRGB_BGRA8:
                    return MTLPixelFormatBGRA8Unorm_sRGB;
                case WEFT_RENDER_TARGET_LINEAR_RGBA16F:
                    return MTLPixelFormatRGBA16Float;
                default:
                    break;
            }
            return MTLPixelFormatInvalid;
        }

        template <class T>
        T bridged(void* pointer) {
            return (__bridge T)pointer;
        }

        std::uint64_t pointer_value(id object) {
            return std::uint64_t(reinterpret_cast<std::uintptr_t>((__bridge void*)object));
        }

        id<MTLFunction> compile(id<MTLDevice> device, std::string_view source, const char* name) {
            MTLCompileOptions* options = [MTLCompileOptions new];
            options.languageVersion = MTLLanguageVersion2_1;
            NSError* error = nil;
            NSString* text = [[NSString alloc] initWithBytes:source.data()
                                                      length:source.size()
                                                    encoding:NSUTF8StringEncoding];
            id<MTLLibrary> library = [device newLibraryWithSource:text options:options error:&error];
            ensure(library != nil, "compiling MSL: " + detail::describe(error));
            id<MTLFunction> out = [library newFunctionWithName:@(name)];
            ensure(out != nil, std::string("the MSL function ") + name);
            return out;
        }
    } // namespace

    ViewBackend::ViewBackend(id<MTLDevice> device, std::uint64_t queue)
        : device_(device), imports_(std::size_t(constants::render::view_external_textures)),
          exports_(std::size_t(constants::render::view_export_images)) {
        spare_scratch_.reserve(std::size_t(constants::render::metal::export_spare_scratch));
        BackendDescription description;
        description.api = WEFT_RENDER_API_METAL;
        description.device_epoch = 1;
        description.queue = queue;
        description.capabilities = {sizeof(weft_render_capabilities),
                                    WEFT_RENDER_FEATURE_SCENE,
                                    1u << WEFT_RENDER_TARGET_LINEAR_BGRA8,
                                    std::uint32_t(constants::render::metal::max_surface_dimension),
                                    std::uint32_t(constants::render::metal::max_surface_dimension),
                                    0};
        table_ = make_c_backend(*this, description);
    }

    ViewBackend::~ViewBackend() {
        shutdown();
    }

    weft_render_external_backend ViewBackend::external() {
        // Apple GPUs sample and filter 8-bit RGBA and BGRA, linear or sRGB, and RGBA16F; RGBA32F where they say so.
        std::uint32_t formats = (1u << WEFT_RENDER_TARGET_LINEAR_RGBA8) | (1u << WEFT_RENDER_TARGET_SRGB_RGBA8) |
                                (1u << WEFT_RENDER_TARGET_LINEAR_BGRA8) | (1u << WEFT_RENDER_TARGET_SRGB_BGRA8) |
                                (1u << WEFT_RENDER_TARGET_LINEAR_RGBA16F);
        if ([device_ respondsToSelector:@selector(supports32BitFloatFiltering)] &&
            device_.supports32BitFloatFiltering) {
            formats |= 1u << WEFT_RENDER_TARGET_LINEAR_RGBA32F;
        }
        return make_c_external_backend(
            *this, {formats, std::uint32_t(imports_.size()), constants::render::view_external_texture_bytes});
    }

    weft_render_result ViewBackend::import_texture(const weft_render_external_texture& description,
                                                   std::uint64_t& out) {
        out = 0;
        if (!description.native_texture || description.native_texture_size < sizeof(weft_render_metal_texture)) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        const auto& p = *static_cast<const weft_render_metal_texture*>(description.native_texture);
        if (p.struct_size < sizeof(p) || p.abi_version != WEFT_RENDER_EXTERNAL_ABI_VERSION || !p.texture ||
            p.reserved) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        id<MTLTexture> texture = bridged<id<MTLTexture>>(p.texture);
        if (texture.device != device_) {
            return WEFT_RENDER_WRONG_BACKEND;
        }
        if (texture.textureType != MTLTextureType2D || texture.sampleCount != 1 ||
            texture.width != description.width || texture.height != description.height ||
            texture.pixelFormat != external_format(description.format) ||
            texture.mipmapLevelCount < std::max<NSUInteger>(description.mip_levels, 1) ||
            !(texture.usage & MTLTextureUsageShaderRead)) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        // One import of a texture at a time, live or frozen.
        if (std::find(imports_.begin(), imports_.end(), texture) != imports_.end()) {
            return WEFT_RENDER_INVALID_STATE;
        }
        const auto free = std::find(imports_.begin(), imports_.end(), nil);
        if (free == imports_.end()) {
            return WEFT_RENDER_BUSY;
        }
        *free = texture;
        out = std::uint64_t(free - imports_.begin()) + 1;
        return WEFT_RENDER_OK;
    }

    void ViewBackend::release_texture(std::uint64_t native) {
        if (!native || native > imports_.size() || !imports_[native - 1]) {
            std::terminate();
        }
        imports_[native - 1] = nil;
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
            renderer_ = std::make_unique<SceneRenderer>(device_, *text_);
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
        Prepared prepared{held, frame.scale ? frame.scale : held->scale, {}};
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto token = textures[i].native_texture;
            if (!token || token > imports_.size() || !imports_[token - 1]) {
                return WEFT_RENDER_STALE_HANDLE;
            }
            const auto& bound = frame.textures[i];
            const auto view = std::uint64_t(reinterpret_cast<std::uintptr_t>((__bridge void*)imports_[token - 1]));
            prepared.textures.push_back({std::string(bound.name, bound.name_size), view,
                                         textures[i].description.origin == WEFT_RENDER_EXTERNAL_BOTTOM_LEFT});
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
        if (found == prepared_.end() || !pass.native_pass || pass.native_pass_size < sizeof(weft_render_metal_pass) ||
            pass.sample_count > 1) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        const auto& p = *static_cast<const weft_render_metal_pass*>(pass.native_pass);
        if (p.struct_size < sizeof(p) || p.abi_version != WEFT_RENDER_ABI_VERSION || p.reserved ||
            p.clear_target > 1 || !p.command_buffer || !p.target) {
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
        id<MTLCommandBuffer> command = bridged<id<MTLCommandBuffer>>(p.command_buffer);
        id<MTLTexture> target = bridged<id<MTLTexture>>(p.target);
        if (command.device != device_ || target.device != device_) {
            return WEFT_RENDER_WRONG_BACKEND;
        }
        failure_.clear();
        if (pass.target_format != WEFT_RENDER_TARGET_LINEAR_BGRA8 || target.pixelFormat != MTLPixelFormatBGRA8Unorm) {
            failure_ = "a target format other than linear BGRA8";
            return WEFT_RENDER_UNSUPPORTED_FEATURE;
        }
        if (!pass.width || !pass.height || target.width < pass.width || target.height < pass.height ||
            target.textureType != MTLTextureType2D || target.sampleCount != 1 ||
            !(target.usage & MTLTextureUsageRenderTarget)) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        const auto& prepared = found->second;
        // A pass reads no texture it draws into, as on D3D11: the host named both.
        const auto target_view = std::uint64_t(reinterpret_cast<std::uintptr_t>((__bridge void*)target));
        if (std::any_of(prepared.textures.begin(), prepared.textures.end(), [&](const SceneTexture& t) {
                return t.view == target_view;
            })) {
            return WEFT_RENDER_INVALID_STATE;
        }
        SceneTargetStart start{true, std::nullopt};
        if (p.clear_target) {
            start.clear = encoded(p.clear_linear_premultiplied);
        }
        // A framebufferOnly target cannot be copied from: backdrops at its level draw transparent.
        const SceneRenderer::Target drawn{target, pass.width, pass.height, !target.framebufferOnly};
        std::unique_ptr<SceneRenderer::Slot> slot;
        if (const auto v = draw(prepared, command, drawn, start, slot); v != WEFT_RENDER_OK) {
            return v;
        }
        out = next_++;
        uses_.emplace(out, Use{std::move(slot)});
        return WEFT_RENDER_OK;
    }

    weft_render_result ViewBackend::draw(const Prepared& prepared, id<MTLCommandBuffer> command,
                                         const SceneRenderer::Target& drawn, const SceneTargetStart& start,
                                         std::unique_ptr<SceneRenderer::Slot>& out) {
        const auto& frame = *prepared.frame;
        auto& index = views_[std::weak_ptr<SceneAnchorChannel>(frame.anchors)];
        if (!index) {
            index = ++next_view_;
        }
        const auto* anchors = &frame.anchors->take(SceneAnchorChannel::Reader::Render);
        std::unique_ptr<SceneRenderer::Slot> slot;
        if (!idle_.empty()) {
            slot = std::move(idle_.back());
            idle_.pop_back();
        } else {
            slot = std::make_unique<SceneRenderer::Slot>();
        }
        bool ok{};
        try {
            ok = renderer_->encode(*slot, *frame.scene, command, drawn, prepared.scale, std::nullopt, index, {},
                                   anchors, start, prepared.textures);
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
        if (!ok) {
            idle_.push_back(std::move(slot));
            failure_ = "the native pass cannot draw the frame: " + renderer_->last_failure();
            return WEFT_RENDER_UNSUPPORTED_FEATURE;
        }
        out = std::move(slot);
        return WEFT_RENDER_OK;
    }

    weft_render_export_backend ViewBackend::exporter() {
        // Formats Apple GPUs render to, filter and fill mips of, whose sampling returns linear values.
        const std::uint32_t formats = (1u << WEFT_RENDER_TARGET_SRGB_RGBA8) | (1u << WEFT_RENDER_TARGET_SRGB_BGRA8) |
                                      (1u << WEFT_RENDER_TARGET_LINEAR_RGBA16F);
        return make_c_export_backend(
            *this, {formats, std::uint32_t(exports_.size()), constants::render::view_export_image_bytes});
    }

    id<MTLRenderPipelineState> ViewBackend::resolve_pipeline(MTLPixelFormat format) {
        if (const auto found = resolve_.find(format); found != resolve_.end()) {
            return found->second;
        }
        if (!resolve_vertex_) {
            const auto source = export_resolve_shader();
            resolve_vertex_ = compile(device_, source.vertex, "vs_main");
            resolve_fragment_ = compile(device_, source.fragment, "ps_main");
        }
        // Every texel written as it is: no blending.
        MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
        d.label = @"weft export resolve";
        d.vertexFunction = resolve_vertex_;
        d.fragmentFunction = resolve_fragment_;
        d.colorAttachments[0].pixelFormat = format;
        NSError* error = nil;
        id<MTLRenderPipelineState> made = [device_ newRenderPipelineStateWithDescriptor:d error:&error];
        ensure(made != nil, "the export resolve pipeline: " + detail::describe(error));
        resolve_.emplace(format, made);
        return made;
    }

    id<MTLTexture> ViewBackend::take_scratch(std::uint32_t width, std::uint32_t height) {
        for (auto it = spare_scratch_.begin(); it != spare_scratch_.end(); ++it) {
            if ((*it).width == width && (*it).height == height) {
                id<MTLTexture> spare = *it;
                spare_scratch_.erase(it);
                return spare;
            }
        }
        // The scene pass draws it and copies from it (backdrops); the resolve reads it.
        id<MTLTexture> made = detail::make_texture(device_, detail::target_format, width, height,
                                                   MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
        made.label = @"weft export scratch";
        return made;
    }

    void ViewBackend::give_scratch(id<MTLTexture> scratch) noexcept {
        if (!scratch) {
            return;
        }
        // The oldest goes first; the capacity stays, so this never allocates.
        if (spare_scratch_.size() == spare_scratch_.capacity()) {
            spare_scratch_.erase(spare_scratch_.begin());
        }
        spare_scratch_.push_back(scratch);
    }

    weft_render_result ViewBackend::create_export_image(const weft_render_export_desc& desc, std::uint64_t& out,
                                                        std::uint64_t& texture) {
        out = 0;
        texture = 0;
        const MTLPixelFormat format = export_format(desc.format);
        if (format == MTLPixelFormatInvalid) {
            return WEFT_RENDER_UNSUPPORTED_FEATURE;
        }
        if (!desc.width || !desc.height || !desc.mip_levels) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        const auto free = std::find(exports_.begin(), exports_.end(), nil);
        if (free == exports_.end()) {
            return WEFT_RENDER_BUSY;
        }
        // Compiled now, so the first frame drawn into it does not wait for it.
        resolve_pipeline(format);
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                     width:desc.width
                                                                                    height:desc.height
                                                                                 mipmapped:NO];
        d.mipmapLevelCount = desc.mip_levels;
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        d.storageMode = MTLStorageModePrivate;
        id<MTLTexture> made = [device_ newTextureWithDescriptor:d];
        if (!made) {
            return WEFT_RENDER_OUT_OF_MEMORY;
        }
        made.label = @"weft export image";
        *free = made;
        out = std::uint64_t(free - exports_.begin()) + 1;
        texture = pointer_value(made);
        return WEFT_RENDER_OK;
    }

    void ViewBackend::destroy_export_image(std::uint64_t native) {
        if (!native || native > exports_.size() || !exports_[native - 1]) {
            std::terminate();
        }
        exports_[native - 1] = nil;
    }

    weft_render_result ViewBackend::encode_export(std::uint64_t native, std::uint64_t image,
                                                  const weft_render_pass& pass, std::uint64_t& out) {
        out = 0;
        const auto found = prepared_.find(native);
        if (found == prepared_.end() || !image || image > exports_.size() || !exports_[image - 1] ||
            !pass.native_pass || pass.native_pass_size < sizeof(weft_render_metal_pass) || pass.sample_count > 1) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        // The image is the target: the pass names only the command buffer.
        const auto& p = *static_cast<const weft_render_metal_pass*>(pass.native_pass);
        if (p.struct_size < sizeof(p) || p.abi_version != WEFT_RENDER_ABI_VERSION || p.reserved || p.clear_target ||
            !p.command_buffer || p.target) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        id<MTLCommandBuffer> command = bridged<id<MTLCommandBuffer>>(p.command_buffer);
        if (command.device != device_) {
            return WEFT_RENDER_WRONG_BACKEND;
        }
        id<MTLTexture> texture = exports_[image - 1];
        if (texture.width != pass.width || texture.height != pass.height) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        failure_.clear();
        const auto& prepared = found->second;
        // A frame that shows the image (imported from an earlier lease) cannot be drawn into it.
        const auto view = pointer_value(texture);
        if (std::any_of(prepared.textures.begin(), prepared.textures.end(), [&](const SceneTexture& t) {
                return t.view == view;
            })) {
            return WEFT_RENDER_INVALID_STATE;
        }
        id<MTLRenderPipelineState> resolve = resolve_pipeline(texture.pixelFormat);
        id<MTLTexture> scratch = take_scratch(pass.width, pass.height);
        std::unique_ptr<SceneRenderer::Slot> slot;
        weft_render_result drawn;
        try {
            // Over transparent, as a host's pass that clears its target.
            drawn = draw(prepared, command, {scratch, pass.width, pass.height, true}, {true, paint::Color{}}, slot);
        } catch (...) {
            give_scratch(scratch);
            throw;
        }
        if (drawn != WEFT_RENDER_OK) {
            give_scratch(scratch);
            return drawn;
        }
        out = next_++;
        uses_.emplace(out, Use{std::move(slot), 0, false, scratch});
        // The scene pass is encoded: from here a failure leaves its commands to the host to cancel (a nonzero use).
        MTLRenderPassDescriptor* target = [MTLRenderPassDescriptor renderPassDescriptor];
        target.colorAttachments[0].texture = texture;
        target.colorAttachments[0].level = 0;
        target.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        target.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:target];
        if (!encoder) {
            failure_ = "no render command encoder for the export resolve";
            return WEFT_RENDER_BACKEND_ERROR;
        }
        encoder.label = @"weft export resolve";
        [encoder setRenderPipelineState:resolve];
        [encoder setFragmentTexture:scratch atIndex:constants::render::metal::export_texture_encoded];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        if (texture.mipmapLevelCount > 1) {
            id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
            if (!blit) {
                failure_ = "no blit command encoder for the export mips";
                return WEFT_RENDER_BACKEND_ERROR;
            }
            blit.label = @"weft export mips";
            [blit generateMipmapsForTexture:texture];
            [blit endEncoding];
        }
        return WEFT_RENDER_OK;
    }

    void ViewBackend::discard_encoded(std::uint64_t native) {
        if (const auto found = uses_.find(native); found != uses_.end()) {
            renderer_->discard(*found->second.slot);
            idle_.push_back(std::move(found->second.slot));
            give_scratch(found->second.scratch);
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
                give_scratch(it->second.scratch);
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
            give_scratch(use.scratch);
        }
        uses_.clear();
    }

    weft_render_result ViewBackend::shutdown() {
        if (!uses_.empty()) {
            return WEFT_RENDER_BUSY;
        }
        prepared_.clear();
        idle_.clear();
        spare_scratch_.clear();
        if (renderer_) {
            for (const auto& [channel, index] : views_) {
                renderer_->forget(index);
            }
        }
        views_.clear();
        return WEFT_RENDER_OK;
    }
} // namespace weft::render::metal

struct weft_render_metal {
    weft::render::metal::ViewBackend backend;

    weft_render_metal(id<MTLDevice> device, std::uint64_t queue) : backend(device, queue) {
    }
};

extern "C" weft_render_result weft_render_metal_create(const weft_render_metal_desc* desc, weft_render_metal** out,
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
    try {
        id<MTLDevice> device = (__bridge id<MTLDevice>)desc->device;
        auto* created = new weft_render_metal(device, desc->queue);
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

extern "C" weft_render_result weft_render_metal_external(weft_render_metal* backend,
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

extern "C" weft_render_result weft_render_metal_export(weft_render_metal* backend,
                                                       weft_render_export_backend* out) noexcept {
    if (!out) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    *out = {};
    if (!backend) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    *out = backend->backend.exporter();
    return WEFT_RENDER_OK;
}

extern "C" void weft_render_metal_destroy(weft_render_metal* backend) noexcept {
    delete backend;
}
