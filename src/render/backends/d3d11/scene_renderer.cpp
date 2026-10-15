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
        // The register of an effect's first texture (scene.hlsl, effectTexture0); its others follow. Then the record
        // order and the anchors (order, anchors).
        constexpr UINT effect_texture_slot = 3;
        constexpr UINT order_slot = 7;
        constexpr UINT anchor_slot = 8;
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
        : SceneRenderer(gpu.device.Get(), text, bounds) {
    }

    SceneRenderer::SceneRenderer(ID3D11Device* device, SceneText& text, SceneCacheBounds bounds)
        : device_(device),
          reference_(text),
          preparer_(
              text, *this, std::uint32_t(constants::render::d3d11::max_surface_dimension),
              [this](const SceneFrame& frame, float scale) {
                  return reference_.raster(frame, scale);
              },
              bounds) {
        ensure(device_ != nullptr, "a D3D11 device");
        device_->GetImmediateContext(&immediate_);
        // The same states as the display list renderer's (Renderer): equal descriptions give the same state objects.
        D3D11_BLEND_DESC over{};
        auto& target = over.RenderTarget[0];
        target.BlendEnable = TRUE;
        target.SrcBlend = target.SrcBlendAlpha = D3D11_BLEND_ONE;
        target.DestBlend = target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        target.BlendOp = target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        throw_if_failed(device_->CreateBlendState(&over, &blend_), "native scene blend state");
        D3D11_SAMPLER_DESC linear{};
        linear.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        linear.AddressU = linear.AddressV = linear.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        linear.MaxLOD = D3D11_FLOAT32_MAX;
        throw_if_failed(device_->CreateSamplerState(&linear, &sampler_), "native scene sampler");
        linear.AddressU = linear.AddressV = linear.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        throw_if_failed(device_->CreateSamplerState(&linear, &effect_sampler_), "native scene effect sampler");
        // Compiled at build time from scene.hlsl, always /O3 (cmake/WeftShaders.cmake).
        static constexpr unsigned char vs[] = {
#include "shaders/scene_native.vs.inc"
        };
        static constexpr unsigned char ps[] = {
#include "shaders/scene_native.ps.inc"
        };
        throw_if_failed(device_->CreateVertexShader(vs, sizeof(vs), nullptr, &vs_), "native scene vertex shader");
        throw_if_failed(device_->CreatePixelShader(ps, sizeof(ps), nullptr, &ps_), "native scene pixel shader");
        static constexpr unsigned char particle_vs[] = {
#include "shaders/scene_particles.vs.inc"
        };
        static constexpr unsigned char particle_ps[] = {
#include "shaders/scene_particles.ps.inc"
        };
        throw_if_failed(device_->CreateVertexShader(particle_vs, sizeof(particle_vs), nullptr, &particle_vs_),
                        "native particle vertex shader");
        throw_if_failed(device_->CreatePixelShader(particle_ps, sizeof(particle_ps), nullptr, &particle_ps_),
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
        throw_if_failed(device_->CreateVertexShader(backdrop_vs, sizeof(backdrop_vs), nullptr, &backdrop_vs_),
                        "native backdrop vertex shader");
        throw_if_failed(device_->CreatePixelShader(area_ps, sizeof(area_ps), nullptr, &area_ps_),
                        "native backdrop area shader");
        throw_if_failed(device_->CreatePixelShader(blur_ps, sizeof(blur_ps), nullptr, &blur_ps_),
                        "native backdrop blur shader");
        throw_if_failed(device_->CreatePixelShader(matrix_ps, sizeof(matrix_ps), nullptr, &matrix_ps_),
                        "native backdrop matrix shader");
        D3D11_SAMPLER_DESC mirror{};
        mirror.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        mirror.AddressU = mirror.AddressV = mirror.AddressW = D3D11_TEXTURE_ADDRESS_MIRROR;
        mirror.MaxLOD = D3D11_FLOAT32_MAX;
        throw_if_failed(device_->CreateSamplerState(&mirror, &mirror_), "native backdrop sampler");
        // Destination over, premultiplied: what the group holds stays over the backdrop.
        D3D11_BLEND_DESC under{};
        auto& rt = under.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_INV_DEST_ALPHA;
        rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ONE;
        rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        throw_if_failed(device_->CreateBlendState(&under, &under_), "native backdrop blend state");
        D3D11_BUFFER_DESC constants{};
        constants.ByteWidth = limits::backdrop_constants_bytes;
        constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        throw_if_failed(device_->CreateBuffer(&constants, nullptr, &backdrop_constants_), "native backdrop constants");
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.ScissorEnable = TRUE;
        raster.DepthClipEnable = TRUE;
        throw_if_failed(device_->CreateRasterizerState(&raster, &raster_), "native scene raster state");
        D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
        offsets_supported_ =
            SUCCEEDED(immediate_.As(&context1_)) &&
            SUCCEEDED(device_->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options))) &&
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
            throw_if_failed(device_->CreateTexture2D(&desc, nullptr, &page.texture), "native scene atlas page");
            throw_if_failed(device_->CreateShaderResourceView(page.texture.Get(), nullptr, &page.view),
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
            immediate_->UpdateSubresource(on.texture.Get(), 0, &box, pixels.data(),
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
        throw_if_failed(device_->CreateTexture2D(&desc, &data, &pixels), "native scene image");
        throw_if_failed(device_->CreateShaderResourceView(pixels.Get(), nullptr, &texture.view),
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
        pass.target = group_target(device_.Get(), layer->width, layer->height);
        draw_pass(pass, *layer->content, pass.target.rtv.Get(), scale);
        return textures_.emplace(layer->id, Texture{layer, pass.target.srv}).first->second.view.Get();
    }

    ID3D11ShaderResourceView* SceneRenderer::backdrop(const ScenePass::Backdrop& b, ID3D11RenderTargetView* from) {
        auto* ctx = immediate_.Get();
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
                throw_if_failed(device_->CreateTexture2D(&d, nullptr, &s.texture), "native backdrop texture");
                throw_if_failed(device_->CreateRenderTargetView(s.texture.Get(), nullptr, &s.rtv),
                                "native backdrop RTV");
                throw_if_failed(device_->CreateShaderResourceView(s.texture.Get(), nullptr, &s.srv),
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
        throw_if_failed(device_->CreatePixelShader(program->d3d11_encoded.data(), program->d3d11_encoded.size(),
                                                   nullptr, &made.shader),
                        "native scene effect shader");
        return effect_shaders_.emplace(program.get(), std::move(made)).first->second.shader.Get();
    }

    void SceneRenderer::structured(ComPtr<ID3D11Buffer>& buffer, ComPtr<ID3D11ShaderResourceView>& view,
                                   unsigned& capacity, std::size_t count, unsigned stride, const char* what) {
        if (buffer && capacity >= count) {
            return;
        }
        unsigned grown = limits::record_initial_capacity;
        while (grown < count) {
            grown *= 2;
        }
        buffer.Reset();
        view.Reset();
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = grown * stride;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        d.StructureByteStride = stride;
        throw_if_failed(device_->CreateBuffer(&d, nullptr, &buffer), what);
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = DXGI_FORMAT_UNKNOWN;
        v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        v.Buffer.NumElements = grown;
        throw_if_failed(device_->CreateShaderResourceView(buffer.Get(), &v, &view), what);
        capacity = grown;
    }

    void SceneRenderer::draw_pass(Slot& slot, const ScenePrepared& prepared, ID3D11RenderTargetView* destination,
                                  float scale, const SceneAnchorFrame* anchors, std::uint64_t serial,
                                  const paint::Color* clear, std::span<const SceneTexture> host_textures) {
        // A kept pass the slot drew before: its records, rounded clips, plane contents and leases are the slot's.
        const bool again = serial && slot.kept == serial;
        // Plane contents first, each into its slot's image; then layers not drawn before, and the textures of the
        // images this pass samples.
        std::vector<ID3D11ShaderResourceView*> views(prepared.planes.size());
        if (!again) {
            slot.leases.clear();
        }
        std::size_t next{};
        for (std::size_t i = 0; i < prepared.planes.size(); ++i) {
            if (prepared.planes[i]) {
                auto& nested = *slot.planes.at(next++);
                if (!nested.target.texture) {
                    nested.target = group_target(device_.Get(), nested.width, nested.height);
                }
                if (!again) {
                    draw_pass(nested, *prepared.planes[i], nested.target.rtv.Get(), scale, nullptr, 0, nullptr,
                              host_textures);
                    ++plane_passes;
                }
                views[i] = nested.target.srv.Get();
            } else if (again) {
                if (prepared.layers[i]) {
                    views[i] = texture(prepared.layers[i], scale);
                } else if (prepared.images[i] && !prepared.images[i]->atlas) {
                    views[i] = texture(prepared.images[i]);
                }
            } else if (prepared.layers[i]) {
                views[i] = texture(prepared.layers[i], scale);
                slot.leases.push_back(prepared.layers[i]);
            } else if (prepared.images[i]) {
                views[i] = texture(prepared.images[i]);
                slot.leases.push_back(prepared.images[i]);
            }
        }
        // The host texture bound to each texture item's name; none, and nothing drawn, for an unbound one.
        const auto host = scene_texture_bindings(prepared, host_textures);
        for (std::size_t i = 0; i < prepared.textures.size(); ++i) {
            if (const auto t = prepared.textures[i]) {
                if (const auto* texture = host[t - 1]) {
                    views[i] = reinterpret_cast<ID3D11ShaderResourceView*>(static_cast<std::uintptr_t>(texture->view));
                    ++texture_draws;
                } else {
                    ++unbound_textures;
                }
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
                if (!again) {
                    slot.leases.push_back(textures[t]);
                }
            }
        }
        // Each emission's sprite, at t3.
        std::vector<ID3D11ShaderResourceView*> sprite_views(prepared.sprites.size());
        for (std::size_t s = 0; s < prepared.sprites.size(); ++s) {
            if (prepared.sprites[s]) {
                sprite_views[s] = texture(prepared.sprites[s]);
                if (!again) {
                    slot.leases.push_back(prepared.sprites[s]);
                }
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
            throw_if_failed(device_->CreateBuffer(&d, nullptr, &slot.records), "native instance buffer");
            D3D11_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = DXGI_FORMAT_UNKNOWN;
            v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            v.Buffer.NumElements = capacity;
            throw_if_failed(device_->CreateShaderResourceView(slot.records.Get(), &v, &slot.view),
                            "native instance view");
            d = {};
            d.ByteWidth = limits::view_constants_bytes;
            d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            throw_if_failed(device_->CreateBuffer(&d, nullptr, &slot.constants), "native view constants");
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
            throw_if_failed(device_->CreateBuffer(&d, nullptr, &clips), "native rounded clip buffer");
            D3D11_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = DXGI_FORMAT_UNKNOWN;
            v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            v.Buffer.NumElements = capacity;
            throw_if_failed(device_->CreateShaderResourceView(clips.Get(), &v, &clip_view), "native rounded clip view");
            slot.clips = std::move(clips);
            slot.clip_view = std::move(clip_view);
            slot.clip_capacity = capacity;
        }
        auto* ctx = immediate_.Get();
        ctx->ClearState();
        if (!records.empty() && !again) {
            // The records of host textures whose rows start at the bottom sample them flipped.
            const auto* data = records.data();
            std::vector<Record> flipped;
            if (const auto flips = scene_texture_flips(prepared, host_textures);
                std::find(flips.begin(), flips.end(), true) != flips.end()) {
                flipped = records;
                for (std::size_t i = 0; i < flipped.size(); ++i) {
                    if (prepared.textures[i] && flips[prepared.textures[i] - 1]) {
                        flip_rows(flipped[i]);
                    }
                }
                data = flipped.data();
            }
            D3D11_BOX box{0, 0, 0, UINT(records.size() * sizeof(Record)), 1, 1};
            ctx->UpdateSubresource(slot.records.Get(), 0, &box, data, 0, 0);
        }
        if (!pass.clips.empty() && !again) {
            D3D11_BOX box{0, 0, 0, UINT(pass.clips.size() * sizeof(SceneClip)), 1, 1};
            ctx->UpdateSubresource(slot.clips.Get(), 0, &box, pass.clips.data(), 0, 0);
        }
        // The record each draw position takes (t7), and the frame's anchors (t8).
        ID3D11ShaderResourceView* order_values{};
        if (anchors && !anchors->order.empty()) {
            ensure(anchors->order.size() == records.size(), "a record order of the pass's records");
            structured(slot.order, slot.order_view, slot.order_capacity, records.size(), sizeof(std::uint32_t),
                       "native record order");
            D3D11_BOX box{0, 0, 0, UINT(records.size() * sizeof(std::uint32_t)), 1, 1};
            ctx->UpdateSubresource(slot.order.Get(), 0, &box, anchors->order.data(), 0, 0);
            order_values = slot.order_view.Get();
        } else {
            if (in_order_capacity_ < records.size()) {
                in_order_.Reset();
                structured(in_order_, in_order_view_, in_order_capacity_, records.size(), sizeof(std::uint32_t),
                           "native record order");
                std::vector<std::uint32_t> places(in_order_capacity_);
                for (std::size_t i = 0; i < places.size(); ++i) {
                    places[i] = std::uint32_t(i);
                }
                ctx->UpdateSubresource(in_order_.Get(), 0, nullptr, places.data(), 0, 0);
            }
            order_values = in_order_view_.Get();
        }
        ID3D11ShaderResourceView* anchor_values{};
        if (anchors && !anchors->anchors.empty()) {
            ensure(anchors->anchors.size() == prepared.anchors.size(), "the anchors of the pass's anchored elements");
            structured(slot.anchors, slot.anchor_view, slot.anchor_capacity, anchors->anchors.size(),
                       sizeof(anchors->anchors[0]), "native anchors");
            D3D11_BOX box{0, 0, 0, UINT(anchors->anchors.size() * sizeof(anchors->anchors[0])), 1, 1};
            ctx->UpdateSubresource(slot.anchors.Get(), 0, &box, anchors->anchors.data(), 0, 0);
            anchor_values = slot.anchor_view.Get();
        } else {
            ensure(prepared.anchors.empty(), "the anchors of the pass's anchored elements");
            if (!no_anchors_) {
                unsigned capacity{};
                structured(no_anchors_, no_anchors_view_, capacity, 1, sizeof(std::array<float, 4>), "native anchors");
            }
            anchor_values = no_anchors_view_.Get();
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
            ctx->VSSetShaderResources(order_slot, 1, &order_values);
            ctx->VSSetShaderResources(anchor_slot, 1, &anchor_values);
            ctx->PSSetShaderResources(anchor_slot, 1, &anchor_values);
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
            ctx->OMSetBlendState(blend_.Get(), blend, UINT_MAX);
            // The clamping sampler, and the repeating one host textures take (scene.hlsl, linearRepeat).
            ID3D11SamplerState* samplers[]{sampler_.Get(), effect_sampler_.Get()};
            ctx->PSSetSamplers(0, 2, samplers);
        };
        bind();
        const Color transparent{};
        if (!prepared.over) {
            ctx->ClearRenderTargetView(destination, pass.clear.data());
        } else if (clear) {
            ctx->ClearRenderTargetView(destination, clear->data());
        }
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
                throw_if_failed(device_->CreateBuffer(&d, nullptr, &slot.draw_constants), "native draw constants");
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
                        s = group_target(device_.Get(), target_width, target_height);
                    }
                    target(s.rtv.Get());
                    ctx->ClearRenderTargetView(s.rtv.Get(), transparent.data());
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
                    auto* repeat = effect_sampler_.Get();
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
                        ctx->OMSetBlendState(blend_.Get(), blend, UINT_MAX);
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
        slot.kept = serial;
    }

    bool SceneRenderer::paint(Slot& slot, const SceneFrame& frame, ID3D11RenderTargetView* destination, float scale,
                              unsigned target_width, unsigned target_height, std::optional<std::array<float, 4>> crop,
                              unsigned view_index, const std::vector<NodeIdentity>& layers,
                              const SceneAnchorTable* anchors, const SceneTargetStart& start,
                              std::span<const SceneTexture> textures) {
        // Textures of images and layers that neither the preparer nor a slot holds any more, and backdrop targets
        // neither the last frame nor this one used (D3D11 keeps a released texture until the GPU is done with it).
        std::erase_if(textures_, [](const auto& texture) {
            return texture.second.source.expired();
        });
        ++backdrop_frame_;
        std::erase_if(backdrop_targets_, [&](const auto& target) {
            return target.second.used + 1 < backdrop_frame_;
        });
        // The caller has confirmed this slot's prior real submission complete. Immutable image leases survive cache
        // eviction and the entire next submission; buffers and targets are never rewritten in flight.
        const auto fits = [&](const ScenePrepared& prepared) {
            const auto held = held_bytes(slot);
            if (scratch_bytes_ - held + scene_scratch_bytes(prepared) > limits::scratch_budget_bytes) {
                ++fallbacks;
                ++reasons["native scratch budget reference"];
                return false;
            }
            shape(slot, prepared, false);
            scratch_bytes_ = scratch_bytes_ - held + held_bytes(slot);
            return true;
        };
        // A frame of the revision the view's kept pass was prepared for draws it again with the frame's anchors.
        const bool keep = frame.revision && !crop;
        auto& kept = kept_[view_index];
        const auto* clear = start.clear ? &*start.clear : nullptr;
        if (keep && kept.prepared && kept.revision == frame.revision && kept.scale == scale &&
            kept.width == target_width && kept.height == target_height && kept.layers == layers &&
            kept.prepared->over == start.over && scene_texture_flips(*kept.prepared, textures) == kept.flips) {
            if (preparer_.place(*kept.prepared, view_index, anchors, kept.frame)) {
                if (!fits(*kept.prepared)) {
                    return false;
                }
                draw_pass(slot, *kept.prepared, destination, scale, &kept.frame, kept.serial, clear, textures);
                ++passes;
                ++kept_passes;
                return true;
            }
            ++kept_rebuilds;
        }
        kept = {};
        std::shared_ptr<const ScenePrepared> prepared = preparer_.prepare(
            frame, scale, target_width, target_height, crop, view_index, layers, anchors, keep, start.over);
        if (!prepared || !fits(*prepared)) {
            return false;
        }
        if (!prepared->keepable) {
            draw_pass(slot, *prepared, destination, scale, nullptr, 0, clear, textures);
            ++passes;
            return true;
        }
        kept = {frame.revision, scale,  target_width,
                target_height,  layers, prepared,
                ++serial_,      {},     scene_texture_flips(*prepared, textures)};
        ensure(preparer_.place(*prepared, view_index, anchors, kept.frame), "a pass placed as it was planned");
        draw_pass(slot, *prepared, destination, scale, &kept.frame, kept.serial, clear, textures);
        ++passes;
        return true;
    }
} // namespace weft::render::d3d11
