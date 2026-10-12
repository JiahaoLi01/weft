#include "render/backends/d3d11/scene_renderer.hpp"

#include "constants/common.hpp"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace weft::render::d3d11 {
    namespace {
        namespace limits = constants::render::scene;
        using Color = std::array<float, 4>;
        // The register of an effect's first texture (scene.hlsl, effectTexture0); its others follow.
        constexpr UINT effect_texture_slot = 3;
        using Record = SceneRecord;
        using Step = ScenePass::Step;

        Surface group_target(ID3D11Device* device, unsigned w, unsigned h) {
            Surface s;
            D3D11_TEXTURE2D_DESC d{};
            d.Width = w;
            d.Height = h;
            d.MipLevels = d.ArraySize = 1;
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            throw_if_failed(device->CreateTexture2D(&d, nullptr, &s.texture), "native group texture");
            throw_if_failed(device->CreateRenderTargetView(s.texture.Get(), nullptr, &s.rtv), "native group RTV");
            throw_if_failed(device->CreateShaderResourceView(s.texture.Get(), nullptr, &s.srv), "native group SRV");
            return s;
        }
    } // namespace

    SceneRenderer::SceneRenderer(Renderer& gpu, SceneText& text, SceneCacheBounds bounds)
        : gpu_(gpu),
          reference_(text),
          preparer_(
              text, *this, std::uint32_t(constants::render::d3d11::max_surface_dimension),
              [this](const SceneFrame& frame, float scale) {
                  return reference_.raster(frame, scale);
              },
              bounds) {
        // Compiled at build time from scene.hlsl, always /O3 (cmake/WeftShaders.cmake).
        static constexpr unsigned char vs[] = {
#include "shaders/scene_native.vs.inc"
        };
        static constexpr unsigned char ps[] = {
#include "shaders/scene_native.ps.inc"
        };
        throw_if_failed(gpu.device->CreateVertexShader(vs, sizeof(vs), nullptr, &vs_), "native scene vertex shader");
        throw_if_failed(gpu.device->CreatePixelShader(ps, sizeof(ps), nullptr, &ps_), "native scene pixel shader");
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.ScissorEnable = TRUE;
        raster.DepthClipEnable = TRUE;
        throw_if_failed(gpu.device->CreateRasterizerState(&raster, &raster_), "native scene raster state");
        D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
        offsets_supported_ =
            SUCCEEDED(gpu.immediate.As(&context1_)) &&
            SUCCEEDED(gpu.device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options))) &&
            options.ConstantBufferOffsetting;
        constant_offsets = offsets_supported_;
    }

    void SceneRenderer::retire(Slot& slot) {
        const auto bytes = held_bytes(slot);
        ensure(bytes <= scratch_bytes_, "native scratch retirement accounting");
        scratch_bytes_ -= bytes;
        slot = Slot{};
    }

    std::size_t SceneRenderer::held_bytes(const Slot& slot) {
        auto bytes = slot.group_bytes;
        for (const auto& nested : slot.planes) {
            bytes += held_bytes(*nested);
        }
        return bytes;
    }

    void SceneRenderer::shape(Slot& slot, const ScenePrepared& prepared, bool plane) {
        const auto bytes = std::size_t(prepared.width) * prepared.height * constants::rgba8_bytes_per_pixel *
                           (prepared.pass.group_depth + (plane ? 1 : 0));
        if (slot.width != prepared.width || slot.height != prepared.height || slot.group_bytes != bytes) {
            slot.groups = {};
            slot.target = {};
            slot.width = prepared.width;
            slot.height = prepared.height;
            slot.group_bytes = bytes;
        }
        const auto count = std::size_t(
            std::count_if(prepared.planes.begin(), prepared.planes.end(), [](const std::unique_ptr<ScenePrepared>& p) {
                return bool(p);
            }));
        slot.planes.resize(count);
        std::size_t next{};
        for (const auto& nested : prepared.planes) {
            if (nested) {
                auto& s = slot.planes[next++];
                if (!s) {
                    s = std::make_unique<Slot>();
                }
                shape(*s, *nested, true);
            }
        }
    }

    ID3D11ShaderResourceView* SceneRenderer::texture(const std::shared_ptr<const SceneImage>& image) {
        if (const auto it = textures_.find(image->id); it != textures_.end()) {
            return it->second.view.Get();
        }
        // Each format as the shader samples it: encoded premultiplied bytes as they are, data in r, sRGB decoded to
        // linear light before filtering, linear floats.
        using Format = SceneImage::Format;
        const auto texel = scene_image_texel_bytes(image->format);
        ensure(image->width && image->height &&
                   image->pixels.size() == std::size_t(image->width) * image->height * texel,
               "scene image size");
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = image->width;
        desc.Height = image->height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = image->format == Format::Bgra   ? DXGI_FORMAT_B8G8R8A8_UNORM
                      : image->format == Format::Data ? DXGI_FORMAT_R8_UNORM
                      : image->format == Format::Srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                                      : DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{image->pixels.data(), UINT(image->width * texel), 0};
        ComPtr<ID3D11Texture2D> pixels;
        Texture texture{image, {}};
        throw_if_failed(gpu_.device->CreateTexture2D(&desc, &data, &pixels), "native scene image");
        throw_if_failed(gpu_.device->CreateShaderResourceView(pixels.Get(), nullptr, &texture.view),
                        "native scene image view");
        return textures_.emplace(image->id, std::move(texture)).first->second.view.Get();
    }

    ID3D11ShaderResourceView* SceneRenderer::texture(const std::shared_ptr<const SceneLayerImage>& layer, float scale) {
        if (const auto it = textures_.find(layer->id); it != textures_.end()) {
            return it->second.view.Get();
        }
        // The pass's buffers and targets serve this pass alone: D3D11 keeps them until the GPU is done with them.
        Slot pass;
        shape(pass, *layer->content, true);
        pass.target = group_target(gpu_.device.Get(), layer->width, layer->height);
        draw_pass(pass, *layer->content, pass.target.rtv.Get(), scale);
        return textures_.emplace(layer->id, Texture{layer, pass.target.srv}).first->second.view.Get();
    }

    ID3D11PixelShader* SceneRenderer::effect_shader(const std::shared_ptr<const paint::EffectProgram>& program) {
        if (const auto found = effect_shaders_.find(program.get()); found != effect_shaders_.end()) {
            return found->second.shader.Get();
        }
        ensure(!program->d3d11_encoded.empty(), "an effect built for D3D11");
        EffectShader made{program, {}};
        throw_if_failed(gpu_.device->CreatePixelShader(program->d3d11_encoded.data(), program->d3d11_encoded.size(),
                                                       nullptr, &made.shader),
                        "native scene effect shader");
        return effect_shaders_.emplace(program.get(), std::move(made)).first->second.shader.Get();
    }

    void SceneRenderer::draw_pass(Slot& slot, const ScenePrepared& prepared, ID3D11RenderTargetView* destination,
                                  float scale) {
        // Plane contents first, each into its slot's image; then layers not drawn before, and the textures of the
        // images this pass samples.
        std::vector<ID3D11ShaderResourceView*> views(prepared.planes.size());
        slot.leases.clear();
        std::size_t next{};
        for (std::size_t i = 0; i < prepared.planes.size(); ++i) {
            if (prepared.planes[i]) {
                auto& nested = *slot.planes.at(next++);
                if (!nested.target.texture) {
                    nested.target = group_target(gpu_.device.Get(), nested.width, nested.height);
                }
                draw_pass(nested, *prepared.planes[i], nested.target.rtv.Get(), scale);
                views[i] = nested.target.srv.Get();
                ++plane_passes;
            } else if (prepared.layers[i]) {
                views[i] = texture(prepared.layers[i], scale);
                slot.leases.push_back(prepared.layers[i]);
            } else if (prepared.images[i]) {
                views[i] = texture(prepared.images[i]);
                slot.leases.push_back(prepared.images[i]);
            }
        }
        // Each effect's textures, at t3 on.
        std::vector<std::array<ID3D11ShaderResourceView*, constants::max_effect_textures>> effect_views(
            prepared.effects.size());
        for (std::size_t e = 0; e < prepared.effects.size(); ++e) {
            const auto& textures = prepared.effects[e].textures;
            ensure(textures.size() <= constants::max_effect_textures, "effect textures");
            for (std::size_t t = 0; t < textures.size(); ++t) {
                effect_views[e][t] = texture(textures[t]);
                slot.leases.push_back(textures[t]);
            }
        }
        const auto& pass = prepared.pass;
        const auto& records = pass.records;
        const auto target_width = prepared.width;
        const auto target_height = prepared.height;
        if (!slot.records || slot.capacity < records.size()) {
            unsigned capacity = limits::record_initial_capacity;
            while (capacity < records.size()) {
                capacity *= 2;
            }
            slot.capacity = capacity;
            slot.view.Reset();
            slot.records.Reset();
            D3D11_BUFFER_DESC d{};
            d.ByteWidth = capacity * sizeof(Record);
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            d.StructureByteStride = sizeof(Record);
            throw_if_failed(gpu_.device->CreateBuffer(&d, nullptr, &slot.records), "native instance buffer");
            D3D11_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = DXGI_FORMAT_UNKNOWN;
            v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            v.Buffer.NumElements = capacity;
            throw_if_failed(gpu_.device->CreateShaderResourceView(slot.records.Get(), &v, &slot.view),
                            "native instance view");
            d = {};
            d.ByteWidth = limits::view_constants_bytes;
            d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            throw_if_failed(gpu_.device->CreateBuffer(&d, nullptr, &slot.constants), "native view constants");
        }
        if (!pass.clips.empty() && slot.clip_capacity < pass.clips.size()) {
            unsigned capacity = limits::record_initial_capacity;
            while (capacity < pass.clips.size()) {
                capacity *= 2;
            }
            ComPtr<ID3D11Buffer> clips;
            ComPtr<ID3D11ShaderResourceView> clip_view;
            D3D11_BUFFER_DESC d{};
            d.ByteWidth = capacity * sizeof(SceneClip);
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            d.StructureByteStride = sizeof(SceneClip);
            throw_if_failed(gpu_.device->CreateBuffer(&d, nullptr, &clips), "native rounded clip buffer");
            D3D11_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = DXGI_FORMAT_UNKNOWN;
            v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            v.Buffer.NumElements = capacity;
            throw_if_failed(gpu_.device->CreateShaderResourceView(clips.Get(), &v, &clip_view),
                            "native rounded clip view");
            slot.clips = std::move(clips);
            slot.clip_view = std::move(clip_view);
            slot.clip_capacity = capacity;
        }
        auto* ctx = gpu_.immediate.Get();
        ctx->ClearState();
        if (!records.empty()) {
            D3D11_BOX box{0, 0, 0, UINT(records.size() * sizeof(Record)), 1, 1};
            ctx->UpdateSubresource(slot.records.Get(), 0, &box, records.data(), 0, 0);
        }
        if (!pass.clips.empty()) {
            D3D11_BOX box{0, 0, 0, UINT(pass.clips.size() * sizeof(SceneClip)), 1, 1};
            ctx->UpdateSubresource(slot.clips.Get(), 0, &box, pass.clips.data(), 0, 0);
            auto* clip_values = slot.clip_view.Get();
            ctx->VSSetShaderResources(2, 1, &clip_values);
            ctx->PSSetShaderResources(2, 1, &clip_values);
        }
        Color view{float(target_width), float(target_height), scale, 0};
        ctx->UpdateSubresource(slot.constants.Get(), 0, nullptr, view.data(), 0, 0);
        auto* buffer = slot.constants.Get();
        ctx->VSSetConstantBuffers(0, 1, &buffer);
        ctx->PSSetConstantBuffers(0, 1, &buffer);
        auto* values = slot.view.Get();
        ctx->VSSetShaderResources(0, 1, &values);
        ctx->PSSetShaderResources(0, 1, &values);
        ctx->VSSetShader(vs_.Get(), nullptr, 0);
        ctx->PSSetShader(ps_.Get(), nullptr, 0);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->RSSetState(raster_.Get());
        D3D11_VIEWPORT vp{0, 0, float(target_width), float(target_height), 0, 1};
        ctx->RSSetViewports(1, &vp);
        D3D11_RECT clip{0, 0, LONG(target_width), LONG(target_height)};
        ctx->RSSetScissorRects(1, &clip);
        float blend[4]{};
        ctx->OMSetBlendState(gpu_.blend.Get(), blend, UINT_MAX);
        auto* sampler = gpu_.sampler.Get();
        ctx->PSSetSamplers(0, 1, &sampler);
        Color clear{};
        ctx->ClearRenderTargetView(destination, pass.clear.data());
        ctx->OMSetRenderTargets(1, &destination, nullptr);
        unsigned depth{};
        std::vector<ID3D11RenderTargetView*> stack{destination};
        // The first record of each draw, in draw order: its view constants go to the GPU a chunk of draws at a time,
        // bound by offset, rather than in an update per draw.
        std::vector<unsigned> starts;
        const bool offsets = constant_offsets && offsets_supported_;
        if (offsets) {
            for (const auto& batch : pass.batches) {
                if (batch.step == Step::Boxes || batch.step == Step::EndGroup || batch.step == Step::Effect ||
                    (batch.step == Step::Image && views[batch.first])) {
                    starts.push_back(batch.first);
                }
            }
            if (!starts.empty() && !slot.draw_constants) {
                D3D11_BUFFER_DESC d{};
                d.ByteWidth = constants::render::d3d11::max_constant_buffer_bytes;
                d.Usage = D3D11_USAGE_DYNAMIC;
                d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                throw_if_failed(gpu_.device->CreateBuffer(&d, nullptr, &slot.draw_constants), "native draw constants");
            }
        }
        constexpr unsigned stride = constants::render::d3d11::constant_buffer_offset_bytes;
        constexpr unsigned per_chunk = constants::render::d3d11::max_constant_buffer_bytes / stride;
        std::size_t next_draw{};
        auto instances = [&](unsigned start, unsigned count, ID3D11ShaderResourceView* image) {
            if (offsets) {
                const auto at = unsigned(next_draw % per_chunk);
                if (!at) {
                    // A new chunk: discarding renames the buffer, so the chunk's draws keep the values they bound.
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    throw_if_failed(ctx->Map(slot.draw_constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
                                    "native draw constants map");
                    const auto end = std::min(starts.size(), next_draw + per_chunk);
                    for (auto i = next_draw; i < end; ++i) {
                        view[3] = float(starts[i]);
                        std::memcpy(static_cast<std::uint8_t*>(mapped.pData) + (i - next_draw) * stride, view.data(),
                                    sizeof(view));
                    }
                    ctx->Unmap(slot.draw_constants.Get(), 0);
                }
                ensure(starts.at(next_draw) == start, "native draw constants in draw order");
                const UINT first = at * stride / constants::render::scene::view_constants_bytes;
                const UINT count_constants = stride / constants::render::scene::view_constants_bytes;
                auto* constants_buffer = slot.draw_constants.Get();
                context1_->VSSetConstantBuffers1(0, 1, &constants_buffer, &first, &count_constants);
                context1_->PSSetConstantBuffers1(0, 1, &constants_buffer, &first, &count_constants);
                ++next_draw;
            } else {
                view[3] = float(start);
                ctx->UpdateSubresource(slot.constants.Get(), 0, nullptr, view.data(), 0, 0);
            }
            ctx->PSSetShaderResources(1, 1, &image);
            ctx->DrawInstanced(constants::render::d3d11::quad_vertex_count, count, 0, 0);
            ++draws;
        };
        // A target bound for output samples nothing: the image last drawn goes before the target changes.
        auto target = [&](ID3D11RenderTargetView* rtv) {
            ID3D11ShaderResourceView* empty = nullptr;
            ctx->PSSetShaderResources(1, 1, &empty);
            ctx->OMSetRenderTargets(1, &rtv, nullptr);
        };
        for (const auto& batch : pass.batches) {
            switch (batch.step) {
                case Step::BeginGroup: {
                    auto& s = slot.groups.at(depth++);
                    if (!s.texture) {
                        s = group_target(gpu_.device.Get(), target_width, target_height);
                    }
                    target(s.rtv.Get());
                    ctx->ClearRenderTargetView(s.rtv.Get(), clear.data());
                    stack.push_back(s.rtv.Get());
                    break;
                }
                case Step::EndGroup: {
                    auto& s = slot.groups.at(--depth);
                    stack.pop_back();
                    target(stack.back());
                    instances(batch.first, 1, s.srv.Get());
                    break;
                }
                case Step::Image:
                    // A path with nothing inside its clip has no image and draws nothing.
                    if (views[batch.first]) {
                        instances(batch.first, 1, views[batch.first]);
                    }
                    break;
                case Step::Boxes:
                    instances(batch.first, batch.count, nullptr);
                    break;
                case Step::Effect: {
                    // The effect's encoded pass over its records, its textures at t3 on, repeated by s1.
                    const auto& effect = prepared.effects.at(batch.source);
                    ctx->PSSetShader(effect_shader(effect.program), nullptr, 0);
                    ctx->PSSetShaderResources(effect_texture_slot, UINT(effect_views[batch.source].size()),
                                              effect_views[batch.source].data());
                    auto* repeat = gpu_.effect_sampler.Get();
                    ctx->PSSetSamplers(1, 1, &repeat);
                    instances(batch.first, batch.count, nullptr);
                    ctx->PSSetShader(ps_.Get(), nullptr, 0);
                    break;
                }
            }
        }
        ctx->ClearState();
    }

    bool SceneRenderer::paint(Slot& slot, const SceneFrame& frame, ID3D11RenderTargetView* destination, float scale,
                              unsigned target_width, unsigned target_height, std::optional<std::array<float, 4>> crop,
                              unsigned view_index, const std::vector<NodeIdentity>& layers) {
        // Textures of images and layers that neither the preparer nor a slot holds any more.
        std::erase_if(textures_, [](const auto& texture) {
            return texture.second.source.expired();
        });
        const auto prepared = preparer_.prepare(frame, scale, target_width, target_height, crop, view_index, layers);
        if (!prepared) {
            return false;
        }
        // The caller has confirmed this slot's prior real submission complete. Immutable image leases survive cache
        // eviction and the entire next submission; buffers and targets are never rewritten in flight.
        const auto held = held_bytes(slot);
        if (scratch_bytes_ - held + scene_scratch_bytes(*prepared) > limits::scratch_budget_bytes) {
            ++fallbacks;
            ++reasons["native scratch budget reference"];
            return false;
        }
        shape(slot, *prepared, false);
        scratch_bytes_ = scratch_bytes_ - held + held_bytes(slot);
        draw_pass(slot, *prepared, destination, scale);
        ++passes;
        return true;
    }
} // namespace weft::render::d3d11
