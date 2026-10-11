#pragma once

#include "probe_common.hpp"
#include "probe_constants.hpp"

namespace probes {
    // GPU producer -> external lease -> UI. Only final validation targets are read
    // back; producer pixels never cross a CPU readback/upload boundary.

    struct ExternalRetirement {
        unsigned count{};
        unsigned reason{};

        static void notify(void* p, uint32_t reason) noexcept {
            auto& s = *static_cast<ExternalRetirement*>(p);
            ++s.count;
            s.reason = reason;
        }
    };

    inline ComPtr<ID3D11Buffer> external_constants(Experiment& gpu) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = weft::constants::render::scene::view_constants_bytes;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> result;
        check_hresult(gpu.device->CreateBuffer(&d, nullptr, &result), "external independent constants");
        return result;
    }

    inline ComPtr<ID3D11Texture2D> external_readback(Experiment& gpu, DXGI_FORMAT format) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = gpu.width;
        d.Height = gpu.height;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = format;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> result;
        check_hresult(gpu.device->CreateTexture2D(&d, nullptr, &result), "external final target staging");
        return result;
    }

    inline std::array<Color, 4> external_colors(unsigned variation,
                                                uint32_t format = WEFT_RENDER_TARGET_LINEAR_RGBA32F) {
        auto colors = variation % 2 ? std::array<Color, 4>{Color{0, .5f, .5f, .5f}, Color{.25f, 0, .25f, .25f},
                                                           Color{1, 1, 0, 1}, Color{0, 0, 0, 0}}
                                    : std::array<Color, 4>{Color{.5f, 0, 0, .5f}, Color{0, .25f, 0, .25f},
                                                           Color{0, 0, 1, 1}, Color{0, 0, 0, 0}};
        if (format == WEFT_RENDER_TARGET_LINEAR_RGBA8) {
            for (auto& c : colors) {
                for (auto& component : c) {
                    component =
                        std::round(component * weft::constants::color_channel_max) / weft::constants::color_channel_max;
                }
            }
        }
        return colors;
    }

    inline void external_produce(Experiment& gpu, const Surface& target, unsigned variation,
                                 const ComPtr<ID3D11Buffer>& buffer) {
        struct Restore {
            Experiment& gpu;
            ComPtr<ID3D11Buffer> old;

            ~Restore() {
                gpu.display_constants = std::move(old);
            }
        } restore{gpu, gpu.display_constants};

        gpu.display_constants = buffer;
        auto* ctx = gpu.deferred.Get();
        ctx->ClearState();
        const float clear[4]{};
        ctx->ClearRenderTargetView(target.rtv.Get(), clear);
        FramePacket quadrants;
        const auto colors = external_colors(variation);
        for (unsigned q = 0; q < 4; ++q) {
            auto straight = colors[q];
            if (straight[3]) {
                for (unsigned c = 0; c < 3; ++c) {
                    straight[c] /= straight[3];
                }
            }
            quadrants.draws.push_back({{float(q % 2) * gpu.width / 2, float(q / 2) * gpu.height / 2,
                                        float(gpu.width) / 2, float(gpu.height) / 2},
                                       straight});
        }
        gpu.record(ctx, quad_list(quadrants, 0, gpu.width, gpu.height), target.rtv.Get(), gpu.width, gpu.height, {});
        ctx->ClearState();
        ComPtr<ID3D11CommandList> commands;
        check_hresult(ctx->FinishCommandList(FALSE, &commands), "external producer commands");
        gpu.immediate->ExecuteCommandList(commands.Get(), TRUE);
        // Same immediate queue orders producer first; no Flush/GetData/CPU wait.
    }

    inline weft_render_external_texture external_gpu_description(Backend& api, const Surface& source,
                                                                 weft_render_d3d11_texture& payload,
                                                                 ExternalRetirement& retired, uint64_t version,
                                                                 bool bottom = false,
                                                                 uint32_t format = WEFT_RENDER_TARGET_LINEAR_RGBA32F) {
        weft_render_info info{};
        info.struct_size = sizeof(info);
        render_result(weft_renderer_get_info(api.renderer, &info), "external identity");
        payload = {sizeof(payload), WEFT_RENDER_EXTERNAL_ABI_VERSION, source.srv.Get(), 0};
        return {sizeof(weft_render_external_texture),
                WEFT_RENDER_EXTERNAL_ABI_VERSION,
                info.identity,
                501,
                version,
                api.gpu.width,
                api.gpu.height,
                format,
                0,
                uint32_t(bottom),
                0,
                0,
                0,
                &payload,
                sizeof(payload),
                &retired,
                ExternalRetirement::notify};
    }

    inline Color external_sample(unsigned variation, bool bottom, uint32_t format, float u, float v, UINT w, UINT h) {
        if (bottom) {
            v = 1 - v;
        }
        const float x = u * w - .5f;
        const float y = v * h - .5f;
        const int ix = int(std::floor(x));
        const int iy = int(std::floor(y));
        const float fx = x - ix;
        const float fy = y - iy;
        const auto colors = external_colors(variation, format);
        Color result{};
        for (int yy = 0; yy < 2; ++yy) {
            for (int xx = 0; xx < 2; ++xx) {
                const int px = std::clamp(ix + xx, 0, int(w) - 1);
                const int py = std::clamp(iy + yy, 0, int(h) - 1);
                const auto& c = colors[(py >= int(h) / 2 ? 2 : 0) + (px >= int(w) / 2 ? 1 : 0)];
                const float weight = (xx ? fx : 1 - fx) * (yy ? fy : 1 - fy);
                for (unsigned channel = 0; channel < 4; ++channel) {
                    result[channel] += weight * c[channel];
                }
            }
        }
        return result;
    }

    inline Color external_pixel(const weft_render_frame& frame, unsigned variation, bool bottom, uint32_t format, int x,
                                int y, UINT w, UINT h) {
        Color dst{};
        for (uint32_t i = 0; i < frame.draw_count; ++i) {
            const auto& d = frame.draws[i];
            const float px = x + .5f;
            const float py = y + .5f;
            if (px < d.x || py < d.y || px >= d.x + d.width || py >= d.y + d.height) {
                continue;
            }
            Color source{};
            if (d.asset_index == WEFT_RENDER_SOLID_ASSET || d.asset_index < frame.asset_count) {
                source[3] = d.srgb[3];
                if (d.asset_index != WEFT_RENDER_SOLID_ASSET) {
                    source[3] *= frame.assets[d.asset_index].bytes[0] / weft::probe_constants::color_channel_max_float;
                }
                for (unsigned c = 0; c < 3; ++c) {
                    source[c] = decode(d.srgb[c]) * source[3];
                }
            } else {
                source = external_sample(variation, bottom, format, d.uv[0] + (px - d.x) / d.width * d.uv[2],
                                         d.uv[1] + (py - d.y) / d.height * d.uv[3], w, h);
            }
            for (auto& c : source) {
                c *= d.opacity;
            }
            for (unsigned c = 0; c < 4; ++c) {
                dst[c] = source[c] + dst[c] * (1 - source[3]);
            }
        }
        return dst;
    }
} // namespace probes
