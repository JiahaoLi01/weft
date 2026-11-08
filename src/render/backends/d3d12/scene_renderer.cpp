#include "render/backends/d3d12/scene_renderer.hpp"

#include "constants/common.hpp"
#include "constants/render.hpp"
#include "scene/scene_schedule.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace weft::render::d3d12 {
    namespace {
        namespace limits = weft::constants::render::d3d12;

        // The shared scene shader's encoded pass, its particle pass and the backdrop passes as DXBC
        // (cmake/WeftShaders.cmake): the bytecode the D3D11 pass runs, which D3D12 takes with a root signature.
        constexpr unsigned char vertex_dxbc[] = {
#include "shaders/d3d12_scene.vs.inc"
        };
        constexpr unsigned char pixel_dxbc[] = {
#include "shaders/d3d12_scene.ps.inc"
        };
        constexpr unsigned char particle_vertex_dxbc[] = {
#include "shaders/d3d12_scene_particles.vs.inc"
        };
        constexpr unsigned char particle_pixel_dxbc[] = {
#include "shaders/d3d12_scene_particles.ps.inc"
        };
        constexpr unsigned char backdrop_vertex_dxbc[] = {
#include "shaders/d3d12_backdrop_blur.vs.inc"
        };
        constexpr unsigned char area_pixel_dxbc[] = {
#include "shaders/d3d12_backdrop_area.ps.inc"
        };
        constexpr unsigned char blur_pixel_dxbc[] = {
#include "shaders/d3d12_backdrop_blur.ps.inc"
        };
        constexpr unsigned char matrix_pixel_dxbc[] = {
#include "shaders/d3d12_backdrop_matrix.ps.inc"
        };

        // Encoded premultiplied BGRA, as the D3D11 and Vulkan scene targets.
        constexpr DXGI_FORMAT target_format = DXGI_FORMAT_B8G8R8A8_UNORM;
        constexpr D3D12_RESOURCE_STATES readable = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        // A root constant buffer view's address is a multiple of this.
        constexpr std::uint64_t constants_alignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;

        std::uint64_t align(std::uint64_t value, std::uint64_t alignment) {
            return (value + alignment - 1) / alignment * alignment;
        }

        DXGI_FORMAT image_format(SceneImage::Format format) {
            // Each format as the shader samples it: encoded premultiplied bytes as they are, data in r, sRGB decoded to
            // linear light before filtering, linear floats.
            return format == SceneImage::Format::Bgra   ? target_format
                   : format == SceneImage::Format::Data ? DXGI_FORMAT_R8_UNORM
                   : format == SceneImage::Format::Srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                                        : DXGI_FORMAT_R32G32B32A32_FLOAT;
        }
    } // namespace

    SceneRenderer::Slot::~Slot() {
        if (!device) {
            return;
        }
        for (auto& image : images) {
            device->destroy(image);
        }
        for (auto& image : spare_images) {
            device->destroy(image);
        }
        device->release_render_target(target_rtv);
    }

    SceneRenderer::SceneRenderer(Device& device, SceneText& text, SceneCacheBounds bounds)
        : gpu(device),
          reference_text_(text),
          preparer_(
              text, *this, std::uint32_t(limits::max_surface_dimension),
              [this](const SceneFrame& frame, float scale) {
                  if (!reference_) {
                      reference_.emplace(reference_text_);
                  }
                  return reference_->raster(frame, scale);
              },
              bounds) {
        try {
            root_ = create_scene_root(gpu);
            backdrop_root_ = create_backdrop_root(gpu);
            const std::array<std::pair<const unsigned char*, std::size_t>, 3> backdrops{
                {{area_pixel_dxbc, sizeof(area_pixel_dxbc)},
                 {blur_pixel_dxbc, sizeof(blur_pixel_dxbc)},
                 {matrix_pixel_dxbc, sizeof(matrix_pixel_dxbc)}}};
            for (std::size_t k = 0; k < backdrops.size(); ++k) {
                // Over a cleared target, source-over writes the pass's color.
                backdrop_pipelines_[k] =
                    create_pipeline(gpu, backdrop_root_.Get(), backdrop_vertex_dxbc, sizeof(backdrop_vertex_dxbc),
                                    backdrops[k].first, backdrops[k].second, target_format);
            }
            pipelines(target_format);
            // Only bound, never sampled for a visible result.
            blank_image_ = gpu.image(DXGI_FORMAT_R8_UNORM, 1, 1, false, readable);
            blank_buffer_ = gpu.buffer(sizeof(SceneClip));
            std::memset(blank_buffer_.mapped, 0, sizeof(SceneClip));
        } catch (...) {
            destroy_all();
            throw;
        }
    }

    SceneRenderer::~SceneRenderer() {
        destroy_all();
    }

    void SceneRenderer::destroy_all() noexcept {
        for (auto& [id, texture] : textures_) {
            gpu.destroy(texture.image);
        }
        textures_.clear();
        for (auto& [key, page] : pages_) {
            gpu.destroy(page);
        }
        pages_.clear();
        pipelines_.clear();
        backdrop_pipelines_ = {};
        gpu.destroy(blank_image_);
        blank_buffer_ = {};
        backdrop_root_.Reset();
        root_.Reset();
    }

    SceneRenderer::Pipelines& SceneRenderer::pipelines(DXGI_FORMAT format) {
        auto& p = pipelines_[format];
        if (!p.scene) {
            p.scene = create_pipeline(gpu, root_.Get(), vertex_dxbc, sizeof(vertex_dxbc), pixel_dxbc,
                                      sizeof(pixel_dxbc), format);
            p.under = create_pipeline(gpu, root_.Get(), vertex_dxbc, sizeof(vertex_dxbc), pixel_dxbc,
                                      sizeof(pixel_dxbc), format, true);
            p.particles = create_pipeline(gpu, root_.Get(), particle_vertex_dxbc, sizeof(particle_vertex_dxbc),
                                          particle_pixel_dxbc, sizeof(particle_pixel_dxbc), format);
        }
        return p;
    }

    ID3D12PipelineState* SceneRenderer::effect_pipeline(DXGI_FORMAT format,
                                                        const std::shared_ptr<const paint::EffectProgram>& program) {
        auto& effects = pipelines(format).effects;
        if (const auto found = effects.find(program.get()); found != effects.end()) {
            return found->second.second.Get();
        }
        // The program's encoded pass is the DXBC the D3D11 pass runs.
        ensure(!program->d3d11_encoded.empty(), "an effect built for D3D11 and D3D12");
        auto pipeline = create_pipeline(gpu, root_.Get(), vertex_dxbc, sizeof(vertex_dxbc),
                                        program->d3d11_encoded.data(), program->d3d11_encoded.size(), format);
        return effects.emplace(program.get(), std::make_pair(program, std::move(pipeline))).first->second.second.Get();
    }

    Image& SceneRenderer::page(const SceneAtlasPage& key) {
        auto& page = pages_[{std::uint32_t(key.format), key.page}];
        if (!page.resource) {
            const auto size = scene_atlas_page_size(key.format);
            // Shader-readable outside the copies of a recording that uploads into it.
            page = gpu.image(key.format == SceneImage::Format::Data ? DXGI_FORMAT_R8_UNORM : target_format, size, size,
                             false, readable);
        }
        return page;
    }

    Image& SceneRenderer::scratch(std::uint32_t width, std::uint32_t height, DXGI_FORMAT format) {
        auto& spares = slot_->spare_images;
        const auto spare = std::find_if(spares.begin(), spares.end(), [&](const Image& image) {
            return image.width == width && image.height == height && image.format == format;
        });
        if (spare != spares.end()) {
            slot_->images.push_back(*spare);
            spares.erase(spare);
            return slot_->images.back();
        }
        // A backdrop copies from a pass's targets and into its own. Every target is shader-readable between the
        // steps that draw into it or copy it.
        slot_->images.push_back(gpu.image(format, width, height, true, readable));
        ++slot_objects;
        return slot_->images.back();
    }

    Buffer& SceneRenderer::buffer(std::uint64_t size) {
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
        std::uint64_t capacity = limits::spare_buffer_min_bytes;
        while (capacity < size) {
            capacity *= 2;
        }
        slot_->buffers.push_back(gpu.buffer(capacity));
        ++slot_objects;
        return slot_->buffers.back();
    }

    std::uint32_t SceneRenderer::descriptors(std::uint32_t count) {
        if (!heap_ || heap_->capacity - heap_->used < count) {
            // A spare with room, or a new heap twice the last one's size and with room.
            auto& spares = slot_->spare_heaps;
            const auto spare = std::find_if(spares.begin(), spares.end(), [&](const DescriptorHeap& h) {
                return h.capacity >= count;
            });
            if (spare != spares.end()) {
                slot_->heaps.push_back(*spare);
                spares.erase(spare);
            } else {
                auto capacity = std::max<std::uint32_t>(std::uint32_t(limits::descriptor_heap_min),
                                                        heap_ ? heap_->capacity * 2 : 0);
                while (capacity < count) {
                    capacity *= 2;
                }
                slot_->heaps.push_back(gpu.heap(capacity));
                ++slot_objects;
            }
            heap_ = &slot_->heaps.back();
            heap_->used = 0;
            ID3D12DescriptorHeap* heaps[]{heap_->heap.Get()};
            command_->SetDescriptorHeaps(1, heaps);
        }
        const auto first = heap_->used;
        heap_->used += count;
        return first;
    }

    void SceneRenderer::write_view(std::uint32_t index, const View& view) {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = view.format;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Texture2D.MipLevels = view.levels;
        auto handle = heap_->heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += std::size_t(index) * gpu.srv_increment;
        gpu.device->CreateShaderResourceView(view.resource, &desc, handle);
    }

    void SceneRenderer::write_buffer(std::uint32_t index, const Buffer& buffer, std::size_t count, std::size_t stride) {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Buffer.NumElements = UINT(count);
        desc.Buffer.StructureByteStride = UINT(stride);
        auto handle = heap_->heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += std::size_t(index) * gpu.srv_increment;
        gpu.device->CreateShaderResourceView(buffer.resource.Get(), &desc, handle);
    }

    D3D12_GPU_DESCRIPTOR_HANDLE SceneRenderer::gpu_descriptor(std::uint32_t index) const {
        auto handle = heap_->heap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += std::uint64_t(index) * gpu.srv_increment;
        return handle;
    }

    void SceneRenderer::release(Slot& slot) noexcept {
        // Spares for the slot's next recordings, as many as it keeps; the rest go.
        for (auto& b : slot.buffers) {
            if (slot.spare_buffers.size() < std::size_t(limits::scene_slot_spare_buffers)) {
                slot.spare_buffers.push_back(std::move(b));
            }
        }
        slot.buffers.clear();
        // Scratch textures: those this recording drew with, whatever their bytes (the next one most likely draws the
        // same, and a pass's groups at a large target pass any fixed bound alone), then the spares it left within a
        // count and their bytes.
        std::vector<Image> spares(slot.images.begin(), slot.images.end());
        std::size_t image_bytes{};
        for (const auto& image : spares) {
            image_bytes += std::size_t(image.width) * image.height * 4;
        }
        for (auto& image : slot.spare_images) {
            const auto bytes = std::size_t(image.width) * image.height * 4;
            if (spares.size() < std::size_t(limits::scene_slot_spare_images) &&
                image_bytes + bytes <= std::size_t(limits::scene_slot_spare_image_bytes)) {
                spares.push_back(image);
                image_bytes += bytes;
            } else {
                gpu.destroy(image);
            }
        }
        slot.spare_images = std::move(spares);
        slot.images.clear();
        for (auto& h : slot.heaps) {
            if (slot.spare_heaps.size() < std::size_t(limits::scene_slot_spare_heaps)) {
                slot.spare_heaps.push_back(std::move(h));
            }
        }
        slot.heaps.clear();
        gpu.release_render_target(slot.target_rtv);
        slot.target_rtv = UINT32_MAX;
        slot.leases.clear();
        slot.held.clear();
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
        for (const auto& sprite : prepared.sprites) {
            if (sprite && pending(sprite->id)) {
                throw Busy("a scene samples what another recording that is not submitted uploads");
            }
        }
        for (const auto& mask : prepared.mask_images) {
            if (mask && pending(mask->id)) {
                throw Busy("a scene samples what another recording that is not submitted uploads");
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
            for (const auto& mask : p.mask_images) {
                if (mask && !textures_.contains(mask->id) && seen.insert(mask->id).second) {
                    pending.push_back(mask);
                }
            }
        };
        walk(walk, prepared);
        if (pending.empty()) {
            return;
        }
        // An image on an atlas page goes into its region, inside its border; others into textures of their own. Each
        // is staged in rows of the copy's pitch at an offset the copy may start from.
        constexpr auto border = std::uint32_t(weft::constants::render::scene::atlas_border);

        struct Copy {
            std::uint64_t offset{};
            std::uint32_t width{};
            std::uint32_t height{};
            std::uint32_t pitch{};
            std::uint32_t x{};
            std::uint32_t y{};
            std::vector<std::uint8_t> bordered;
        };

        std::vector<Copy> copies(pending.size());
        std::uint64_t staged{};
        for (std::size_t i = 0; i < pending.size(); ++i) {
            const auto& image = *pending[i];
            ensure(image.width && image.height &&
                       image.pixels.size() ==
                           std::size_t(image.width) * image.height * scene_image_texel_bytes(image.format),
                   "scene image size");
            auto& copy = copies[i];
            copy.width = image.width;
            copy.height = image.height;
            if (image.atlas) {
                copy.bordered = scene_atlas_pixels(image);
                copy.x = image.atlas->x - border;
                copy.y = image.atlas->y - border;
                copy.width += 2 * border;
                copy.height += 2 * border;
            }
            copy.pitch = std::uint32_t(align(std::uint64_t(copy.width) * scene_image_texel_bytes(image.format),
                                             D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
            staged = align(staged, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
            copy.offset = staged;
            staged += std::uint64_t(copy.pitch) * copy.height;
        }
        const auto& staging = buffer(staged);
        std::vector<D3D12_RESOURCE_BARRIER> to_copy;
        std::vector<D3D12_RESOURCE_BARRIER> to_read;
        std::vector<Image*> destinations;
        for (std::size_t i = 0; i < pending.size(); ++i) {
            const auto& image = *pending[i];
            const auto& copy = copies[i];
            const auto& pixels = image.atlas ? copy.bordered : image.pixels;
            const auto row = std::size_t(copy.width) * scene_image_texel_bytes(image.format);
            for (std::uint32_t y = 0; y < copy.height; ++y) {
                std::memcpy(static_cast<std::uint8_t*>(staging.mapped) + copy.offset + std::size_t(y) * copy.pitch,
                            pixels.data() + y * row, row);
            }
            if (image.atlas) {
                // A page keeps what earlier recordings put on it: it leaves the shader-readable state for these copies,
                // and returns to it.
                auto& handle = page({image.format, image.atlas->page});
                destinations.push_back(&handle);
                textures_.emplace(image.id, Texture{pending[i], {}, slot_->recording});
                slot_->created.push_back(image.id);
                if (std::none_of(to_copy.begin(), to_copy.end(), [&](const D3D12_RESOURCE_BARRIER& b) {
                        return b.Transition.pResource == handle.resource.Get();
                    })) {
                    to_copy.push_back(barrier(handle.resource.Get(), readable, D3D12_RESOURCE_STATE_COPY_DEST));
                    to_read.push_back(barrier(handle.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, readable));
                }
                continue;
            }
            Texture texture{
                pending[i],
                gpu.image(image_format(image.format), image.width, image.height, false, D3D12_RESOURCE_STATE_COPY_DEST),
                slot_->recording};
            to_read.push_back(barrier(texture.image.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, readable));
            texture.image.state = readable;
            auto& placed = textures_.emplace(image.id, std::move(texture)).first->second;
            destinations.push_back(&placed.image);
            slot_->created.push_back(image.id);
        }
        if (!to_copy.empty()) {
            command_->ResourceBarrier(UINT(to_copy.size()), to_copy.data());
        }
        for (std::size_t i = 0; i < pending.size(); ++i) {
            const auto& copy = copies[i];
            D3D12_TEXTURE_COPY_LOCATION from{};
            from.pResource = staging.resource.Get();
            from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            from.PlacedFootprint.Offset = copy.offset;
            from.PlacedFootprint.Footprint = {destinations[i]->format, copy.width, copy.height, 1, copy.pitch};
            D3D12_TEXTURE_COPY_LOCATION to{};
            to.pResource = destinations[i]->resource.Get();
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.SubresourceIndex = 0;
            command_->CopyTextureRegion(&to, copy.x, copy.y, 0, &from, nullptr);
        }
        command_->ResourceBarrier(UINT(to_read.size()), to_read.data());
        uploads += pending.size();
    }

    View SceneRenderer::layer(const std::shared_ptr<const SceneLayerImage>& layer, float scale) {
        if (const auto it = textures_.find(layer->id); it != textures_.end()) {
            return {it->second.image.resource.Get(), it->second.image.format};
        }
        // Drawn here into a texture that the cache then keeps.
        auto& texture =
            textures_
                .emplace(layer->id,
                         Texture{layer, gpu.image(target_format, layer->width, layer->height, true, readable),
                                 slot_->recording})
                .first->second;
        slot_->created.push_back(layer->id);
        record(*layer->content,
               {&texture.image, texture.image.rtv, texture.image.format, texture.image.width, texture.image.height},
               scale);
        transition(command_, texture.image, readable);
        return {texture.image.resource.Get(), texture.image.format};
    }

    View SceneRenderer::filter(const std::shared_ptr<const SceneFilterImage>& filter) {
        if (const auto it = textures_.find(filter->id); it != textures_.end()) {
            return {it->second.image.resource.Get(), it->second.image.format};
        }
        // The chain's images: its result a texture that the cache then keeps, the others the slot's.
        const auto& chain = filter->chain;
        const auto size = chain.images[chain.result];
        auto& texture =
            textures_
                .emplace(filter->id,
                         Texture{filter, gpu.image(target_format, size[0], size[1], true, readable), slot_->recording})
                .first->second;
        slot_->created.push_back(filter->id);
        std::vector<Image*> images(chain.images.size());
        for (std::size_t k = 0; k < images.size(); ++k) {
            images[k] = k == chain.result ? &texture.image : &scratch(chain.images[k][0], chain.images[k][1]);
        }
        // The content into the first, then each pass into its image.
        auto& content = *images[0];
        record(*filter->content, {&content, content.rtv, content.format, content.width, content.height}, filter->scale);
        transition(command_, content, readable);
        if (!chain.steps.empty()) {
            const auto stride = align(sizeof(BackdropConstants), constants_alignment);
            auto& values = buffer(stride * chain.steps.size());
            const auto first = descriptors(std::uint32_t(chain.steps.size()));
            for (std::size_t k = 0; k < chain.steps.size(); ++k) {
                std::memcpy(static_cast<std::uint8_t*>(values.mapped) + stride * k, &chain.steps[k].constants,
                            sizeof(BackdropConstants));
                const auto& source = *images[chain.steps[k].source];
                write_view(first + std::uint32_t(k), {source.resource.Get(), source.format});
            }
            command_->SetGraphicsRootSignature(backdrop_root_.Get());
            command_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            for (std::size_t k = 0; k < chain.steps.size(); ++k) {
                const auto& step = chain.steps[k];
                auto& over = *images[step.target];
                transition(command_, over, D3D12_RESOURCE_STATE_RENDER_TARGET);
                command_->OMSetRenderTargets(1, &over.rtv, FALSE, nullptr);
                if (!step.over) {
                    const float none[4]{};
                    command_->ClearRenderTargetView(over.rtv, none, 0, nullptr);
                }
                const D3D12_VIEWPORT viewport{0, 0, float(over.width), float(over.height), 0, 1};
                command_->RSSetViewports(1, &viewport);
                const D3D12_RECT scissor{0, 0, LONG(over.width), LONG(over.height)};
                command_->RSSetScissorRects(1, &scissor);
                // The backdrop pipelines draw source-over: over a cleared image, the pass's color.
                command_->SetPipelineState(backdrop_pipelines_
                                               .at(step.shader == render::FilterStep::Shader::Area   ? 0
                                                   : step.shader == render::FilterStep::Shader::Blur ? 1
                                                                                                     : 2)
                                               .Get());
                command_->SetGraphicsRootConstantBufferView(limits::backdrop_root_constants,
                                                            values.address + stride * k);
                command_->SetGraphicsRootDescriptorTable(limits::backdrop_root_source,
                                                         gpu_descriptor(first + std::uint32_t(k)));
                command_->DrawInstanced(3, 1, 0, 0);
                transition(command_, over, readable);
            }
        }
        ++filter_passes;
        return {texture.image.resource.Get(), texture.image.format};
    }

    void SceneRenderer::record(const ScenePrepared& prepared, const Surface& out, float scale,
                               const SceneAnchorFrame* anchors, std::uint64_t serial, const paint::Color* clear,
                               std::span<const SceneTexture> host_textures, const ScenePatchHistory* history) {
        const auto& pass = prepared.pass;
        auto* command = command_;
        // What the pass draws, in order (scene/scene_schedule.hpp).
        const auto schedule = schedule_scene_pass(prepared, host_textures, clear);
        // Nested passes first, each ending before this one begins: plane contents, and layers not drawn before. The
        // slot leases every image and layer the pass samples.
        std::vector<View> views(prepared.images.size());
        for (std::size_t i = 0; i < prepared.images.size(); ++i) {
            if (prepared.planes[i]) {
                auto& plane = scratch(prepared.planes[i]->width, prepared.planes[i]->height);
                record(*prepared.planes[i], {&plane, plane.rtv, plane.format, plane.width, plane.height}, scale,
                       nullptr, 0, nullptr, host_textures);
                transition(command, plane, readable);
                views[i] = {plane.resource.Get(), plane.format};
                ++plane_passes;
            } else if (prepared.layers[i]) {
                views[i] = layer(prepared.layers[i], scale);
                slot_->leases.push_back(prepared.layers[i]);
            } else if (prepared.filters[i]) {
                views[i] = filter(prepared.filters[i]);
                slot_->leases.push_back(prepared.filters[i]);
            } else if (const auto& drawn = prepared.images[i]) {
                const auto& image =
                    drawn->atlas ? page({drawn->format, drawn->atlas->page}) : textures_.at(drawn->id).image;
                views[i] = {image.resource.Get(), image.format};
                slot_->leases.push_back(drawn);
            }
        }
        // The host texture each texture item draws.
        for (std::size_t i = 0; i < schedule.textures.size(); ++i) {
            if (const auto* texture = schedule.textures[i]) {
                views[i] = *reinterpret_cast<const View*>(static_cast<std::uintptr_t>(texture->view));
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
        // The pass's records and clips. A kept pass's stay in the slot's own buffers while the slot draws that pass.
        const Buffer* records{};
        const Buffer* clips = &blank_buffer_;
        if (serial) {
            const auto put = [&](Buffer& kept, const void* data, std::size_t bytes) {
                if (kept.size < std::max<std::size_t>(bytes, 1)) {
                    std::uint64_t capacity = limits::spare_buffer_min_bytes;
                    while (capacity < bytes) {
                        capacity *= 2;
                    }
                    kept = gpu.buffer(capacity);
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
            records = &slot_->kept_records;
            if (!pass.clips.empty()) {
                clips = &slot_->kept_clips;
            }
        } else {
            auto& record_buffer = buffer(std::max<std::size_t>(pass.records.size(), 1) * sizeof(SceneRecord));
            records = &record_buffer;
            std::memcpy(record_buffer.mapped, record_data, pass.records.size() * sizeof(SceneRecord));
            if (!pass.clips.empty()) {
                auto& clip_buffer = buffer(pass.clips.size() * sizeof(SceneClip));
                clips = &clip_buffer;
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
        const auto* order = &order_buffer;
        std::memcpy(order_buffer.mapped, order_values->data(), pass.records.size() * sizeof(std::uint32_t));
        const Buffer* anchor_values = &blank_buffer_;
        std::size_t anchor_count = 1;
        if (anchors && !anchors->anchors.empty()) {
            ensure(anchors->anchors.size() == prepared.anchors.size(), "the anchors of the pass's anchored elements");
            auto& anchor_buffer = buffer(anchors->anchors.size() * sizeof(anchors->anchors[0]));
            anchor_values = &anchor_buffer;
            anchor_count = anchors->anchors.size();
            std::memcpy(anchor_buffer.mapped, anchors->anchors.data(),
                        anchors->anchors.size() * sizeof(anchors->anchors[0]));
        } else {
            ensure(prepared.anchors.empty(), "the anchors of the pass's anchored elements");
        }
        // The View constants: the target's size, the scale and, per draw, its first record (SV_InstanceID counts from
        // the draw's first instance on D3D12, so the shader adds it).
        std::array<std::uint32_t, limits::view_constant_count> constants{
            std::bit_cast<std::uint32_t>(float(prepared.width)), std::bit_cast<std::uint32_t>(float(prepared.height)),
            std::bit_cast<std::uint32_t>(scale), 0};
        // A group texture of the pass's size for each depth.
        std::vector<Image*> groups;
        for (std::size_t depth = 0; depth < schedule.groups; ++depth) {
            groups.push_back(&scratch(prepared.width, prepared.height));
        }
        // Each backdrop's chain of textures: its copy (of the format of the target it reads), then one target per
        // filter pass; the backdrop draws the last.
        std::vector<std::vector<Image*>> chains;
        for (const auto& chain : schedule.chains) {
            auto& images = chains.emplace_back();
            for (std::size_t k = 0; k < chain.images.size(); ++k) {
                const auto [w, h] = chain.images[k];
                images.push_back(&scratch(w, h, k == 0 && chain.from_out ? out.format : target_format));
            }
        }

        // A table per distinct sampling of the pass's draws: the texture a draw samples (a texture, a page, the group
        // it composites, a backdrop's filtered copy, or none) with an effect's textures (or a sprite); per draw in
        // order, its table; and per effect draw in order, its program. Draws that sample alike share a table.
        using Textures = std::array<View, weft::constants::max_effect_textures>;
        const View blank{blank_image_.resource.Get(), blank_image_.format};

        struct Sampling {
            View image;
            bool effect{};
            Textures textures{};
        };

        std::vector<Sampling> samplings;
        std::vector<std::size_t> draw_tables;
        const auto use = [&](const View& image, const std::optional<Textures>& textures) {
            const Sampling s{image, textures.has_value(), textures.value_or(Textures{})};
            const auto found = std::find_if(samplings.begin(), samplings.end(), [&](const Sampling& o) {
                return o.image == s.image && o.effect == s.effect && o.textures == s.textures;
            });
            draw_tables.push_back(std::size_t(found - samplings.begin()));
            if (found == samplings.end()) {
                samplings.push_back(s);
            }
        };
        std::vector<std::shared_ptr<const paint::EffectProgram>> effects;
        const auto view_of = [](const Image& image) {
            return View{image.resource.Get(), image.format};
        };
        for (const auto& step : schedule.commands) {
            const auto* draw = std::get_if<SceneSchedule::Draw>(&step);
            if (!draw) {
                continue;
            }
            View sampled = blank;
            switch (draw->image.kind) {
                case SceneSchedule::Image::Kind::Blank:
                    break;
                case SceneSchedule::Image::Kind::Item:
                    sampled = views.at(draw->image.index);
                    ensure(sampled.resource != nullptr, "an image item's texture");
                    break;
                case SceneSchedule::Image::Kind::Page:
                    sampled = view_of(page(prepared.pages.at(draw->image.index)));
                    break;
                case SceneSchedule::Image::Kind::Group:
                    sampled = view_of(*groups.at(draw->image.index));
                    break;
                case SceneSchedule::Image::Kind::Backdrop:
                    sampled = view_of(*chains.at(draw->image.index).back());
                    break;
            }
            if (draw->extra == SceneSchedule::Extra::None && !draw->backdrop) {
                use(sampled, std::nullopt);
                continue;
            }
            Textures textures;
            textures.fill(blank);
            if (draw->backdrop) {
                // A blending group's backdrop where an effect's second texture goes (scene.hlsl, blendBackdrop).
                textures[1] = view_of(*chains.at(*draw->backdrop).back());
            }
            if (draw->extra == SceneSchedule::Extra::Effect) {
                const auto& effect = prepared.effects.at(draw->source);
                ensure(effect.textures.size() <= weft::constants::max_effect_textures, "effect textures");
                for (std::size_t t = 0; t < effect.textures.size(); ++t) {
                    textures[t] = view_of(textures_.at(effect.textures[t]->id).image);
                    slot_->leases.push_back(effect.textures[t]);
                }
                effects.push_back(effect.program);
            } else if (draw->extra == SceneSchedule::Extra::Mask) {
                // The image of the item's mask where an effect's first texture goes (scene.hlsl, maskImage).
                const auto& mask = prepared.mask_images.at(draw->source);
                ensure(mask != nullptr, "a mask's image");
                textures[0] = view_of(textures_.at(mask->id).image);
                slot_->leases.push_back(mask);
            } else if (draw->extra == SceneSchedule::Extra::Sprite) {
                // The emission's sprite where an effect's first texture goes.
                if (const auto& sprite = prepared.sprites.at(draw->source)) {
                    textures[0] = view_of(textures_.at(sprite->id).image);
                    slot_->leases.push_back(sprite);
                }
            }
            use(sampled, textures);
        }
        // Each chain's first filter pass among all of the pass's.
        std::vector<std::size_t> first_filters;
        std::size_t backdrop_passes_total{};
        for (const auto& chain : schedule.chains) {
            first_filters.push_back(backdrop_passes_total);
            backdrop_passes_total += chain.passes.size();
        }
        // Every image table of the pass, every backdrop pass's source and the pass's buffers, in one run of the
        // recording's heap.
        const auto table_size = std::uint32_t(limits::table_descriptors);
        const auto first_descriptor =
            descriptors(std::uint32_t(samplings.size()) * table_size + std::uint32_t(backdrop_passes_total) +
                        std::uint32_t(limits::buffer_descriptors));
        const auto sources = first_descriptor + std::uint32_t(samplings.size()) * table_size;
        const auto buffer_descriptors = sources + std::uint32_t(backdrop_passes_total);
        const auto records_count = std::max<std::size_t>(pass.records.size(), 1);
        write_buffer(buffer_descriptors, *records, records_count, sizeof(SceneRecord));
        write_buffer(buffer_descriptors + 1, *clips, std::max<std::size_t>(pass.clips.size(), 1), sizeof(SceneClip));
        write_buffer(buffer_descriptors + 2, *order, records_count, sizeof(std::uint32_t));
        write_buffer(buffer_descriptors + 3, *anchor_values, anchor_count, sizeof(std::array<float, 4>));
        for (std::size_t s = 0; s < samplings.size(); ++s) {
            const auto at = first_descriptor + std::uint32_t(s) * table_size;
            write_view(at, samplings[s].image);
            for (std::uint32_t t = 0; t + 1 < table_size; ++t) {
                write_view(at + 1 + t, samplings[s].effect ? samplings[s].textures[t] : blank);
            }
        }
        // The backdrop passes' constants, each at its own aligned offset.
        const auto stride = align(sizeof(BackdropConstants), constants_alignment);
        D3D12_GPU_VIRTUAL_ADDRESS backdrop_constants{};
        if (backdrop_passes_total) {
            auto& values = buffer(stride * backdrop_passes_total);
            backdrop_constants = values.address;
            std::size_t k{};
            for (std::size_t c = 0; c < schedule.chains.size(); ++c) {
                const auto& filters = schedule.chains[c].passes;
                for (std::size_t p = 0; p < filters.size(); ++p, ++k) {
                    std::memcpy(static_cast<std::uint8_t*>(values.mapped) + stride * k, &filters[p].second,
                                sizeof(BackdropConstants));
                    write_view(sources + std::uint32_t(k), view_of(*chains[c][p]));
                }
            }
        }

        // The scene's root signature and its root views: set once, and again after a backdrop's passes replaced them.
        constexpr auto unbound = std::numeric_limits<std::size_t>::max();
        auto bound = unbound;
        bool scene_root{};
        const auto bind = [&] {
            command->SetGraphicsRootSignature(root_.Get());
            command->SetGraphicsRootDescriptorTable(limits::root_buffers, gpu_descriptor(buffer_descriptors));
            command->SetGraphicsRoot32BitConstants(limits::root_view, limits::view_constant_count, constants.data(), 0);
            command->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            bound = unbound;
            scene_root = true;
        };
        // The target the pass draws into now, cleared first when asked.
        DXGI_FORMAT format{};
        const auto begin = [&](const Surface& s, const paint::Color* color) {
            transition(command, *s.image, D3D12_RESOURCE_STATE_RENDER_TARGET);
            command->OMSetRenderTargets(1, &s.rtv, FALSE, nullptr);
            if (color) {
                const D3D12_RECT area{0, 0, LONG(prepared.width), LONG(prepared.height)};
                command->ClearRenderTargetView(s.rtv, color->data(), 1, &area);
            }
            const D3D12_VIEWPORT viewport{0, 0, float(prepared.width), float(prepared.height), 0, 1};
            command->RSSetViewports(1, &viewport);
            const D3D12_RECT scissor{0, 0, LONG(prepared.width), LONG(prepared.height)};
            command->RSSetScissorRects(1, &scissor);
            format = s.format;
            command->SetPipelineState(pipelines(format).scene.Get());
        };
        // A backdrop chain's image as the target, cleared, over all of it.
        const auto begin_over = [&](Image& over, const paint::Color& color) {
            transition(command, over, D3D12_RESOURCE_STATE_RENDER_TARGET);
            command->OMSetRenderTargets(1, &over.rtv, FALSE, nullptr);
            command->ClearRenderTargetView(over.rtv, color.data(), 0, nullptr);
            const D3D12_VIEWPORT viewport{0, 0, float(over.width), float(over.height), 0, 1};
            command->RSSetViewports(1, &viewport);
            const D3D12_RECT scissor{0, 0, LONG(over.width), LONG(over.height)};
            command->RSSetScissorRects(1, &scissor);
        };
        std::size_t next_draw{};
        // count instances of vertices each.
        const auto instances = [&](std::uint32_t first, std::uint32_t count, std::uint32_t vertices) {
            if (const auto table = draw_tables.at(next_draw++); table != bound) {
                command->SetGraphicsRootDescriptorTable(
                    limits::root_images, gpu_descriptor(first_descriptor + std::uint32_t(table) * table_size));
                bound = table;
            }
            command->SetGraphicsRoot32BitConstant(limits::root_view, std::bit_cast<std::uint32_t>(float(first)),
                                                  limits::view_base_instance);
            command->DrawInstanced(vertices, count, 0, 0);
            ++draws;
        };
        // The texture of a target of the schedule: the pass's own, a group's, or a chain's.
        const auto image_of = [&](const SceneSchedule::Target& t) -> Image& {
            switch (t.kind) {
                case SceneSchedule::Target::Kind::Group:
                    return *groups.at(t.index);
                case SceneSchedule::Target::Kind::Chain:
                    return *chains.at(t.index).at(t.image);
                case SceneSchedule::Target::Kind::Out:
                    break;
            }
            return *out.image;
        };
        bind();
        std::size_t next_effect{};
        for (const auto& step : schedule.commands) {
            if (const auto* b = std::get_if<SceneSchedule::Begin>(&step)) {
                if (b->target.kind == SceneSchedule::Target::Kind::Chain) {
                    begin_over(image_of(b->target), b->clear.value_or(paint::Color{}));
                    continue;
                }
                if (!scene_root) {
                    bind();
                }
                if (b->target.kind == SceneSchedule::Target::Kind::Out) {
                    begin(out, b->clear ? &*b->clear : nullptr);
                } else {
                    auto& image = image_of(b->target);
                    begin({&image, image.rtv, image.format, image.width, image.height},
                          b->clear ? &*b->clear : nullptr);
                }
            } else if (const auto* e = std::get_if<SceneSchedule::End>(&step)) {
                // Its draws done, a target that is sampled next becomes shader-readable.
                if (e->sampled) {
                    transition(command, image_of(e->target), readable);
                }
            } else if (const auto* draw = std::get_if<SceneSchedule::Draw>(&step)) {
                ID3D12PipelineState* pipeline{};
                switch (draw->pipeline) {
                    case SceneSchedule::Pipeline::Scene:
                        break;
                    case SceneSchedule::Pipeline::Under:
                        pipeline = pipelines(format).under.Get();
                        break;
                    case SceneSchedule::Pipeline::Particles:
                        pipeline = pipelines(format).particles.Get();
                        break;
                    case SceneSchedule::Pipeline::Effect:
                        pipeline = effect_pipeline(format, effects.at(next_effect++));
                        break;
                    case SceneSchedule::Pipeline::Blend:
                        // Only a backend that reads its target in the shader schedules these (fetch_blends).
                        ensure(false, "a blending composite that reads its target");
                        break;
                }
                if (pipeline) {
                    command->SetPipelineState(pipeline);
                }
                instances(draw->first, draw->count, draw->vertices);
                if (pipeline) {
                    command->SetPipelineState(pipelines(format).scene.Get());
                }
            } else if (const auto* copy = std::get_if<SceneSchedule::Copy>(&step)) {
                // The region of the target the backdrop reads, into its chain's first texture; the target returns to
                // the state it had.
                auto& from = image_of(copy->from);
                auto& to = *chains.at(copy->chain).front();
                const auto was = from.state;
                transition(command, from, D3D12_RESOURCE_STATE_COPY_SOURCE);
                transition(command, to, D3D12_RESOURCE_STATE_COPY_DEST);
                D3D12_TEXTURE_COPY_LOCATION source{};
                source.pResource = from.resource.Get();
                source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_TEXTURE_COPY_LOCATION destination{};
                destination.pResource = to.resource.Get();
                destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                const auto& r = copy->region;
                const D3D12_BOX region{UINT(r[0]), UINT(r[1]), 0, UINT(r[2]), UINT(r[3]), 1};
                command->CopyTextureRegion(&destination, 0, 0, 0, &source, &region);
                transition(command, from, was);
                transition(command, to, readable);
            } else if (const auto* filter = std::get_if<SceneSchedule::Filter>(&step)) {
                // The backdrop passes' root signature, until the scene's next target.
                if (scene_root) {
                    command->SetGraphicsRootSignature(backdrop_root_.Get());
                    command->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    scene_root = false;
                }
                const auto k = std::uint32_t(first_filters.at(filter->chain) + filter->pass);
                auto& over = *chains.at(filter->chain).at(filter->pass + 1);
                begin_over(over, paint::Color{});
                const auto kind = schedule.chains.at(filter->chain).passes.at(filter->pass).first;
                command->SetPipelineState(backdrop_pipelines_.at(std::size_t(kind)).Get());
                command->SetGraphicsRootConstantBufferView(limits::backdrop_root_constants,
                                                           backdrop_constants + stride * k);
                command->SetGraphicsRootDescriptorTable(limits::backdrop_root_source, gpu_descriptor(sources + k));
                command->DrawInstanced(3, 1, 0, 0);
                transition(command, over, readable);
            }
        }
        backdrop_passes += schedule.backdrops;
        backdrop_copies += schedule.copies;
    }

    bool SceneRenderer::encode(Slot& slot, const SceneFrame& frame, ID3D12GraphicsCommandList* command,
                               const Target& target, float scale, unsigned view_index, const SceneAnchorTable* anchors,
                               const SceneTargetStart& start, std::span<const SceneTexture> textures) {
        if (slot.state == Slot::State::Submitted) {
            retire(slot);
        }
        ensure(slot.state == Slot::State::Idle, "an idle or completed scene slot");
        ensure(command && target.resource && target.width && target.height &&
                   target.width <= unsigned(limits::max_surface_dimension) &&
                   target.height <= unsigned(limits::max_surface_dimension),
               "bounded D3D12 scene target");
        // Textures of images and layers that neither the preparer nor a slot holds any more: every recording that
        // sampled them completed.
        for (auto it = textures_.begin(); it != textures_.end();) {
            if (it->second.source.expired()) {
                gpu.destroy(it->second.image);
                it = textures_.erase(it);
            } else {
                ++it;
            }
        }
        // A frame of the revision the view's kept pass was prepared for records it again with the frame's anchors; a
        // frame of another revision patches it where the pass stands (ScenePreparer::patch).
        const bool keep = frame.revision != 0;
        auto& kept = kept_[view_index];
        std::shared_ptr<ScenePrepared> prepared;
        if (keep && kept.prepared && kept.scale == scale && kept.width == target.width &&
            kept.height == target.height && kept.prepared->over == start.over &&
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
            prepared = preparer_.prepare(frame, scale, target.width, target.height, std::nullopt, view_index, {},
                                         anchors, keep, start.over);
            if (!prepared) {
                last_failure_ = preparer_.last_fallback();
                return false;
            }
            if (prepared->keepable) {
                kept = {frame.revision,
                        scale,
                        target.width,
                        target.height,
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
        if (!gpu.filters(DXGI_FORMAT_R32G32B32A32_FLOAT) && linear(linear, *prepared)) {
            ++fallbacks;
            ++reasons["effect float texture filtering"];
            last_failure_ = "effect float texture filtering";
            return false;
        }
        slot_ = &slot;
        command_ = command;
        heap_ = nullptr;
        slot.recording = ++next_recording_;
        slot.device = &gpu;
        // The host's target as a texture whose state the pass follows: a render target, where it leaves it.
        Image host;
        host.resource = target.resource;
        host.format = target.format;
        host.width = target.width;
        host.height = target.height;
        host.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        try {
            check_pending(*prepared);
            // No reference to the host's target: the host keeps it while the commands run, and a swap chain's buffer
            // must lose every reference before the swap chain resizes.
            slot.target_rtv = gpu.render_target(target.resource, target.format, host.rtv);
            upload(*prepared);
            record(*prepared, {&host, host.rtv, host.format, prepared->width, prepared->height}, scale, placed, serial,
                   start.clear ? &*start.clear : nullptr, textures, serial ? &kept.history : nullptr);
            ensure(host.state == D3D12_RESOURCE_STATE_RENDER_TARGET, "the host's target left a render target");
        } catch (...) {
            // The host discards the commands: what they would upload or draw leaves the cache.
            for (const auto id : slot.created) {
                if (const auto it = textures_.find(id); it != textures_.end()) {
                    gpu.destroy(it->second.image);
                    textures_.erase(it);
                }
            }
            release(slot);
            slot_ = nullptr;
            command_ = nullptr;
            heap_ = nullptr;
            throw;
        }
        slot_ = nullptr;
        command_ = nullptr;
        heap_ = nullptr;
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
        for (const auto id : slot.created) {
            if (const auto it = textures_.find(id); it != textures_.end() && it->second.pending == slot.recording) {
                gpu.destroy(it->second.image);
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
} // namespace weft::render::d3d12
