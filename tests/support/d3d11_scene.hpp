#pragma once

#include "render/backends/d3d11/scene_renderer.hpp"
#include "support/check.hpp"
#include "support/scene_fonts.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// The native D3D11 scene pass on WARP over a transparent width x height target, read back as encoded premultiplied
// BGRA rows; bounds are its preparer's (SceneCacheBounds). The pass keeps the D2D reference raster for color glyph
// images, which needs COM on the thread.
namespace weft::testing {
    struct D3D11Scene {
        unsigned width{};
        unsigned height{};
        render::d3d11::Renderer gpu{true, width, height};
        render::d3d11::SceneRenderer scene;
        render::d3d11::SceneRenderer::Slot slot;

        D3D11Scene(unsigned w, unsigned h, SceneCacheBounds bounds = {})
            : width(w), height(h), scene(gpu, thread_scene_text(), bounds) {
        }

        std::vector<std::uint8_t> draw(const SceneFrame& frame, float scale = 1,
                                       const SceneAnchorTable* anchors = nullptr) {
            using Microsoft::WRL::ComPtr;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            ComPtr<ID3D11Texture2D> target;
            ComPtr<ID3D11RenderTargetView> rtv;
            require(SUCCEEDED(gpu.device->CreateTexture2D(&desc, nullptr, &target)) &&
                        SUCCEEDED(gpu.device->CreateRenderTargetView(target.Get(), nullptr, &rtv)),
                    "D3D11 scene target");
            if (!scene.paint(slot, frame, rtv.Get(), scale, width, height, std::nullopt, 0, {}, anchors)) {
                std::string why;
                for (const auto& [reason, count] : scene.reasons) {
                    why += " " + reason;
                }
                require(false, "native D3D11 pass:" + why);
            }
            desc.BindFlags = 0;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            require(SUCCEEDED(gpu.device->CreateTexture2D(&desc, nullptr, &staging)), "D3D11 readback");
            gpu.immediate->CopyResource(staging.Get(), target.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            require(SUCCEEDED(gpu.immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "D3D11 map");
            std::vector<std::uint8_t> out(std::size_t(width) * height * 4);
            for (unsigned y = 0; y < height; ++y) {
                std::memcpy(out.data() + std::size_t(y) * width * 4,
                            static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(y) * mapped.RowPitch,
                            width * 4);
            }
            gpu.immediate->Unmap(staging.Get(), 0);
            return out;
        }
    };
} // namespace weft::testing
