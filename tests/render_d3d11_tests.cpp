#include "render/backends/d3d11/host.hpp"
#include "support/check.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

// The D3D11 reference renderer as a library, without the probes: two frames through the synchronous host on
// WARP, pixel readback, texture retirement after the actual completion query, and a clean debug layer.
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

    bool matches(const Pixel& actual, const Pixel& expected) {
        for (std::size_t c = 0; c < 4; ++c) {
            if (std::abs(int(actual[c]) - int(expected[c])) > (c == 3 ? 0 : 2)) {
                return false;
            }
        }
        return true;
    }
} // namespace

int main() {
    try {
        Renderer renderer(true, width, height);
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
        renderer.check_debug_messages();
        std::cout << "PASS D3D11 renderer library on WARP: 2 frames, " << renderer.native_releases
                  << " textures retired after completion; debug layer "
                  << (renderer.debug_layer ? "clean" : "unavailable") << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
