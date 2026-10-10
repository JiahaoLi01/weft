#include "render/backends/d3d11/host.hpp"
#include "render/srgb.hpp"
#include "support/check.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// The D3D11 reference renderer as a library, without the probes: two frames through the synchronous host on
// WARP (--hardware: the default adapter), pixel readback, texture retirement after the actual completion query, the
// GPU conversion of translucent sRGB inputs against the CPU reference bit for bit, and a clean debug layer.
namespace {
#define CHECK(v) WEFT_REQUIRE(v)

    using weft::render::Asset;
    using weft::render::Encoding;
    using weft::render::FramePacket;
    using weft::render::d3d11::Renderer;
    using Pixel = std::array<std::uint8_t, 4>;

    constexpr unsigned width = 64;
    constexpr unsigned height = 32;

    // Opaque black, a half-transparent white quad on the left and a coverage-masked white quad on the right.
    FramePacket packet() {
        FramePacket p;
        p.assets.push_back(std::make_shared<const Asset>(Asset{1, 1, Encoding::Coverage, {128}}));
        p.draws.push_back({{0, 0, float(width), float(height)}, {0, 0, 0, 1}});
        p.draws.push_back({{0, 0, 32, 32}, {1, 1, 1, 0.5f}});
        p.draws.push_back({{32, 0, 32, 32}, {1, 1, 1, 1}, 0});
        return p;
    }

    std::vector<Pixel> read_output(Renderer& renderer) {
        renderer.immediate->CopyResource(renderer.staging.Get(), renderer.output.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        weft::render::d3d11::throw_if_failed(
            renderer.immediate->Map(renderer.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "map readback");
        std::vector<Pixel> pixels(std::size_t(width) * height);
        for (unsigned y = 0; y < height; ++y) {
            std::memcpy(pixels.data() + std::size_t(y) * width,
                        static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(y) * mapped.RowPitch,
                        width * sizeof(Pixel));
        }
        renderer.immediate->Unmap(renderer.staging.Get(), 0);
        return pixels;
    }

    // Every channel value at every alpha: R = x, G = 255 - x, B = 7x mod 256, A = y.
    constexpr unsigned conversion_size = 256;

    std::shared_ptr<const Asset> conversion_asset() {
        std::vector<std::uint8_t> bytes(std::size_t(conversion_size) * conversion_size * 4);
        for (unsigned y = 0; y < conversion_size; ++y) {
            for (unsigned x = 0; x < conversion_size; ++x) {
                auto* p = bytes.data() + (std::size_t(y) * conversion_size + x) * 4;
                p[0] = std::uint8_t(x);
                p[1] = std::uint8_t(255 - x);
                p[2] = std::uint8_t(x * 7);
                p[3] = std::uint8_t(y);
            }
        }
        return std::make_shared<const Asset>(
            Asset{conversion_size, conversion_size, Encoding::SrgbStraight, std::move(bytes)});
    }

    // The CPU conversion the renderer used before srgb_convert.hlsl: decode table times byte / 255.
    std::array<float, 4> reference_texel(const std::uint8_t* p) {
        const float alpha = p[3] / 255.0f;
        std::array<float, 4> out{};
        for (unsigned c = 0; c < 3; ++c) {
            out[c] = weft::render::srgb_to_linear(p[c] / 255.0f) * alpha;
        }
        out[3] = alpha;
        return out;
    }

    bool matches(const Pixel& actual, const Pixel& expected) {
        for (std::size_t c = 0; c < 4; ++c) {
            if (std::abs(int(actual[c]) - int(expected[c])) > (c == 3 ? 0 : 2)) {
                return false;
            }
        }
        return true;
    }
} // namespace

int main(int argc, char** argv) {
    try {
        const bool hardware = argc > 1 && std::string(argv[1]) == "--hardware";
        Renderer renderer(!hardware, width, height);
        unsigned presents{};
        for (int frame = 0; frame < 2; ++frame) {
            weft::render::d3d11::render_window(renderer, packet(), [&] {
                // Submitted but not yet complete: the frame's texture is still pinned.
                CHECK(!renderer.acknowledged && renderer.native_releases == std::size_t(frame));
                ++presents;
            });
            CHECK(renderer.acknowledged && renderer.window_serial == std::uint64_t(2 * (frame + 1)));
            // The texture retires once the actual completion query has signaled.
            CHECK(renderer.native_releases == std::size_t(frame + 1));
            const auto pixels = read_output(renderer);
            // 50% white over black and 128/255 coverage are both linear 0.5: sRGB 188 (the color fixture's value).
            CHECK(matches(pixels[std::size_t(16) * width + 16], {188, 188, 188, 255}));
            CHECK(matches(pixels[std::size_t(16) * width + 48], {188, 188, 188, 255}));
            CHECK(renderer.tracker.Stats().completed_serial == renderer.window_serial);
        }
        CHECK(presents == 2 && renderer.uploads == 2);

        // A translucent sRGB input is converted on the GPU; copy its float texels while the frame still pins them.
        const auto source = conversion_asset();
        auto conversion = packet();
        conversion.assets.push_back(source);
        conversion.draws.push_back({{0, 0, 16, 16}, {1, 1, 1, 1}, 1});
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = conversion_size;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        weft::render::d3d11::ComPtr<ID3D11Texture2D> texels;
        weft::render::d3d11::throw_if_failed(renderer.device->CreateTexture2D(&desc, nullptr, &texels),
                                             "conversion staging");
        const auto conversions = renderer.gpu_conversions;
        weft::render::d3d11::render_window(renderer, conversion, [&] {
            bool copied{};
            for (const auto& a : renderer.assets) {
                if (a.rtv) {
                    renderer.immediate->CopyResource(texels.Get(), a.texture.Get());
                    copied = true;
                }
            }
            CHECK(copied);
        });
        CHECK(renderer.gpu_conversions == conversions + 1);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        weft::render::d3d11::throw_if_failed(renderer.immediate->Map(texels.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                                             "map conversion");
        std::size_t exact{};
        for (unsigned y = 0; y < conversion_size; ++y) {
            const auto* row = reinterpret_cast<const float*>(static_cast<const std::uint8_t*>(mapped.pData) +
                                                             std::size_t(y) * mapped.RowPitch);
            for (unsigned x = 0; x < conversion_size; ++x) {
                const auto expected =
                    reference_texel(source->bytes.data() + (std::size_t(y) * conversion_size + x) * 4);
                exact += std::memcmp(row + std::size_t(x) * 4, expected.data(), sizeof(expected)) == 0;
            }
        }
        renderer.immediate->Unmap(texels.Get(), 0);
        CHECK(exact == std::size_t(conversion_size) * conversion_size);
        renderer.check_debug_messages();
        std::cout << "PASS D3D11 renderer library on " << renderer.adapter << ": 2 frames, " << renderer.native_releases
                  << " textures retired after completion; " << exact
                  << " GPU-converted sRGB texels equal the CPU reference bit for bit; debug layer "
                  << (renderer.debug_layer ? "clean" : "unavailable") << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
