#include "render/backends/d3d11/scene_renderer.hpp"

#include "constants/common.hpp"
#include "render/shadow_image.hpp"
#include "scene/scene_paint.hpp"
#include "scene/scene_text_image.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace weft::render::d3d11 {
    namespace {
        namespace limits = constants::render::scene;
        using Kind = paint::Item::Kind;
        using Color = std::array<float, 4>;
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

    SceneRenderer::SceneRenderer(Renderer& gpu, SceneText& text)
        : gpu_(gpu), text_(text), reference_(text), geometry_(text) {
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
    }

    void SceneRenderer::retire(Slot& slot) {
        const auto bytes = held_bytes(slot);
        ensure(bytes <= scratch_bytes_, "native scratch retirement accounting");
        scratch_bytes_ -= bytes;
        slot = Slot{};
    }

    std::size_t SceneRenderer::scratch_of(const Prepared& prepared, bool plane) {
        auto bytes = std::size_t(prepared.width) * prepared.height * 4 * (prepared.pass.group_depth + (plane ? 1 : 0));
        for (const auto& nested : prepared.planes) {
            if (nested) {
                bytes += scratch_of(*nested, true);
            }
        }
        return bytes;
    }

    std::size_t SceneRenderer::held_bytes(const Slot& slot) {
        auto bytes = slot.group_bytes;
        for (const auto& nested : slot.planes) {
            bytes += held_bytes(*nested);
        }
        return bytes;
    }

    void SceneRenderer::shape(Slot& slot, const Prepared& prepared, bool plane) {
        const auto bytes =
            std::size_t(prepared.width) * prepared.height * 4 * (prepared.pass.group_depth + (plane ? 1 : 0));
        if (slot.width != prepared.width || slot.height != prepared.height || slot.group_bytes != bytes) {
            slot.groups = {};
            slot.target = {};
            slot.width = prepared.width;
            slot.height = prepared.height;
            slot.group_bytes = bytes;
        }
        const auto count = std::size_t(
            std::count_if(prepared.planes.begin(), prepared.planes.end(), [](const std::unique_ptr<Prepared>& p) {
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

    std::shared_ptr<const SceneRenderer::Image>
    SceneRenderer::image(const paint::Item& item, const paint::Raster& raster, float scale, unsigned view_index) {
        const auto& n = *raster.source;
        std::array<std::uint64_t, 4> id{view_index, n.identity.view_epoch, n.identity.slot, n.identity.generation};
        auto it = images_.find(id);
        if (it != images_.end() && scene_paint_source_equal(it->second->key, n) && it->second->scale == scale) {
            if (item.kind == Kind::Layer) {
                ++layer_reuses;
            } else {
                ++text_reuses;
            }
            return it->second;
        }
        if (item.kind == Kind::Layer) {
            bool supported = true;
            auto inspect = [&](auto&& self, const SceneNode& node) -> void {
                if (!node.space.empty() && node.space != constants::xhtml_namespace_uri) {
                    supported = false;
                }
                const auto* t = node.style.find(PropertyId::Transform);
                if (t && t->text().find("perspective(") != std::string::npos) {
                    supported = false;
                }
                if (!node.text.empty()) {
                    auto m = text_.measure(node.text, node.style);
                    if (m.width > node.box[2] + limits::text_fit_tolerance ||
                        m.height > node.box[3] + limits::text_fit_tolerance) {
                        supported = false;
                    }
                }
                for (const auto& child : node.children) {
                    self(self, child);
                }
            };
            inspect(inspect, n);
            const auto* overflow = n.style.find(PropertyId::Overflow);
            if (!overflow || overflow->text() != "hidden") {
                SceneFrame bounds;
                bounds.root = n;
                for (const auto& e : geometry_.geometry(bounds)) {
                    if (e.rect[0] < -limits::layer_bounds_tolerance || e.rect[1] < -limits::layer_bounds_tolerance ||
                        e.rect[0] + e.rect[2] > n.box[2] + limits::layer_bounds_tolerance ||
                        e.rect[1] + e.rect[3] > n.box[3] + limits::layer_bounds_tolerance) {
                        supported = false;
                    }
                }
            }
            if (!supported) {
                throw std::invalid_argument("explicit native layer bounds reference");
            }
        }
        auto metrics = text_.measure(n.text, n.style);
        const auto* overflow = n.style.find(PropertyId::Overflow);
        if (item.kind == Kind::Text && (!overflow || overflow->text() != "hidden") &&
            (metrics.width > n.box[2] + limits::text_fit_tolerance ||
             metrics.height > n.box[3] + limits::text_fit_tolerance)) {
            throw std::invalid_argument("text overflow reference");
        }
        // The node image_margin px in from the edges of a transparent frame of its size and margins: an explicit
        // layer's content, drawn by a nested pass into the layer's image, and what the reference raster draws of text
        // with color glyphs. Text without color glyphs is FreeType coverage.
        const auto framed = [&] {
            SceneFrame frame;
            frame.root.tag = "div";
            frame.root.identity = {UINT32_MAX, 1, n.identity.view_epoch};
            frame.root.box = {0, 0, n.box[2] + limits::image_margins, n.box[3] + limits::image_margins};
            frame.root.style = {{"background-color", "transparent"}};
            auto child = n;
            child.box[0] = child.box[1] = limits::image_margin;
            frame.root.children.push_back(std::move(child));
            return frame;
        };
        auto result = std::make_shared<Image>();
        result->key = n;
        result->scale = scale;
        if (item.kind == Kind::Layer) {
            const auto frame = framed();
            const auto w = unsigned(std::ceil(frame.root.box[2] * scale));
            const auto h = unsigned(std::ceil(frame.root.box[3] * scale));
            result->bytes = std::size_t(w) * h * constants::rgba8_bytes_per_pixel;
            if (w > unsigned(constants::render::d3d11::max_surface_dimension) ||
                h > unsigned(constants::render::d3d11::max_surface_dimension) ||
                result->bytes > limits::image_cache_budget_bytes) {
                throw std::invalid_argument("layer image byte bound");
            }
            const auto listed = scene_display_list(frame, {}, &text_);
            if (!listed.fallback.empty()) {
                throw std::invalid_argument(listed.fallback);
            }
            if (const auto valid = paint::check(listed.list, limits::max_group_depth); !valid) {
                throw std::invalid_argument(valid.error().message);
            }
            const auto prepared = prepare(listed.list, scale, w, h, std::nullopt, view_index);
            // The pass's buffers and targets serve this pass alone: D3D11 keeps them until the GPU is done with them.
            Slot pass;
            shape(pass, *prepared, true);
            pass.target = group_target(gpu_.device.Get(), w, h);
            draw_pass(pass, *prepared, pass.target.rtv.Get(), scale);
            result->view = pass.target.srv;
            ++layer_builds;
        } else {
            SceneBitmap bitmap;
            auto text = scene_text_image(text_, n, scale, limits::image_margin);
            if (!text.color_glyphs) {
                bitmap = std::move(text.bitmap);
                ++text_portable_builds;
            } else {
                bitmap = reference_.raster(framed(), scale);
                ++reference_builds;
            }
            result->bytes = bitmap.rgba.size();
            if (result->bytes > limits::image_cache_budget_bytes) {
                throw std::invalid_argument("text source byte bound");
            }
            // Straight RGBA to premultiplied BGRA.
            for (std::size_t i = 0; i < bitmap.rgba.size(); i += 4) {
                std::swap(bitmap.rgba[i], bitmap.rgba[i + 2]);
                for (unsigned c = 0; c < 3; ++c) {
                    bitmap.rgba[i + c] = std::uint8_t(
                        (unsigned(bitmap.rgba[i + c]) * bitmap.rgba[i + 3] + constants::color_channel_max / 2) /
                        constants::color_channel_max);
                }
            }
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = bitmap.width;
            desc.Height = bitmap.height;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data{bitmap.rgba.data(), bitmap.width * 4, 0};
            ComPtr<ID3D11Texture2D> texture;
            throw_if_failed(gpu_.device->CreateTexture2D(&desc, &data, &texture), "native text source");
            throw_if_failed(gpu_.device->CreateShaderResourceView(texture.Get(), nullptr, &result->view),
                            "native text view");
            ++text_builds;
        }
        const auto bytes = result->bytes;
        if (images_.size() >= limits::image_cache_max_entries ||
            image_bytes_ + bytes > limits::image_cache_budget_bytes) {
            images_.clear();
            image_bytes_ = 0;
            ++image_evictions;
        }
        it = images_.find(id);
        if (it != images_.end()) {
            image_bytes_ -= it->second->bytes;
            images_.erase(it);
        }
        image_bytes_ += bytes;
        images_.emplace(id, result);
        return result;
    }

    std::shared_ptr<const SceneRenderer::Image> SceneRenderer::shadow(const paint::Item& item, const paint::Shadow& s,
                                                                      float scale) {
        const std::array<float, 15> key{s.x,        s.y,        s.blur,     s.spread,    s.color[0],
                                        s.color[1], s.color[2], s.color[3], s.radii[0],  s.radii[1],
                                        s.radii[2], s.radii[3], item.width, item.height, scale};
        if (auto it = shadows_.find(key); it != shadows_.end()) {
            ++shadow_reuses;
            return it->second;
        }
        auto image = shadow_image(s, item.width, item.height, scale);
        if (!image) {
            throw std::invalid_argument(image.error().message);
        }
        // Premultiplied RGBA to the texture's BGRA.
        auto& pixels = image->rgba;
        for (std::size_t i = 0; i < pixels.size(); i += 4) {
            std::swap(pixels[i], pixels[i + 2]);
        }
        const auto bytes = pixels.size();
        if (bytes > limits::image_cache_budget_bytes) {
            throw std::invalid_argument("shadow image byte bound");
        }
        auto result = std::make_shared<Image>();
        result->scale = scale;
        result->bytes = bytes;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = image->geometry.width;
        desc.Height = image->geometry.height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{pixels.data(), image->geometry.width * 4, 0};
        ComPtr<ID3D11Texture2D> texture;
        throw_if_failed(gpu_.device->CreateTexture2D(&desc, &data, &texture), "native shadow image");
        throw_if_failed(gpu_.device->CreateShaderResourceView(texture.Get(), nullptr, &result->view),
                        "native shadow view");
        ++shadow_builds;
        if (shadows_.size() >= limits::image_cache_max_entries ||
            shadow_bytes_ + bytes > limits::image_cache_budget_bytes) {
            shadows_.clear();
            shadow_bytes_ = 0;
            ++image_evictions;
        }
        shadow_bytes_ += bytes;
        shadows_.emplace(key, result);
        return result;
    }

    std::shared_ptr<const SceneRenderer::Image> SceneRenderer::path(const paint::Item& item, const paint::PathPaint& p,
                                                                    float scale,
                                                                    const std::array<std::int32_t, 4>& mask) {
        paint::Affine device = item.transform;
        for (auto& v : device) {
            v *= scale;
        }
        // The key: every value the coverage depends on, as bytes.
        std::string key;
        const auto add = [&](const void* data, std::size_t size) {
            key.append(static_cast<const char*>(data), size);
        };
        const auto& path = *p.path;
        const auto verbs = path.verbs.size();
        add(&verbs, sizeof verbs);
        add(path.verbs.data(), path.verbs.size() * sizeof(path.verbs[0]));
        add(path.points.data(), path.points.size() * sizeof(path.points[0]));
        add(&path.evenodd, sizeof path.evenodd);
        add(device.data(), sizeof device);
        add(mask.data(), sizeof mask);
        const bool stroked = p.stroke.has_value();
        add(&stroked, sizeof stroked);
        if (stroked) {
            const auto& s = *p.stroke;
            add(&s.width, sizeof s.width);
            add(&s.cap, sizeof s.cap);
            add(&s.join, sizeof s.join);
            add(&s.miter_limit, sizeof s.miter_limit);
            add(&s.dash_offset, sizeof s.dash_offset);
            add(s.dashes.data(), s.dashes.size() * sizeof(s.dashes[0]));
        }
        if (auto it = paths_.find(key); it != paths_.end()) {
            ++path_reuses;
            return it->second;
        }
        auto coverage = vector_.rasterize(p, device, mask);
        if (!coverage) {
            throw std::invalid_argument(coverage.error().message);
        }
        // White coverage, premultiplied BGRA: the record tints it.
        std::vector<std::uint8_t> pixels(coverage->coverage.size() * 4);
        for (std::size_t i = 0; i < coverage->coverage.size(); ++i) {
            std::fill_n(pixels.data() + i * 4, 4, coverage->coverage[i]);
        }
        const auto bytes = pixels.size();
        if (bytes > limits::image_cache_budget_bytes) {
            throw std::invalid_argument("path image byte bound");
        }
        auto result = std::make_shared<Image>();
        result->scale = scale;
        result->bytes = bytes;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = coverage->width;
        desc.Height = coverage->height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{pixels.data(), coverage->width * 4, 0};
        ComPtr<ID3D11Texture2D> texture;
        throw_if_failed(gpu_.device->CreateTexture2D(&desc, &data, &texture), "native path image");
        throw_if_failed(gpu_.device->CreateShaderResourceView(texture.Get(), nullptr, &result->view),
                        "native path view");
        ++path_builds;
        if (paths_.size() >= limits::image_cache_max_entries ||
            path_bytes_ + bytes > limits::image_cache_budget_bytes) {
            paths_.clear();
            path_bytes_ = 0;
            ++image_evictions;
        }
        path_bytes_ += bytes;
        paths_.emplace(std::move(key), result);
        return result;
    }

    std::unique_ptr<SceneRenderer::Prepared> SceneRenderer::prepare(const paint::DisplayList& list, float scale,
                                                                    unsigned width, unsigned height,
                                                                    std::optional<std::array<float, 4>> crop,
                                                                    unsigned view_index) {
        // Scene slots hold no asset textures.
        if (std::any_of(list.items.begin(), list.items.end(), [](const paint::Item& item) {
                return item.kind == Kind::Image;
            })) {
            throw std::invalid_argument("scene renderer draws no image items");
        }
        auto planned = plan_scene_pass(list, scale, width, height, crop);
        if (!planned) {
            throw std::invalid_argument(planned.error().message);
        }
        auto out = std::make_unique<Prepared>();
        out->pass = std::move(*planned);
        out->width = width;
        out->height = height;
        out->images.resize(list.items.size());
        out->planes.resize(list.items.size());
        for (std::size_t i = 0; i < list.items.size(); ++i) {
            const auto& item = list.items[i];
            if (item.kind == Kind::Text || item.kind == Kind::Layer) {
                out->images[i] = image(item, list.rasters[item.payload], scale, view_index);
            } else if (item.kind == Kind::Shadow) {
                out->images[i] = shadow(item, list.shadows[item.payload], scale);
            } else if (item.kind == Kind::Path) {
                const auto& mask = out->pass.masks[i];
                if (mask[2] > mask[0] && mask[3] > mask[1]) {
                    out->images[i] = path(item, list.paths[item.payload], scale, mask);
                }
            } else if (item.kind == Kind::Plane) {
                // The content flat in its own image, image_margin px in, texel per device pixel.
                const auto& plane = list.planes[item.payload];
                const auto pixels = plane_pixels(plane, scale);
                if (pixels[0] > unsigned(constants::render::d3d11::max_surface_dimension) ||
                    pixels[1] > unsigned(constants::render::d3d11::max_surface_dimension)) {
                    throw std::invalid_argument("plane image bound");
                }
                paint::DisplayList content;
                const float margin = limits::image_margin;
                if (const auto shifted = paint::append(content, *plane.content, {1, 0, 0, 1, margin, margin});
                    !shifted) {
                    throw std::invalid_argument(shifted.error().message);
                }
                out->planes[i] = prepare(content, scale, pixels[0], pixels[1], std::nullopt, view_index);
            }
        }
        return out;
    }

    void SceneRenderer::draw_pass(Slot& slot, const Prepared& prepared, ID3D11RenderTargetView* destination,
                                  float scale) {
        // Plane contents first, each into its slot's image, which this pass then samples.
        std::vector<ID3D11ShaderResourceView*> planes(prepared.planes.size());
        std::size_t next{};
        for (std::size_t i = 0; i < prepared.planes.size(); ++i) {
            if (prepared.planes[i]) {
                auto& nested = *slot.planes.at(next++);
                if (!nested.target.texture) {
                    nested.target = group_target(gpu_.device.Get(), nested.width, nested.height);
                }
                draw_pass(nested, *prepared.planes[i], nested.target.rtv.Get(), scale);
                planes[i] = nested.target.srv.Get();
                ++plane_passes;
            }
        }
        const auto& pass = prepared.pass;
        const auto& records = pass.records;
        const auto& images = prepared.images;
        const auto target_width = prepared.width;
        const auto target_height = prepared.height;
        slot.leases = images;
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
        auto instances = [&](unsigned start, unsigned count, ID3D11ShaderResourceView* image) {
            view[3] = float(start);
            ctx->UpdateSubresource(slot.constants.Get(), 0, nullptr, view.data(), 0, 0);
            ctx->PSSetShaderResources(1, 1, &image);
            ctx->DrawInstanced(constants::render::d3d11::quad_vertex_count, count, 0, 0);
            ++draws;
            ID3D11ShaderResourceView* empty = nullptr;
            ctx->PSSetShaderResources(1, 1, &empty);
        };
        for (const auto& batch : pass.batches) {
            switch (batch.step) {
                case Step::BeginGroup: {
                    auto& s = slot.groups.at(depth++);
                    if (!s.texture) {
                        s = group_target(gpu_.device.Get(), target_width, target_height);
                    }
                    ctx->ClearRenderTargetView(s.rtv.Get(), clear.data());
                    auto* rtv = s.rtv.Get();
                    ctx->OMSetRenderTargets(1, &rtv, nullptr);
                    stack.push_back(rtv);
                    break;
                }
                case Step::EndGroup: {
                    auto& s = slot.groups.at(--depth);
                    stack.pop_back();
                    auto* rtv = stack.back();
                    ctx->OMSetRenderTargets(1, &rtv, nullptr);
                    instances(batch.first, 1, s.srv.Get());
                    break;
                }
                case Step::Image:
                    // A path with nothing inside its clip has no image and draws nothing.
                    if (images[batch.first]) {
                        instances(batch.first, 1, images[batch.first]->view.Get());
                    } else if (planes[batch.first]) {
                        instances(batch.first, 1, planes[batch.first]);
                    }
                    break;
                case Step::Boxes:
                    instances(batch.first, batch.count, nullptr);
                    break;
            }
        }
        ctx->ClearState();
    }

    bool SceneRenderer::paint(Slot& slot, const SceneFrame& frame, ID3D11RenderTargetView* destination, float scale,
                              unsigned target_width, unsigned target_height, std::optional<std::array<float, 4>> crop,
                              unsigned view_index, const std::vector<NodeIdentity>& layers) {
        auto compiled = scene_display_list(frame, layers, &text_);
        if (!compiled.fallback.empty()) {
            ++fallbacks;
            ++reasons[compiled.fallback];
            return false;
        }
        const auto& list = compiled.list;
        if (const auto valid = paint::check(list, limits::max_group_depth); !valid) {
            ++fallbacks;
            ++reasons[valid.error().message];
            return false;
        }
        std::unique_ptr<Prepared> prepared;
        try {
            prepared = prepare(list, scale, target_width, target_height, crop, view_index);
        } catch (const std::invalid_argument& e) {
            ++fallbacks;
            ++reasons[e.what()];
            return false;
        }
        // The caller has confirmed this slot's prior real submission complete. Immutable image leases survive cache
        // eviction and the entire next submission; buffers and targets are never rewritten in flight.
        const auto held = held_bytes(slot);
        if (scratch_bytes_ - held + scratch_of(*prepared, false) > limits::scratch_budget_bytes) {
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
