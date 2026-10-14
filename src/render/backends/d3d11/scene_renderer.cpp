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
        static constexpr unsigned char particle_vs[] = {
#include "shaders/scene_particles.vs.inc"
        };
        static constexpr unsigned char particle_ps[] = {
#include "shaders/scene_particles.ps.inc"
        };
        throw_if_failed(gpu.device->CreateVertexShader(particle_vs, sizeof(particle_vs), nullptr, &particle_vs_),
                        "native particle vertex shader");
        throw_if_failed(gpu.device->CreatePixelShader(particle_ps, sizeof(particle_ps), nullptr, &particle_ps_),
                        "native particle pixel shader");
        static constexpr unsigned char backdrop_vs[] = {
#include "shaders/backdrop_blur.vs.inc"
        };
        static constexpr unsigned char area_ps[] = {
#include "shaders/backdrop_area.ps.inc"
        };
        static constexpr unsigned char blur_ps[] = {
#include "shaders/backdrop_blur.ps.inc"
        };
        static constexpr unsigned char matrix_ps[] = {
#include "shaders/backdrop_matrix.ps.inc"
        };
        throw_if_failed(gpu.device->CreateVertexShader(backdrop_vs, sizeof(backdrop_vs), nullptr, &backdrop_vs_),
                        "native backdrop vertex shader");
        throw_if_failed(gpu.device->CreatePixelShader(area_ps, sizeof(area_ps), nullptr, &area_ps_),
                        "native backdrop area shader");
        throw_if_failed(gpu.device->CreatePixelShader(blur_ps, sizeof(blur_ps), nullptr, &blur_ps_),
                        "native backdrop blur shader");
        throw_if_failed(gpu.device->CreatePixelShader(matrix_ps, sizeof(matrix_ps), nullptr, &matrix_ps_),
                        "native backdrop matrix shader");
        D3D11_SAMPLER_DESC mirror{};
        mirror.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        mirror.AddressU = mirror.AddressV = mirror.AddressW = D3D11_TEXTURE_ADDRESS_MIRROR;
        mirror.MaxLOD = D3D11_FLOAT32_MAX;
        throw_if_failed(gpu.device->CreateSamplerState(&mirror, &mirror_), "native backdrop sampler");
        // Destination over, premultiplied: what the group holds stays over the backdrop.
        D3D11_BLEND_DESC under{};
        auto& rt = under.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_INV_DEST_ALPHA;
        rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ONE;
        rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        throw_if_failed(gpu.device->CreateBlendState(&under, &under_), "native backdrop blend state");
        D3D11_BUFFER_DESC constants{};
        constants.ByteWidth = limits::backdrop_constants_bytes;
        constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        throw_if_failed(gpu.device->CreateBuffer(&constants, nullptr, &backdrop_constants_),
                        "native backdrop constants");
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

    SceneRenderer::Page& SceneRenderer::page(const SceneAtlasPage& key) {
        auto& page = pages_[{std::uint32_t(key.format), key.page}];
        if (!page.texture) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = desc.Height = scene_atlas_page_size(key.format);
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = key.format == SceneImage::Format::Data ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            throw_if_failed(gpu_.device->CreateTexture2D(&desc, nullptr, &page.texture), "native scene atlas page");
            throw_if_failed(gpu_.device->CreateShaderResourceView(page.texture.Get(), nullptr, &page.view),
                            "native scene atlas page view");
        }
        return page;
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
        if (const auto& at = image->atlas) {
            // Into its region of its page, inside its border. The immediate context orders the write after the draws
            // that sampled what the region held before.
            auto& on = page({image->format, at->page});
            constexpr auto border = std::uint32_t(constants::render::scene::atlas_border);
            const auto pixels = scene_atlas_pixels(*image);
            const D3D11_BOX box{
                at->x - border, at->y - border, 0, at->x + image->width + border, at->y + image->height + border, 1};
            gpu_.immediate->UpdateSubresource(on.texture.Get(), 0, &box, pixels.data(),
                                              UINT((image->width + 2 * border) * texel), 0);
            return textures_.emplace(image->id, Texture{image, on.view}).first->second.view.Get();
        }
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

    ID3D11ShaderResourceView* SceneRenderer::backdrop(const ScenePass::Backdrop& b, ID3D11RenderTargetView* from) {
        auto* ctx = gpu_.immediate.Get();
        ComPtr<ID3D11Resource> resource;
        from->GetResource(&resource);
        ComPtr<ID3D11Texture2D> source;
        throw_if_failed(resource.As(&source), "native backdrop source texture");
        D3D11_TEXTURE2D_DESC desc{};
        source->GetDesc(&desc);
        // A target of the chain: its place in the chain names it, so a backdrop's targets are distinct.
        unsigned place{};
        const auto target = [&](unsigned w, unsigned h, DXGI_FORMAT format) -> Surface& {
            auto& kept = backdrop_targets_[{unsigned(format), w, h, place++}];
            kept.used = backdrop_frame_;
            auto& s = kept.surface;
            if (!s.texture) {
                D3D11_TEXTURE2D_DESC d{};
                d.Width = w;
                d.Height = h;
                d.MipLevels = d.ArraySize = 1;
                d.Format = format;
                d.SampleDesc.Count = 1;
                d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                throw_if_failed(gpu_.device->CreateTexture2D(&d, nullptr, &s.texture), "native backdrop texture");
                throw_if_failed(gpu_.device->CreateRenderTargetView(s.texture.Get(), nullptr, &s.rtv),
                                "native backdrop RTV");
                throw_if_failed(gpu_.device->CreateShaderResourceView(s.texture.Get(), nullptr, &s.srv),
                                "native backdrop SRV");
            }
            return s;
        };
        auto w = unsigned(b.region[2] - b.region[0]);
        auto h = unsigned(b.region[3] - b.region[1]);
        auto* current = &target(w, h, desc.Format);
        const D3D11_BOX box{UINT(b.region[0]), UINT(b.region[1]), 0, UINT(b.region[2]), UINT(b.region[3]), 1};
        ctx->CopySubresourceRegion(current->texture.Get(), 0, 0, 0, 0, resource.Get(), 0, &box);
        ctx->ClearState();
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(backdrop_vs_.Get(), nullptr, 0);
        auto* sampler = mirror_.Get();
        ctx->PSSetSamplers(0, 1, &sampler);
        auto* constants = backdrop_constants_.Get();
        ctx->PSSetConstantBuffers(0, 1, &constants);
        // One fullscreen pass from the current image into a w x h target.
        const auto pass = [&](ID3D11PixelShader* shader, const BackdropConstants& values) {
            auto& out = target(w, h, DXGI_FORMAT_B8G8R8A8_UNORM);
            ctx->UpdateSubresource(constants, 0, nullptr, &values, 0, 0);
            ID3D11ShaderResourceView* none = nullptr;
            ctx->PSSetShaderResources(0, 1, &none);
            ctx->OMSetRenderTargets(1, out.rtv.GetAddressOf(), nullptr);
            const D3D11_VIEWPORT viewport{0, 0, float(w), float(h), 0, 1};
            ctx->RSSetViewports(1, &viewport);
            ctx->PSSetShader(shader, nullptr, 0);
            auto* view = current->srv.Get();
            ctx->PSSetShaderResources(0, 1, &view);
            ctx->Draw(3, 0);
            current = &out;
        };
        if (b.pre != paint::identity_color_matrix) {
            pass(matrix_ps_.Get(), backdrop_matrix_constants(b.pre));
        }
        if (b.size[0] < w) {
            const auto wide = w;
            w = b.size[0];
            pass(area_ps_.Get(), backdrop_area_constants(wide, w, true));
        }
        if (b.size[1] < h) {
            const auto tall = h;
            h = b.size[1];
            pass(area_ps_.Get(), backdrop_area_constants(tall, h, false));
        }
        if (b.sigma[0] > 0) {
            pass(blur_ps_.Get(), backdrop_blur_constants(b.sigma[0], true, w, h));
            pass(blur_ps_.Get(), backdrop_blur_constants(b.sigma[1], false, w, h));
        }
        ++backdrop_passes;
        return current->srv.Get();
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
        // Each emission's sprite, at t3.
        std::vector<ID3D11ShaderResourceView*> sprite_views(prepared.sprites.size());
        for (std::size_t s = 0; s < prepared.sprites.size(); ++s) {
            if (prepared.sprites[s]) {
                sprite_views[s] = texture(prepared.sprites[s]);
                slot.leases.push_back(prepared.sprites[s]);
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
        float blend[4]{};
        // The pass's state: set once, and again after a backdrop's passes replaced it.
        const auto bind = [&] {
            if (!pass.clips.empty()) {
                auto* clip_values = slot.clip_view.Get();
                ctx->VSSetShaderResources(2, 1, &clip_values);
                ctx->PSSetShaderResources(2, 1, &clip_values);
            }
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
            const D3D11_VIEWPORT vp{0, 0, float(target_width), float(target_height), 0, 1};
            ctx->RSSetViewports(1, &vp);
            const D3D11_RECT clip{0, 0, LONG(target_width), LONG(target_height)};
            ctx->RSSetScissorRects(1, &clip);
            ctx->OMSetBlendState(gpu_.blend.Get(), blend, UINT_MAX);
            auto* sampler = gpu_.sampler.Get();
            ctx->PSSetSamplers(0, 1, &sampler);
        };
        bind();
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
                if (batch.step == Step::Boxes || batch.step == Step::Atlas || batch.step == Step::EndGroup ||
                    batch.step == Step::Effect || batch.step == Step::Particles || batch.step == Step::Backdrop ||
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
        // count instances of a quad, or one instance of vertices (the particles' 6 per particle).
        auto instances = [&](unsigned start, unsigned count, ID3D11ShaderResourceView* image,
                             unsigned vertices = constants::render::d3d11::quad_vertex_count) {
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
            ctx->DrawInstanced(vertices, count, 0, 0);
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
                case Step::Atlas:
                    instances(batch.first, batch.count, page(prepared.pages.at(batch.source)).view.Get());
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
                case Step::Backdrop: {
                    // The backdrop's filtered copy of the target it reads (the current one, or the one enclosing
                    // the current group), drawn as its record over the current target or beneath its content.
                    const auto& b = pass.backdrops.at(batch.source);
                    ensure(!b.under || stack.size() > 1, "a backdrop beneath a group inside one");
                    auto* filtered = backdrop(b, b.under ? stack[stack.size() - 2] : stack.back());
                    bind();
                    target(stack.back());
                    if (b.under) {
                        ctx->OMSetBlendState(under_.Get(), blend, UINT_MAX);
                    }
                    instances(batch.first, 1, filtered);
                    if (b.under) {
                        ctx->OMSetBlendState(gpu_.blend.Get(), blend, UINT_MAX);
                    }
                    break;
                }
                case Step::Particles: {
                    // The particle pass over the emission's record, its sprite at t3.
                    ctx->VSSetShader(particle_vs_.Get(), nullptr, 0);
                    ctx->PSSetShader(particle_ps_.Get(), nullptr, 0);
                    auto* sprite = sprite_views.at(batch.source);
                    ctx->PSSetShaderResources(effect_texture_slot, 1, &sprite);
                    instances(batch.first, 1, nullptr, constants::render::d3d11::quad_vertex_count * batch.count);
                    ctx->VSSetShader(vs_.Get(), nullptr, 0);
                    ctx->PSSetShader(ps_.Get(), nullptr, 0);
                    break;
                }
            }
        }
        ctx->ClearState();
    }

    bool SceneRenderer::paint(Slot& slot, const SceneFrame& frame, ID3D11RenderTargetView* destination, float scale,
                              unsigned target_width, unsigned target_height, std::optional<std::array<float, 4>> crop,
                              unsigned view_index, const std::vector<NodeIdentity>& layers,
                              const SceneAnchorTable* anchors) {
        // Textures of images and layers that neither the preparer nor a slot holds any more, and backdrop targets
        // neither the last frame nor this one used (D3D11 keeps a released texture until the GPU is done with it).
        std::erase_if(textures_, [](const auto& texture) {
            return texture.second.source.expired();
        });
        ++backdrop_frame_;
        std::erase_if(backdrop_targets_, [&](const auto& target) {
            return target.second.used + 1 < backdrop_frame_;
        });
        const auto prepared =
            preparer_.prepare(frame, scale, target_width, target_height, crop, view_index, layers, anchors);
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
