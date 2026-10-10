#include "render/backends/d3d11/scene_renderer.hpp"

#include "constants/common.hpp"
#include "scene/scene_paint.hpp"
#include "scene/scene_text_image.hpp"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace weft::render::d3d11 {
    namespace {
        namespace limits = constants::render::d3d11::scene;
        using Paint = NativePaint;
        using Color = std::array<float, 4>;

        // One Paint of scene.hlsl.
        struct Record {
            Color bounds;
            Color inv_x;
            Color inv_y;
            Color size;
            Color clip;
            Color radii;
            Color widths;
            Color background;
            std::array<Color, 4> edges;
            Color tile;
            Color extra;
            std::array<Color, limits::max_gradient_stops> stops;
            std::array<Color, limits::gradient_offset_vectors> offsets;
        };

        static_assert(sizeof(Record) == limits::record_bytes);

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

        Record record(const Paint& p, float scale, unsigned w, unsigned h) {
            auto m = p.matrix;
            Record r{};
            r.size = {p.width, p.height, float(p.gradient), float(p.stops.size())};
            r.clip = p.clip;
            r.radii = p.radii;
            r.widths = p.widths;
            r.background = p.background;
            r.edges = p.edges;
            r.tile = {p.tile_width, p.tile_height, p.offset_x, p.offset_y};
            r.extra = {p.angle, p.opacity,
                       (p.kind == Paint::Text || p.kind == Paint::Layer) ? limits::flag_enabled : limits::flag_disabled,
                       p.text_mask ? limits::flag_enabled : limits::flag_disabled};
            for (unsigned i = 0; i < p.stops.size(); ++i) {
                r.stops[i] = p.stops[i].color;
                r.offsets[i / limits::stops_per_offset_vector][i % limits::stops_per_offset_vector] = p.stops[i].offset;
            }
            if (p.kind == Paint::EndGroup) {
                m = {1, 0, 0, 1, 0, 0};
                r.size = {float(w) / scale, float(h) / scale, 0, 0};
                r.extra = {0, p.opacity, limits::flag_enabled, limits::flag_disabled};
            } else if (p.kind == Paint::Text || p.kind == Paint::Layer) {
                m[4] -= limits::image_margin * (m[0] + m[2]);
                m[5] -= limits::image_margin * (m[1] + m[3]);
                r.size[0] += limits::image_margins;
                r.size[1] += limits::image_margins;
            }
            float det = m[0] * m[3] - m[1] * m[2];
            r.inv_x = {m[3] / det, -m[2] / det, (m[2] * m[5] - m[3] * m[4]) / det, 0};
            r.inv_y = {-m[1] / det, m[0] / det, (m[1] * m[4] - m[0] * m[5]) / det, 0};
            float left = limits::record_bounds_sentinel;
            float top = limits::record_bounds_sentinel;
            float right = -limits::record_bounds_sentinel;
            float bottom = -limits::record_bounds_sentinel;
            for (auto point : std::array<std::array<float, 2>, 4>{
                     {{0, 0}, {r.size[0], 0}, {0, r.size[1]}, {r.size[0], r.size[1]}}}) {
                float x = m[0] * point[0] + m[2] * point[1] + m[4];
                float y = m[1] * point[0] + m[3] * point[1] + m[5];
                left = std::min(left, x);
                top = std::min(top, y);
                right = std::max(right, x);
                bottom = std::max(bottom, y);
            }
            r.bounds = {left - limits::record_bounds_outset / scale, top - limits::record_bounds_outset / scale,
                        right - left + limits::record_bounds_outsets / scale,
                        bottom - top + limits::record_bounds_outsets / scale};
            return r;
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
        ensure(slot.group_bytes <= scratch_bytes_, "native scratch retirement accounting");
        scratch_bytes_ -= slot.group_bytes;
        slot = Slot{};
    }

    std::shared_ptr<const SceneRenderer::Image> SceneRenderer::image(const Paint& p, float scale, unsigned view_index) {
        const auto& n = *p.text;
        std::array<std::uint64_t, 4> id{view_index, n.identity.view_epoch, n.identity.slot, n.identity.generation};
        auto it = images_.find(id);
        if (it != images_.end() && scene_paint_source_equal(it->second->key, n) && it->second->scale == scale) {
            if (p.kind == Paint::Layer) {
                ++layer_reuses;
            } else {
                ++text_reuses;
            }
            return it->second;
        }
        if (p.kind == Paint::Layer) {
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
        if (p.kind == Paint::Text && (!overflow || overflow->text() != "hidden") &&
            (metrics.width > n.box[2] + limits::text_fit_tolerance ||
             metrics.height > n.box[3] + limits::text_fit_tolerance)) {
            throw std::invalid_argument("text overflow reference");
        }
        // Text without color glyphs is FreeType coverage (portable); color glyphs and layers use the reference
        // raster.
        SceneBitmap bitmap;
        bool portable = false;
        if (p.kind == Paint::Text) {
            auto text = scene_text_image(text_, n, scale, limits::image_margin);
            if (!text.color_glyphs) {
                bitmap = std::move(text.bitmap);
                portable = true;
                ++text_portable_builds;
            }
        }
        if (!portable) {
            SceneFrame frame;
            frame.root.tag = "div";
            frame.root.identity = {UINT32_MAX, 1, n.identity.view_epoch};
            frame.root.box = {0, 0, n.box[2] + limits::image_margins, n.box[3] + limits::image_margins};
            frame.root.style = {{"background-color", "transparent"}};
            auto child = n;
            child.box[0] = child.box[1] = limits::image_margin;
            frame.root.children.push_back(std::move(child));
            bitmap = reference_.raster(frame, scale);
        }
        const auto bytes = bitmap.rgba.size();
        if (bytes > limits::image_cache_budget_bytes) {
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
        auto result = std::make_shared<Image>();
        result->key = n;
        result->scale = scale;
        result->bytes = bytes;
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
        if (p.kind == Paint::Layer) {
            ++layer_builds;
        } else {
            ++text_builds;
        }
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

    bool SceneRenderer::paint(Slot& slot, const SceneFrame& frame, ID3D11RenderTargetView* destination, float scale,
                              unsigned target_width, unsigned target_height, std::optional<std::array<float, 4>> crop,
                              unsigned view_index, const std::vector<NodeIdentity>& layers) {
        auto plan = compile_native_scene(frame, layers);
        if (!plan.fallback.empty()) {
            ++fallbacks;
            ++reasons[plan.fallback];
            return false;
        }
        std::vector<Record> records;
        std::vector<std::shared_ptr<const Image>> images(plan.paints.size());
        try {
            for (std::size_t i = 0; i < plan.paints.size(); ++i) {
                auto& p = plan.paints[i];
                if (crop) {
                    p.clip = {std::max(p.clip[0], (*crop)[0]), std::max(p.clip[1], (*crop)[1]),
                              std::min(p.clip[2], (*crop)[2]), std::min(p.clip[3], (*crop)[3])};
                }
                records.push_back(record(p, scale, target_width, target_height));
                if (p.kind == Paint::Text || p.kind == Paint::Layer) {
                    images[i] = image(p, scale, view_index);
                }
            }
        } catch (const std::invalid_argument& e) {
            ++fallbacks;
            ++reasons[e.what()];
            return false;
        }
        // The caller has confirmed this slot's prior real submission complete. Immutable image leases survive cache
        // eviction and the entire next submission; buffers and group targets are never rewritten in flight.
        unsigned depth_needed{};
        unsigned depth_now{};
        for (const auto& p : plan.paints) {
            if (p.kind == Paint::BeginGroup) {
                depth_needed = std::max(depth_needed, ++depth_now);
            }
            if (p.kind == Paint::EndGroup) {
                --depth_now;
            }
        }
        const auto desired_bytes = std::size_t(target_width) * target_height * 4 * depth_needed;
        if (scratch_bytes_ - slot.group_bytes + desired_bytes > limits::scratch_budget_bytes) {
            ++fallbacks;
            ++reasons["native scratch budget reference"];
            return false;
        }
        if (slot.width != target_width || slot.height != target_height || slot.group_bytes != desired_bytes) {
            scratch_bytes_ -= slot.group_bytes;
            slot.groups = {};
            slot.width = target_width;
            slot.height = target_height;
            slot.group_bytes = desired_bytes;
            scratch_bytes_ += desired_bytes;
        }
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
        auto* ctx = gpu_.immediate.Get();
        ctx->ClearState();
        if (!records.empty()) {
            D3D11_BOX box{0, 0, 0, UINT(records.size() * sizeof(Record)), 1, 1};
            ctx->UpdateSubresource(slot.records.Get(), 0, &box, records.data(), 0, 0);
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
        Color initial{};
        unsigned first{};
        if (!plan.paints.empty()) {
            // An opaque full-target first rectangle is the clear color.
            const auto& p = plan.paints.front();
            if (p.kind == Paint::Shape && p.matrix == std::array<float, 6>{1, 0, 0, 1, 0, 0} &&
                p.width * scale == target_width && p.height * scale == target_height && p.clip[0] <= 0 &&
                p.clip[1] <= 0 && p.clip[2] >= p.width && p.clip[3] >= p.height && p.opacity == 1 &&
                p.background[3] == 1 && !p.gradient &&
                std::all_of(p.radii.begin(), p.radii.end(),
                            [](float v) {
                                return v == 0;
                            }) &&
                std::all_of(p.widths.begin(), p.widths.end(), [](float v) {
                    return v == 0;
                })) {
                initial = p.background;
                first = 1;
            }
        }
        ctx->ClearRenderTargetView(destination, initial.data());
        ctx->OMSetRenderTargets(1, &destination, nullptr);
        unsigned depth{};
        std::vector<ID3D11RenderTargetView*> stack{destination};
        auto draw = [&](unsigned start, unsigned count, ID3D11ShaderResourceView* image) {
            view[3] = float(start);
            ctx->UpdateSubresource(slot.constants.Get(), 0, nullptr, view.data(), 0, 0);
            ctx->PSSetShaderResources(1, 1, &image);
            ctx->DrawInstanced(constants::render::d3d11::quad_vertex_count, count, 0, 0);
            ++draws;
            ID3D11ShaderResourceView* empty = nullptr;
            ctx->PSSetShaderResources(1, 1, &empty);
        };
        for (unsigned i = first; i < plan.paints.size();) {
            const auto& p = plan.paints[i];
            if (p.kind == Paint::BeginGroup) {
                auto& s = slot.groups.at(depth++);
                if (!s.texture) {
                    s = group_target(gpu_.device.Get(), target_width, target_height);
                }
                ctx->ClearRenderTargetView(s.rtv.Get(), clear.data());
                auto* rtv = s.rtv.Get();
                ctx->OMSetRenderTargets(1, &rtv, nullptr);
                stack.push_back(rtv);
                ++i;
            } else if (p.kind == Paint::EndGroup) {
                auto& s = slot.groups.at(--depth);
                stack.pop_back();
                auto* rtv = stack.back();
                ctx->OMSetRenderTargets(1, &rtv, nullptr);
                draw(i++, 1, s.srv.Get());
            } else if (p.kind == Paint::Text || p.kind == Paint::Layer) {
                draw(i, 1, images[i]->view.Get());
                ++i;
            } else {
                auto start = i++;
                while (i < plan.paints.size() && plan.paints[i].kind == Paint::Shape) {
                    ++i;
                }
                draw(start, i - start, nullptr);
            }
        }
        ctx->ClearState();
        ++passes;
        return true;
    }
} // namespace weft::render::d3d11
