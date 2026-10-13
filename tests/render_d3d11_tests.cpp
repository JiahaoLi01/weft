#include "render/backends/d3d11/host.hpp"
#include "render/paint_list.hpp"
#include "render/srgb.hpp"
#include "support/check.hpp"
#include "support/effect_lists.hpp"
#include "support/render_lists.hpp"

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
// GPU conversion of translucent sRGB inputs against the CPU reference bit for bit, a display list drawn directly and
// as a C ABI list frame against CPU expectations, isolated groups, UI shader effects against CPU expectations and
// against the boxes they reproduce, and a clean debug layer.
namespace {
#define CHECK(v) WEFT_REQUIRE(v)

    using weft::render::Asset;
    using weft::render::Encoding;
    using weft::render::FramePacket;
    using weft::render::d3d11::Renderer;
    using namespace weft::testing;

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

        // A page-like list drawn directly by the shared scene shader, against CPU expectations of its parts.
        Renderer listed(!hardware, 96, 64);
        weft::render::d3d11::render_list(listed, page_list(), [] {
        });
        CHECK(listed.acknowledged && listed.native_releases == 3);
        const auto drawn = read_output(listed);
        auto at = [&](unsigned x, unsigned y) {
            return drawn[std::size_t(y) * 96 + x];
        };
        const auto base = linear({.1f, .12f, .15f, 1});
        CHECK(close_to(at(80, 50), encode(base), 1));
        CHECK(close_to(at(10, 10), encode(over(linear({1, 1, 1, .5f}), base)), 1));
        // The tinted mask's coverage is linear in its texels, so filtering between them is exact: 4 (8 v + u) / 255
        // at texel coordinates (5.25, 6.25).
        CHECK(close_to(at(39, 17), encode(over(scaled(linear({.9f, .7f, .2f, 1}), 4 * 55.25f / 255), base)), 1));
        CHECK(close_to(at(60, 16), encode(over(picture_sample(60.5f, 16.5f), base)), 2));
        // Inside the distance contour the fill, in the outline band the outline color.
        const auto fill = at(84, 12);
        const auto band = at(90, 11);
        CHECK(fill[0] >= 250 && fill[1] >= 250 && fill[2] >= 250);
        CHECK(band[2] > band[0] + 60 && band[2] > 200);
        // The rounded clip removes the cut box from its corner and keeps it inside.
        CHECK(close_to(at(8, 30), encode(base), 1));
        CHECK(close_to(at(20, 45), encode(over(linear({.8f, .2f, .3f, .9f}), base)), 1));
        listed.check_debug_messages();

        // The same list as a C ABI list frame, through the backend that also takes quad frames.
        Renderer framed(!hardware, 96, 64);
        FramePacket list_packet;
        list_packet.list = std::make_shared<const weft::paint::DisplayList>(page_list());
        weft::render::d3d11::render_window(framed, list_packet, [] {
        });
        CHECK(read_output(framed) == drawn && framed.native_releases == 3);
        // Effects come to the C ABI with a feature bit of their own: a list frame with one is refused, untouched.
        {
            const weft_paint_list effect_list{std::make_shared<const weft::paint::DisplayList>(uv_list())};
            const weft_render_list_frame input{sizeof(input), WEFT_RENDER_ABI_VERSION, &effect_list, 0, 0, nullptr};
            weft_render_handle refused{};
            const auto result = weft_renderer_prepare_list(framed.ensure_backend().renderer, &input, &refused);
            // A frame prepared by mistake goes back before the check fails, so the renderer tears down cleanly.
            if (refused.renderer_id) {
                weft_renderer_release_prepared(framed.ensure_backend().renderer, refused);
            }
            CHECK(result == WEFT_RENDER_UNSUPPORTED_FEATURE && !refused.renderer_id);
        }
        // So do particles.
        {
            weft::paint::DisplayList list;
            weft::paint::Particles p;
            p.emission.kind = weft::paint::ParticleEmission::Kind::Burst;
            p.emission.count = 4;
            list.particles.push_back(p);
            weft::paint::Item item;
            item.kind = weft::paint::Item::Kind::Particles;
            item.width = 10;
            item.height = 10;
            item.clip = {0, 0, 96, 64};
            list.items.push_back(item);
            const weft_paint_list particle_list{std::make_shared<const weft::paint::DisplayList>(std::move(list))};
            const weft_render_list_frame input{sizeof(input), WEFT_RENDER_ABI_VERSION, &particle_list, 0, 0, nullptr};
            weft_render_handle refused{};
            const auto result = weft_renderer_prepare_list(framed.ensure_backend().renderer, &input, &refused);
            if (refused.renderer_id) {
                weft_renderer_release_prepared(framed.ensure_backend().renderer, refused);
            }
            CHECK(result == WEFT_RENDER_UNSUPPORTED_FEATURE && !refused.renderer_id);
        }
        framed.check_debug_messages();

        // Isolated groups composite once at their opacity: red under blue at half opacity shows half blue where they
        // overlap, and nested halves are a quarter. The second frame has no red box: its group layer starts cleared.
        Renderer grouped(!hardware, 64, 32);
        const auto black = linear({0, 0, 0, 1});
        for (int frame = 0; frame < 2; ++frame) {
            weft::render::d3d11::render_list(grouped, groups(frame == 0), [] {
            });
            const auto pixels = read_output(grouped);
            CHECK(close_to(pixels[std::size_t(10) * 64 + 10],
                           encode(frame == 0 ? over(scaled(linear({1, 0, 0, 1}), .5f), black) : black), 1));
            CHECK(
                close_to(pixels[std::size_t(10) * 64 + 20], encode(over(scaled(linear({0, 0, 1, 1}), .5f), black)), 1));
            CHECK(close_to(pixels[std::size_t(10) * 64 + 45], encode(over(scaled(linear({1, 1, 1, 1}), .25f), black)),
                           1));
            CHECK(close_to(pixels[std::size_t(28) * 64 + 60], encode(black), 0));
        }
        grouped.check_debug_messages();

        // UI shader effects (tests/effects): where in its box an effect runs; the record field of each parameter and
        // the time, sixteen effects in one draw; textures read as data, as color and repeated, apart from the atlas
        // region the cache holds for the same asset as an image; a box drawn after effects; and an effect drawn as the
        // box it reproduces, rotated, with corners, under a rounded clip in a group.
        {
            Renderer effects(!hardware, effect_width, effect_height);
            effects.cache_assets = true;
            const auto draw = [&](weft::paint::DisplayList list) {
                weft::render::d3d11::render_list(effects, std::move(list), [] {
                });
                return read_output(effects);
            };
            auto draws = effects.display_draws;
            CHECK(effect_matches("uv effect", draw(uv_list()), uv_expected, 1));
            // A frame of one effect: its draw and the frame's own.
            const auto single = effects.display_draws - draws;
            draws = effects.display_draws;
            CHECK(effect_matches("parameters effect", draw(parameters_list()), parameters_expected, 1));
            CHECK(effects.display_draws - draws == single);
            draw(atlas_list());
            CHECK(effects.atlas_placements == 1);
            CHECK(effect_matches("textures effect", draw(textures_list()), textures_expected, 1));
            CHECK(effects.atlas_placements == 1);
            const auto composed = effect_difference(draw(composed_list(true)), draw(composed_list(false)));
            std::cout << "effect against its box: worst " << composed[0] << " over " << composed[1] << " pixels\n";
            CHECK(composed[0] <= 1 && composed[1] > 400);
            effects.check_debug_messages();
        }

        // A caching renderer places the glyph images on an atlas page and draws the run in a few draws; the pixels are
        // those of one texture per image, because each placed image repeats its edge texels around it.
        Renderer single(!hardware, 96, 64);
        weft::render::d3d11::render_list(single, glyph_run(), [] {
        });
        const auto separate = read_output(single);
        Renderer cached(!hardware, 96, 64);
        cached.cache_assets = true;
        FramePacket run;
        run.list = std::make_shared<const weft::paint::DisplayList>(glyph_run());
        for (int frame = 0; frame < 2; ++frame) {
            const auto draws = cached.display_draws;
            weft::render::d3d11::render_window(cached, run, [] {
            });
            const auto atlas_pixels = read_output(cached);
            int worst{};
            std::size_t differing{};
            for (std::size_t i = 0; i < atlas_pixels.size(); ++i) {
                int d{};
                for (unsigned c = 0; c < 4; ++c) {
                    d = std::max(d, std::abs(int(atlas_pixels[i][c]) - int(separate[i][c])));
                }
                worst = std::max(worst, d);
                differing += d > 0;
            }
            std::cout << "atlas vs separate textures: " << differing << " pixels differ, worst " << worst << "; draws "
                      << cached.display_draws - draws << '\n';
            // The atlas coordinates of a scaled image round a little differently, and WARP quantizes its filter weights,
            // so a filtered texel may land one level apart; hardware draws them alike.
            CHECK(worst <= 1);
            CHECK(cached.display_draws - draws <= 4);
        }
        CHECK(cached.atlas_placements == 48 && cached.atlas.pages() == 1 && cached.atlas.live() == 48 &&
              cached.cache.hits >= 48 && single.display_draws >= 48);
        // Draining the cache gives every region back.
        cached.evict_assets(0, true);
        CHECK(cached.atlas.live() == 0);
        cached.check_debug_messages();
        std::cout << "driver command lists: " << renderer.driver_command_lists << '\n';
        std::cout
            << "PASS D3D11 renderer library on " << renderer.adapter << ": 2 frames, " << renderer.native_releases
            << " textures retired after completion; " << exact
            << " GPU-converted sRGB texels equal the CPU reference bit for bit; a display list drawn directly "
               "and as a C ABI list frame matches its CPU expectations; isolated groups; UI shader effects; debug layer "
            << (renderer.debug_layer ? "clean" : "unavailable") << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
