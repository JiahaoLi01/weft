#include "effects/crpg_smoke.hpp"
#include "render/srgb.hpp"
#include "support/check.hpp"
#include "support/crpg_smoke.hpp"
#include "support/d3d11_scene.hpp"
#include "support/png.hpp"
#ifdef WEFT_TEST_VULKAN
#include "render/backends/vulkan/scene_renderer.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>

// The CRPG UI's smoke background as a UI shader effect (examples/crpg-gallery/effects/smoke.hlsl) against the
// original's tick on the CPU (tests/support/crpg_smoke.hpp): over a box of the canvas's size, one canvas pixel per
// device pixel, the alpha of nearly every pixel within 2 of the original's byte, the rarity color premultiplied by it;
// over larger boxes, the canvas stretched bilinearly as the page scales it.
// Several times, a clock started after the frame's, both fade axes, turned texture axes, another canvas size and no
// noise; the native D3D11 pass on WARP and the Vulkan pass on this machine's device (skipped without one). The float
// arithmetic of a shader may move a texture coordinate across a texel boundary where JavaScript's doubles do not: at
// most 1% of the pixels may differ more.
namespace {
    using weft::testing::require;
    using Pixels = std::vector<std::uint8_t>;

    struct Case {
        const char* name;
        weft::testing::SmokeParameters parameters;
        std::array<std::uint8_t, 3> color;
        double time;
        // The effect time at which the original's clock starts (its mount).
        double start{};
        // The box the canvas is stretched over, in device pixels; the canvas's own size when zero.
        unsigned width{};
        unsigned height{};

        unsigned box_width() const {
            return width ? width : parameters.width;
        }

        unsigned box_height() const {
            return height ? height : parameters.height;
        }
    };

    // The canvas stretched over a w x h box as a page scales it: bilinear between the canvas pixels around each device
    // pixel's center, clamped at the edges, in the precision of the alpha bytes.
    std::vector<std::uint8_t> stretch(const std::vector<std::uint8_t>& canvas, unsigned cw, unsigned ch, unsigned w,
                                      unsigned h) {
        std::vector<std::uint8_t> out(std::size_t(w) * h);
        const auto at = [&](int x, int y) {
            return double(canvas[std::size_t(std::clamp(y, 0, int(ch) - 1)) * cw + std::clamp(x, 0, int(cw) - 1)]);
        };
        for (unsigned y = 0; y < h; ++y) {
            const double v = (y + .5) * ch / h - .5;
            const double y0 = std::floor(v);
            for (unsigned x = 0; x < w; ++x) {
                const double u = (x + .5) * cw / w - .5;
                const double x0 = std::floor(u);
                const double fx = u - x0;
                const double fy = v - y0;
                const auto a = [&](int dx, int dy) {
                    return at(int(x0) + dx, int(y0) + dy);
                };
                const double top = a(0, 0) + (a(1, 0) - a(0, 0)) * fx;
                const double bottom = a(0, 1) + (a(1, 1) - a(0, 1)) * fx;
                out[std::size_t(y) * w + x] = std::uint8_t(std::lround(top + (bottom - top) * fy));
            }
        }
        return out;
    }

    std::vector<std::uint8_t> expected_alpha(const Case& c, const weft::paint::Asset& cloud,
                                             const weft::paint::Asset& noise) {
        const auto& p = c.parameters;
        const auto canvas = weft::testing::smoke_canvas(cloud, noise, p, c.time - c.start);
        return c.width || c.height ? stretch(canvas, p.width, p.height, c.box_width(), c.box_height()) : canvas;
    }

    std::shared_ptr<const weft::paint::Asset> red_channel(const std::filesystem::path& path) {
        const auto image = weft::testing::read_png(path);
        std::vector<std::uint8_t> red(std::size_t(image.width) * image.height);
        for (std::size_t i = 0; i < red.size(); ++i) {
            red[i] = image.rgba[i * 4];
        }
        return std::make_shared<const weft::paint::Asset>(
            weft::paint::Asset{image.width, image.height, weft::paint::Encoding::Coverage, std::move(red)});
    }

    weft::SceneFrame smoke_frame(const Case& c, const std::shared_ptr<const weft::paint::Asset>& cloud,
                                 const std::shared_ptr<const weft::paint::Asset>& noise) {
        const auto& p = c.parameters;
        weft::SceneFrame f;
        f.time_ms = c.time * 1000;
        f.root.identity = {1, 1, 1};
        f.root.tag = "div";
        f.root.box = {0, 0, float(c.box_width()), float(c.box_height())};
        const auto light = [&](unsigned i) {
            return weft::render::srgb_to_linear(c.color[i] / 255.f);
        };
        f.root.effect = std::make_shared<const weft::SceneEffect>(weft::SceneEffect{
            weft::effects::crpg_smoke(),
            {cloud, noise},
            {{float(p.cloud_scale), float(p.noise_scale), float(p.gamma), float(p.noise_strength)},
             {float(p.cloud_speed_x), float(p.cloud_speed_y), float(p.noise_speed_x), float(p.noise_speed_y)},
             {float(p.fade_power), p.fade_right ? 1.f : 0.f, p.rotate90 ? 1.f : 0.f, 0},
             {float(p.width), float(p.height), 0, 0},
             {light(0), light(1), light(2), 1},
             {float(c.start), 0, 0, 0}}});
        return f;
    }

    // Compares a pass's pixels (encoded premultiplied BGRA) with the original's canvas: the share of pixels whose
    // alpha differs by more than 2, the largest difference, and whether every pixel's color is the rarity color
    // premultiplied by its alpha.
    void compare(const char* pass, const Case& c, const Pixels& pixels, const std::vector<std::uint8_t>& expected) {
        std::size_t off{};
        int worst{};
        bool colors = true;
        for (std::size_t i = 0; i < expected.size(); ++i) {
            const auto* p = pixels.data() + i * 4;
            const int d = std::abs(int(p[3]) - int(expected[i]));
            worst = std::max(worst, d);
            off += d > 2;
            for (unsigned k = 0; k < 3; ++k) {
                const int want = int(std::lround(c.color[2 - k] * p[3] / 255.0));
                colors &= std::abs(int(p[k]) - want) <= 2;
            }
        }
        std::cout << pass << ' ' << c.name << ": " << off << " of " << expected.size()
                  << " pixels over 2 levels, worst " << worst << '\n';
        require(off * 100 <= expected.size() && colors, std::string(pass) + " smoke " + c.name);
    }
} // namespace

int main() {
    try {
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM init");
        const auto assets = std::filesystem::path(WEFT_SOURCE_DIR) / "tests/fixtures/crpg-gallery-assets/UI/public";
        const auto cloud = red_channel(assets / "cloud.png");
        const auto noise = red_channel(assets / "tiling-noise.png");
        std::vector<Case> cases;
        weft::testing::SmokeParameters defaults;
        cases.push_back({"start", defaults, {96, 165, 250}, 0});
        cases.push_back({"25 ticks", defaults, {192, 132, 252}, 1.65});
        cases.push_back({"later", defaults, {245, 158, 11}, 12.3});
        cases.push_back({"mounted at 2 s", defaults, {192, 132, 252}, 3.65, 2});
        weft::testing::SmokeParameters chip = defaults;
        chip.fade_right = true;
        chip.rotate90 = true;
        chip.width = 96;
        chip.height = 24;
        chip.cloud_speed_x = -3;
        chip.noise_speed_y = 4;
        cases.push_back({"chip", chip, {212, 184, 130}, 3.3});
        weft::testing::SmokeParameters plain = defaults;
        plain.noise_strength = 0;
        plain.gamma = .7;
        plain.fade_power = 1;
        cases.push_back({"no noise", plain, {156, 163, 175}, 5});
        // Stretched as on the page: three times, and by ratios that put device pixel centers between canvas pixels.
        cases.push_back({"stretched 3x", defaults, {96, 165, 250}, 1.65, 0, 144, 192});
        cases.push_back({"stretched chip", chip, {212, 184, 130}, 3.3, 0, 230, 37});

        for (const auto& c : cases) {
            const auto expected = expected_alpha(c, *cloud, *noise);
            weft::testing::D3D11Scene d3d11(c.box_width(), c.box_height());
            compare("D3D11 WARP", c, d3d11.draw(smoke_frame(c, cloud, noise)), expected);
            d3d11.gpu.check_debug_messages();
        }
        std::string vulkan = "skipped";
#ifdef WEFT_TEST_VULKAN
        namespace vk = weft::render::vulkan;
        std::string unavailable;
        if (const auto device = vk::Device::open({}, unavailable)) {
            vk::SceneRenderer scene(*device, weft::testing::thread_scene_text());
            for (const auto& c : cases) {
                const auto expected = expected_alpha(c, *cloud, *noise);
                auto pixels =
                    scene.draw(smoke_frame(c, cloud, noise), 1, c.box_width(), c.box_height(), std::nullopt, 0, {});
                require(pixels.has_value(), "native Vulkan pass");
                compare("Vulkan", c, *pixels, expected);
            }
            require(device->validation_messages == 0, "Vulkan validation");
            vulkan = device->name;
        } else {
            std::cout << "Vulkan skipped: " << unavailable << '\n';
        }
#endif
        std::cout << "PASS CRPG smoke effect against the original's tick: D3D11 WARP, Vulkan " << vulkan
                  << "; times, a later start, fade axes, turned axes, canvas sizes, noise off, stretched\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
