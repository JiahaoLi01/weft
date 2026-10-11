#include "render/backends/d3d11/host.hpp"
#include "render/quad_lowering.hpp"
#include "render/srgb.hpp"
#include "support/check.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// The D3D11 reference renderer as a library, without the probes: two frames through the synchronous host on
// WARP (--hardware: the default adapter), pixel readback, texture retirement after the actual completion query, the
// GPU conversion of translucent sRGB inputs against the CPU reference bit for bit, a display list drawn by the
// shared scene shader against its quads, and a clean debug layer.
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
        std::vector<Pixel> pixels(std::size_t(renderer.width) * renderer.height);
        for (unsigned y = 0; y < renderer.height; ++y) {
            std::memcpy(pixels.data() + std::size_t(y) * renderer.width,
                        static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(y) * mapped.RowPitch,
                        renderer.width * sizeof(Pixel));
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

    namespace paint = weft::paint;

    // A page-like display list: an opaque backdrop, a translucent box, a tinted coverage mask, a translucent image
    // over part of its asset, signed distances with an outline, and a box and an image cut by a rounded clip.
    paint::DisplayList page_list() {
        paint::DisplayList list;
        auto box = [&](paint::Color color, float x, float y, float w, float h) {
            paint::Item item;
            item.kind = paint::Item::Kind::Box;
            item.payload = std::uint32_t(list.boxes.size());
            item.transform = {1, 0, 0, 1, x, y};
            item.width = w;
            item.height = h;
            item.clip = {0, 0, 96, 64};
            list.boxes.emplace_back().background = color;
            list.items.push_back(item);
            return list.items.size() - 1;
        };
        auto image = [&](paint::Image payload, float x, float y, float w, float h) {
            paint::Item item;
            item.kind = paint::Item::Kind::Image;
            item.payload = std::uint32_t(list.images.size());
            item.transform = {1, 0, 0, 1, x, y};
            item.width = w;
            item.height = h;
            item.clip = {0, 0, 96, 64};
            list.images.push_back(payload);
            list.items.push_back(item);
            return list.items.size() - 1;
        };
        std::vector<std::uint8_t> mask(64);
        for (unsigned i = 0; i < mask.size(); ++i) {
            mask[i] = std::uint8_t(i * 4);
        }
        list.assets.push_back(std::make_shared<const Asset>(Asset{8, 8, Encoding::Coverage, mask}));
        list.assets.push_back(conversion_asset());
        std::vector<std::uint8_t> distance(16 * 16);
        for (unsigned y = 0; y < 16; ++y) {
            for (unsigned x = 0; x < 16; ++x) {
                const float d = 5 - std::hypot(x + .5f - 8, y + .5f - 8);
                distance[y * 16 + x] = std::uint8_t(std::clamp(128 + d * 128 / 6, 0.f, 255.f));
            }
        }
        list.assets.push_back(std::make_shared<const Asset>(Asset{16, 16, Encoding::SignedDistance, distance, 6}));
        box({.1f, .12f, .15f, 1}, 0, 0, 96, 64);
        box({1, 1, 1, .5f}, 4, 4, 20, 12);
        paint::Image tinted;
        tinted.asset = 0;
        tinted.color = {.9f, .7f, .2f, 1};
        image(tinted, 28, 4, 16, 16);
        paint::Image picture;
        picture.asset = 1;
        picture.uv = {.25f, .25f, .5f, .5f};
        image(picture, 48, 4, 24, 24);
        paint::Image sdf;
        sdf.asset = 2;
        sdf.color = {1, 1, 1, 1};
        sdf.stroke = 2;
        sdf.outline = {.2f, .4f, 1, 1};
        image(sdf, 76, 4, 16, 16);
        // A rounded clip that cuts both of the next items.
        list.clips.push_back({{8, 30, 40, 30}, {10, 10, 10, 10}, {8, 8, 8, 8}});
        const auto cut_box = box({.8f, .2f, .3f, .9f}, 4, 28, 48, 34);
        paint::Image cut_picture = picture;
        cut_picture.uv = {0, 0, 1, 1};
        const auto cut_image = image(cut_picture, 30, 34, 30, 30);
        for (auto i : {cut_box, cut_image}) {
            list.items[i].clips_count = 1;
        }
        return list;
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

        // The same display list as a linear pass of the shared scene shader and as quads with CPU rounded masks.
        Renderer listed(!hardware, 96, 64);
        weft::render::d3d11::render_list(listed, page_list(), [] {
        });
        CHECK(listed.acknowledged && listed.native_releases == 3);
        const auto drawn = read_output(listed);
        Renderer quads(!hardware, 96, 64);
        weft::render::d3d11::render_window(quads, weft::render::lower_to_quads(page_list()), [] {
        });
        const auto reference = read_output(quads);
        std::size_t edges{};
        int worst{};
        double total{};
        for (std::size_t i = 0; i < drawn.size(); ++i) {
            int difference{};
            for (unsigned c = 0; c < 4; ++c) {
                difference = std::max(difference, std::abs(int(drawn[i][c]) - int(reference[i][c])));
                total += std::abs(int(drawn[i][c]) - int(reference[i][c]));
            }
            worst = std::max(worst, difference);
            edges += difference > 2;
        }
        const auto mean = total / (drawn.size() * 4);
        std::cout << "display list vs quads: " << edges << " pixels over 2 levels, worst " << worst << ", mean " << mean
                  << '\n';
        // Away from rounded clip edges the two paths sample and blend alike; along them the GPU's analytic coverage
        // differs from the quads' 16x16 supersampled CSS px masks by a few levels.
        CHECK(edges <= 32 && worst <= 16 && mean < .1);
        CHECK(drawn[std::size_t(10) * 96 + 10] == reference[std::size_t(10) * 96 + 10]);
        listed.check_debug_messages();
        quads.check_debug_messages();
        std::cout << "PASS D3D11 renderer library on " << renderer.adapter << ": 2 frames, " << renderer.native_releases
                  << " textures retired after completion; " << exact
                  << " GPU-converted sRGB texels equal the CPU reference bit for bit; a display list within " << worst
                  << " levels of its quads (" << edges << " clip-edge pixels over 2); debug layer "
                  << (renderer.debug_layer ? "clean" : "unavailable") << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
