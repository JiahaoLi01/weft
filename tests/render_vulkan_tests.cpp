#include "render/backends/vulkan/renderer.hpp"
#include "support/check.hpp"
#include "support/render_lists.hpp"

#ifdef _WIN32
#include "render/backends/d3d11/host.hpp"
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

// The Vulkan renderer on this machine's Vulkan device (--adapter <name> picks one): the display lists of the D3D11
// test against their CPU expectations and, on Windows, against the D3D11 renderer on WARP, with no validation layer
// warning or error. Exit 77 skips without a Vulkan loader or device.
namespace {
#define CHECK(v) WEFT_REQUIRE(v)

    using namespace weft::testing;
    namespace vk = weft::render::vulkan;

    // Exit code that makes CTest report the test as skipped.
    constexpr int skipped = 77;

    struct Difference {
        int worst{};
        std::size_t over{};
        double mean{};
        // Bounds of the pixels over the levels: left, top, right, bottom (inclusive).
        std::array<std::size_t, 4> box{SIZE_MAX, SIZE_MAX, 0, 0};
    };

    // The largest channel difference, the pixels whose difference exceeds levels, and the mean channel difference.
    Difference difference(const std::vector<Pixel>& a, const std::vector<Pixel>& b, int levels, std::size_t width) {
        Difference d;
        double total{};
        for (std::size_t i = 0; i < a.size(); ++i) {
            int worst{};
            for (unsigned c = 0; c < 4; ++c) {
                const int e = std::abs(int(a[i][c]) - int(b[i][c]));
                worst = std::max(worst, e);
                total += e;
            }
            d.worst = std::max(d.worst, worst);
            if (worst > levels) {
                ++d.over;
                const auto x = i % width;
                const auto y = i / width;
                d.box = {std::min(d.box[0], x), std::min(d.box[1], y), std::max(d.box[2], x), std::max(d.box[3], y)};
            }
        }
        d.mean = total / (a.size() * 4);
        return d;
    }

#ifdef _WIN32
    // The same list drawn by the D3D11 renderer on WARP.
    std::vector<Pixel> d3d11(const paint::DisplayList& list, unsigned w, unsigned h) {
        weft::render::d3d11::Renderer r(true, w, h);
        weft::render::d3d11::render_list(r, list, [] {
        });
        r.immediate->CopyResource(r.staging.Get(), r.output.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        weft::render::d3d11::throw_if_failed(r.immediate->Map(r.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                                             "map D3D11 readback");
        std::vector<Pixel> pixels(std::size_t(w) * h);
        for (unsigned y = 0; y < h; ++y) {
            std::memcpy(pixels.data() + std::size_t(y) * w,
                        static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(y) * mapped.RowPitch,
                        w * sizeof(Pixel));
        }
        r.immediate->Unmap(r.staging.Get(), 0);
        return pixels;
    }

    void report(const char* what, const Difference& d) {
        std::cout << what << " vs D3D11 WARP: worst " << d.worst << ", " << d.over << " pixels over 2 levels";
        if (d.over) {
            std::cout << " in x " << d.box[0] << ".." << d.box[2] << " y " << d.box[1] << ".." << d.box[3];
        }
        std::cout << ", mean " << d.mean << '\n';
    }
#endif
} // namespace

int main(int argc, char** argv) {
    try {
        vk::DeviceOptions options;
        for (int i = 1; i + 1 < argc; ++i) {
            if (std::string(argv[i]) == "--adapter") {
                options.adapter = argv[i + 1];
            }
        }
        std::string unavailable;
        const auto device = vk::Device::open(options, unavailable);
        if (!device) {
            std::cout << "SKIP Vulkan: " << unavailable << '\n';
            return skipped;
        }
        std::cout << "Vulkan device: " << device->name << ", API " << VK_API_VERSION_MAJOR(device->api_version) << '.'
                  << VK_API_VERSION_MINOR(device->api_version) << '.' << VK_API_VERSION_PATCH(device->api_version)
                  << ", validation layer " << (device->validation ? "on" : "not installed") << '\n';

        // The page list: its parts against their CPU expectations, as the D3D11 test checks them.
        vk::Renderer page(*device, 96, 64);
        const auto drawn = page.draw(page_list());
        auto at = [&](unsigned x, unsigned y) {
            return drawn[std::size_t(y) * 96 + x];
        };
        const auto base = linear({.1f, .12f, .15f, 1});
        CHECK(close_to(at(80, 50), encode(base), 1));
        CHECK(close_to(at(10, 10), encode(over(linear({1, 1, 1, .5f}), base)), 1));
        CHECK(close_to(at(39, 17), encode(over(scaled(linear({.9f, .7f, .2f, 1}), 4 * 55.25f / 255), base)), 1));
        CHECK(close_to(at(60, 16), encode(over(picture_sample(60.5f, 16.5f), base)), 2));
        const auto fill = at(84, 12);
        const auto band = at(90, 11);
        CHECK(fill[0] >= 250 && fill[1] >= 250 && fill[2] >= 250);
        CHECK(band[2] > band[0] + 60 && band[2] > 200);
        CHECK(close_to(at(8, 30), encode(base), 1));
        CHECK(close_to(at(20, 45), encode(over(linear({.8f, .2f, .3f, .9f}), base)), 1));

        // Isolated groups, and a second frame whose cleared layer shows no red.
        vk::Renderer grouped(*device, 64, 32);
        const auto black = linear({0, 0, 0, 1});
        std::vector<Pixel> grouped_first;
        for (int frame = 0; frame < 2; ++frame) {
            const auto pixels = grouped.draw(groups(frame == 0));
            CHECK(close_to(pixels[std::size_t(10) * 64 + 10],
                           encode(frame == 0 ? over(scaled(linear({1, 0, 0, 1}), .5f), black) : black), 1));
            CHECK(
                close_to(pixels[std::size_t(10) * 64 + 20], encode(over(scaled(linear({0, 0, 1, 1}), .5f), black)), 1));
            CHECK(close_to(pixels[std::size_t(10) * 64 + 45], encode(over(scaled(linear({1, 1, 1, 1}), .25f), black)),
                           1));
            CHECK(close_to(pixels[std::size_t(28) * 64 + 60], encode(black), 0));
            if (!frame) {
                grouped_first = pixels;
            }
        }

        vk::Renderer run(*device, 96, 64);
        const auto glyphs = run.draw(glyph_run());
#ifdef _WIN32
        const auto page_difference = difference(drawn, d3d11(page_list(), 96, 64), 2, 96);
        report("page list", page_difference);
        const auto group_difference = difference(grouped_first, d3d11(groups(true), 64, 32), 2, 64);
        report("groups", group_difference);
        const auto glyph_difference = difference(glyphs, d3d11(glyph_run(), 96, 64), 2, 96);
        report("glyph run", glyph_difference);
        CHECK(!page_difference.over && !group_difference.over && !glyph_difference.over);
#endif
        if (device->validation_messages) {
            std::cerr << device->validation_messages
                      << " validation messages; first: " << device->first_validation_message << '\n';
        }
        CHECK(device->validation_messages == 0);
        std::cout << "PASS Vulkan renderer on " << device->name << ": " << page.draws + grouped.draws + run.draws
                  << " draws; the page list matches its CPU expectations; isolated groups; validation "
                  << (device->validation ? "clean" : "not installed") << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
