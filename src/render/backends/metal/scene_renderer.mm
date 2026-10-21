#import "render/backends/metal/scene_renderer.hpp"

#import "render/backends/metal/scene_internal.hpp"

#include "constants/common.hpp"
#include "render/backends/metal/shaders.hpp"
#include "render/scene_pass.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <utility>

// The Metal scene renderer's device objects, scratch and cache, uploads and nested passes; record() is in
// scene_record.mm, encode() and draw() in scene_encode.mm.
namespace weft::render::metal {
    namespace {
        using namespace detail;

        NSString* text(std::string_view s) {
            return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding];
        }

        id<MTLSamplerState> sampler(id<MTLDevice> device, MTLSamplerAddressMode mode) {
            MTLSamplerDescriptor* d = [MTLSamplerDescriptor new];
            d.minFilter = MTLSamplerMinMagFilterLinear;
            d.magFilter = MTLSamplerMinMagFilterLinear;
            d.mipFilter = MTLSamplerMipFilterLinear;
            d.sAddressMode = mode;
            d.tAddressMode = mode;
            d.rAddressMode = mode;
            id<MTLSamplerState> out = [device newSamplerStateWithDescriptor:d];
            ensure(out != nil, "a sampler");
            return out;
        }

        std::size_t align(std::size_t value, std::size_t alignment) {
            return (value + alignment - 1) / alignment * alignment;
        }

        // The pixel format an image of the preparer's format is sampled as: encoded premultiplied bytes as they are,
        // data in r, sRGB decoded to linear light before filtering, linear floats.
        MTLPixelFormat image_format(SceneImage::Format format) {
            switch (format) {
                case SceneImage::Format::Bgra:
                    return target_format;
                case SceneImage::Format::Data:
                    return MTLPixelFormatR8Unorm;
                case SceneImage::Format::Srgb:
                    return MTLPixelFormatRGBA8Unorm_sRGB;
                case SceneImage::Format::Linear:
                    break;
            }
            return MTLPixelFormatRGBA32Float;
        }

        // Premultiplied source-over, as the D3D11 and Vulkan renderers blend; or destination-over (under).
        id<MTLRenderPipelineState> make_pipeline(id<MTLDevice> device, id<MTLFunction> vertex,
                                                 id<MTLFunction> fragment, bool under) {
            MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
            d.vertexFunction = vertex;
            d.fragmentFunction = fragment;
            MTLRenderPipelineColorAttachmentDescriptor* c = d.colorAttachments[0];
            c.pixelFormat = target_format;
            c.blendingEnabled = YES;
            c.rgbBlendOperation = MTLBlendOperationAdd;
            c.alphaBlendOperation = MTLBlendOperationAdd;
            c.sourceRGBBlendFactor = under ? MTLBlendFactorOneMinusDestinationAlpha : MTLBlendFactorOne;
            c.sourceAlphaBlendFactor = c.sourceRGBBlendFactor;
            c.destinationRGBBlendFactor = under ? MTLBlendFactorOne : MTLBlendFactorOneMinusSourceAlpha;
            c.destinationAlphaBlendFactor = c.destinationRGBBlendFactor;
            NSError* error = nil;
            id<MTLRenderPipelineState> out = [device newRenderPipelineStateWithDescriptor:d error:&error];
            ensure(out != nil, "a render pipeline: " + describe(error));
            return out;
        }
    } // namespace

    namespace detail {
        id<MTLTexture> make_texture(id<MTLDevice> device, MTLPixelFormat format, std::uint32_t width,
                                    std::uint32_t height, MTLTextureUsage usage) {
            MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                         width:width
                                                                                        height:height
                                                                                     mipmapped:NO];
            d.usage = usage;
            d.storageMode = MTLStorageModePrivate;
            id<MTLTexture> out = [device newTextureWithDescriptor:d];
            ensure(out != nil, "a texture");
            return out;
        }
    } // namespace detail

    void ensure(bool condition, std::string_view message) {
        if (!condition) {
            throw std::runtime_error("WEFT_METAL: " + std::string(message));
        }
    }

    id<MTLFunction> SceneRenderer::function(std::string_view source, const char* name) {
        MTLCompileOptions* options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_1;
        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:text(source) options:options error:&error];
        ensure(library != nil, "compiling MSL: " + describe(error));
        id<MTLFunction> out = [library newFunctionWithName:@(name)];
        ensure(out != nil, std::string("the MSL function ") + name);
        return out;
    }

    id<MTLRenderPipelineState> SceneRenderer::pipeline(std::string_view vertex, std::string_view fragment, bool under) {
        return make_pipeline(device, function(vertex, "vs_main"), function(fragment, "ps_main"), under);
    }

    SceneRenderer::SceneRenderer(id<MTLDevice> metal, SceneText& text, SceneCacheBounds bounds)
        : device(metal), preparer_(text, *this, std::uint32_t(limits::max_surface_dimension), {}, bounds) {
        ensure(device != nil, "a Metal device");
        queue_ = [device newCommandQueue];
        ensure(queue_ != nil, "a command queue");
        clamp_sampler_ = sampler(device, MTLSamplerAddressModeClampToEdge);
        repeat_sampler_ = sampler(device, MTLSamplerAddressModeRepeat);
        mirror_sampler_ = sampler(device, MTLSamplerAddressModeMirrorRepeat);
        const auto scene = scene_shader();
        scene_vertex_ = function(scene.vertex, "vs_main");
        id<MTLFunction> scene_fragment = function(scene.fragment, "ps_main");
        scene_pipeline_ = make_pipeline(device, scene_vertex_, scene_fragment, false);
        under_pipeline_ = make_pipeline(device, scene_vertex_, scene_fragment, true);
        const auto particles = particle_shader();
        particle_pipeline_ = pipeline(particles.vertex, particles.fragment);
        const BackdropFilter filters[]{BackdropFilter::Area, BackdropFilter::Blur, BackdropFilter::Matrix};
        id<MTLFunction> backdrop_vertex = function(backdrop_shader(BackdropFilter::Area).vertex, "vs_main");
        for (std::size_t k = 0; k < backdrop_pipelines_.size(); ++k) {
            // Over a cleared target, source-over writes the pass's color.
            backdrop_pipelines_[k] = make_pipeline(
                device, backdrop_vertex, function(backdrop_shader(filters[k]).fragment, "ps_main"), false);
        }
        MTLTextureDescriptor* blank = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                         width:1
                                                                                        height:1
                                                                                     mipmapped:NO];
        blank.usage = MTLTextureUsageShaderRead;
        blank_texture_ = [device newTextureWithDescriptor:blank];
        ensure(blank_texture_ != nil, "the blank texture");
        const std::uint8_t none[4]{};
        [blank_texture_ replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:none bytesPerRow:4];
        blank_buffer_ = [device newBufferWithLength:std::max(sizeof(SceneClip), sizeof(std::array<float, 4>))
                                            options:MTLResourceStorageModeShared];
        ensure(blank_buffer_ != nil, "the blank buffer");
        std::memset(blank_buffer_.contents, 0, blank_buffer_.length);
        float_filtering_ = [device respondsToSelector:@selector(supports32BitFloatFiltering)] &&
                           device.supports32BitFloatFiltering;
    }

    SceneRenderer::~SceneRenderer() = default;

    const Image& SceneRenderer::page(const SceneAtlasPage& key) {
        auto& page = pages_[{std::uint32_t(key.format), key.page}];
        if (page.texture) {
            return page;
        }
        const auto size = scene_atlas_page_size(key.format);
        page = {make_texture(device, key.format == SceneImage::Format::Data ? MTLPixelFormatR8Unorm : target_format,
                             size, size, MTLTextureUsageShaderRead),
                size, size};
        return page;
    }

    Image SceneRenderer::target(std::uint32_t width, std::uint32_t height, bool kept) {
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
        // A backdrop copies from a pass's targets and into its own (blits need no usage of their own).
        Image image{make_texture(device, target_format, width, height,
                                 MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead),
                    width, height};
        if (!kept) {
            slot_->images.push_back(image);
            ++slot_objects;
        }
        return image;
    }

    Buffer SceneRenderer::new_buffer(std::size_t size) {
        std::size_t capacity = limits::spare_buffer_min_bytes;
        while (capacity < size) {
            capacity *= 2;
        }
        id<MTLBuffer> made = [device newBufferWithLength:capacity options:MTLResourceStorageModeShared];
        ensure(made != nil, "a buffer");
        return {made, capacity, made.contents};
    }

    Buffer& SceneRenderer::buffer(std::size_t size) {
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
        slot_->buffers.push_back(new_buffer(size));
        ++slot_objects;
        return slot_->buffers.back();
    }

    void SceneRenderer::release(Slot& slot) noexcept {
        for (auto& b : slot.buffers) {
            if (slot.spare_buffers.size() < std::size_t(limits::scene_slot_spare_buffers)) {
                slot.spare_buffers.push_back(b);
            }
        }
        slot.buffers.clear();
        // Scratch textures: those this recording drew with, then the spares it left within a count and their bytes.
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
            }
        }
        slot.spare_images = std::move(slot.images);
        slot.images.clear();
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
        // The images not on the device yet, each once: in this pass, its planes' passes, and the contents of the
        // layers and filtered images that are drawn in this recording.
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
        // An image on an atlas page goes into its region, inside its border; others into textures of their own. Rows
        // are staged at a pitch of a multiple of the staging alignment.
        constexpr auto border = std::uint32_t(weft::constants::render::scene::atlas_border);
        constexpr auto alignment = std::size_t(limits::staging_alignment);
        struct Staged {
            std::vector<std::uint8_t> bordered;
            std::uint32_t width{};
            std::uint32_t height{};
            std::size_t pitch{};
            std::size_t offset{};
        };
        std::vector<Staged> staged(pending.size());
        std::size_t total{};
        for (std::size_t i = 0; i < pending.size(); ++i) {
            const auto& image = *pending[i];
            const auto texel = scene_image_texel_bytes(image.format);
            ensure(image.width && image.height &&
                       image.pixels.size() == std::size_t(image.width) * image.height * texel,
                   "scene image size");
            auto& s = staged[i];
            s.width = image.width;
            s.height = image.height;
            if (image.atlas) {
                s.bordered = scene_atlas_pixels(image);
                s.width += 2 * border;
                s.height += 2 * border;
            }
            s.pitch = align(std::size_t(s.width) * texel, alignment);
            s.offset = total;
            total += align(s.pitch * s.height, alignment);
        }
        auto& staging = buffer(total);
        id<MTLBlitCommandEncoder> blit = [command_buffer_ blitCommandEncoder];
        ensure(blit != nil, "a blit command encoder");
        for (std::size_t i = 0; i < pending.size(); ++i) {
            const auto& image = *pending[i];
            const auto& s = staged[i];
            const auto& pixels = image.atlas ? s.bordered : image.pixels;
            const auto row = std::size_t(s.width) * scene_image_texel_bytes(image.format);
            for (std::uint32_t y = 0; y < s.height; ++y) {
                std::memcpy(static_cast<std::uint8_t*>(staging.mapped) + s.offset + y * s.pitch,
                            pixels.data() + y * row, row);
            }
            id<MTLTexture> destination;
            MTLOrigin origin = MTLOriginMake(0, 0, 0);
            if (image.atlas) {
                destination = page({image.format, image.atlas->page}).texture;
                origin = MTLOriginMake(image.atlas->x - border, image.atlas->y - border, 0);
                textures_.emplace(image.id, Texture{pending[i], {}, slot_->recording});
            } else {
                Texture made{pending[i],
                             {make_texture(device, image_format(image.format), image.width, image.height,
                                           MTLTextureUsageShaderRead),
                              image.width, image.height},
                             slot_->recording};
                destination = made.image.texture;
                textures_.emplace(image.id, std::move(made));
            }
            slot_->created.push_back(image.id);
            [blit copyFromBuffer:staging.buffer
                       sourceOffset:s.offset
                  sourceBytesPerRow:s.pitch
                sourceBytesPerImage:s.pitch * s.height
                         sourceSize:MTLSizeMake(s.width, s.height, 1)
                          toTexture:destination
                   destinationSlice:0
                   destinationLevel:0
                  destinationOrigin:origin];
        }
        [blit endEncoding];
        uploads += pending.size();
    }

    id<MTLRenderPipelineState>
    SceneRenderer::effect_pipeline(const std::shared_ptr<const paint::EffectProgram>& program) {
        if (const auto found = effect_pipelines_.find(program.get()); found != effect_pipelines_.end()) {
            return found->second.pipeline;
        }
        ensure(!program->msl_encoded.empty(), "an effect built for Metal");
        EffectPipeline made{program,
                            make_pipeline(device, scene_vertex_, function(program->msl_encoded, "ps_main"), false)};
        return effect_pipelines_.emplace(program.get(), std::move(made)).first->second.pipeline;
    }

    id<MTLRenderCommandEncoder> SceneRenderer::begin(id<MTLTexture> texture, std::uint32_t width,
                                                     std::uint32_t height, const std::optional<paint::Color>& clear) {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        MTLRenderPassColorAttachmentDescriptor* color = pass.colorAttachments[0];
        color.texture = texture;
        color.loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
        if (clear) {
            color.clearColor = MTLClearColorMake((*clear)[0], (*clear)[1], (*clear)[2], (*clear)[3]);
        }
        color.storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> encoder = [command_buffer_ renderCommandEncoderWithDescriptor:pass];
        ensure(encoder != nil, "a render command encoder");
        const MTLViewport viewport{0, 0, double(width), double(height), 0, 1};
        [encoder setViewport:viewport];
        const MTLScissorRect scissor{0, 0, width, height};
        [encoder setScissorRect:scissor];
        return encoder;
    }

    id<MTLTexture> SceneRenderer::layer(const std::shared_ptr<const SceneLayerImage>& layer, float scale) {
        if (const auto it = textures_.find(layer->id); it != textures_.end()) {
            return it->second.image.texture;
        }
        // Drawn here into a texture that the cache then keeps.
        const auto image = target(layer->width, layer->height, true);
        textures_.emplace(layer->id, Texture{layer, image, slot_->recording});
        slot_->created.push_back(layer->id);
        record(*layer->content, image.texture, true, scale);
        return image.texture;
    }

    id<MTLTexture> SceneRenderer::filter(const std::shared_ptr<const SceneFilterImage>& filter) {
        if (const auto it = textures_.find(filter->id); it != textures_.end()) {
            return it->second.image.texture;
        }
        // The chain's textures: its result one that the cache then keeps, the others the slot's.
        const auto& chain = filter->chain;
        std::vector<Image> images(chain.images.size());
        for (std::size_t k = 0; k < images.size(); ++k) {
            images[k] = target(chain.images[k][0], chain.images[k][1], k == chain.result);
        }
        textures_.emplace(filter->id, Texture{filter, images[chain.result], slot_->recording});
        slot_->created.push_back(filter->id);
        // The content into the first, then each pass into its texture: cleared first, unless the pass draws
        // source-over what the texture holds.
        record(*filter->content, images[0].texture, true, filter->scale);
        for (const auto& step : chain.steps) {
            const auto& over = images[step.target];
            id<MTLRenderCommandEncoder> encoder =
                begin(over.texture, over.width, over.height,
                      step.over ? std::nullopt : std::optional<paint::Color>(paint::Color{}));
            [encoder setRenderPipelineState:backdrop_pipelines_.at(std::size_t(step.shader))];
            [encoder setFragmentBytes:&step.constants
                               length:sizeof(BackdropConstants)
                              atIndex:limits::backdrop_buffer_constants];
            [encoder setFragmentTexture:images[step.source].texture atIndex:limits::backdrop_texture_source];
            [encoder setFragmentSamplerState:mirror_sampler_ atIndex:limits::backdrop_sampler_mirror];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [encoder endEncoding];
        }
        ++filter_passes;
        return images[chain.result].texture;
    }
} // namespace weft::render::metal
