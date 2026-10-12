#include "effects/effect_color.hpp"
#include "effects/effect_parameters.hpp"
#include "effects/effect_repeat.hpp"
#include "effects/effect_textures.hpp"
#include "render/backends/d3d11/scene_renderer.hpp"
#include "render/srgb.hpp"
#include "support/check.hpp"
#include "support/scene_fonts.hpp"
#ifdef WEFT_TEST_VULKAN
#include "render/backends/vulkan/scene_renderer.hpp"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>

// UI shader effects in the native scene passes, which composite encoded premultiplied colors: the D3D11 pass on WARP
// and the Vulkan pass on this machine's device (skipped without one). An effect against the box it reproduces under a
// rotation, corners, opacity and a rounded overflow clip in a group; textures read as data, as opaque sRGB and as
// translucent linear color, repeated; the frame's time; the two passes against each other.
namespace {
    using weft::testing::require;
    using Pixels = std::vector<std::uint8_t>;
    constexpr unsigned width = 64;
    constexpr unsigned height = 48;

    weft::SceneNode node(std::uint32_t slot, std::array<float, 4> box,
                         std::initializer_list<std::pair<const char*, const char*>> style) {
        weft::SceneNode n;
        n.identity = {1, 1, slot};
        n.tag = "div";
        n.box = box;
        for (const auto& [k, v] : style) {
            n.style[k] = v;
        }
        return n;
    }

    std::shared_ptr<const weft::SceneEffect> effect(const std::shared_ptr<const weft::paint::EffectProgram>& program,
                                                    std::vector<std::shared_ptr<const weft::paint::Asset>> textures,
                                                    std::vector<std::array<float, 4>> parameters) {
        return std::make_shared<const weft::SceneEffect>(
            weft::SceneEffect{program, std::move(textures), std::move(parameters)});
    }

    // In a group at half opacity under a rounded overflow clip, a rotated box with corners at .8 opacity: the color
    // effect, or the box with the same color as its background.
    weft::SceneFrame composed(bool with_effect) {
        weft::SceneFrame f;
        f.root = node(1, {0, 0, float(width), float(height)}, {});
        auto clip = node(2, {6, 4, 52, 40},
                         {{"overflow", "hidden"},
                          {"opacity", "0.5"},
                          {"border-top-left-radius", "10px"},
                          {"border-top-right-radius", "10px"},
                          {"border-bottom-right-radius", "10px"},
                          {"border-bottom-left-radius", "10px"}});
        auto card = node(3, {10, 4, 36, 26},
                         {{"transform", "rotate(20deg)"},
                          {"opacity", "0.8"},
                          {"border-top-left-radius", "6px"},
                          {"border-top-right-radius", "6px"},
                          {"border-bottom-right-radius", "6px"},
                          {"border-bottom-left-radius", "6px"}});
        if (with_effect) {
            card.effect = effect(weft::effects::effect_color(), {},
                                 {{weft::render::srgb_to_linear(.8f), weft::render::srgb_to_linear(.3f),
                                   weft::render::srgb_to_linear(.2f), .7f}});
        } else {
            card.style["background-color"] = "rgba(204, 77, 51, 0.7)";
        }
        clip.children = {card};
        f.root.children = {clip};
        return f;
    }

    std::uint8_t data_texel(unsigned x, unsigned y) {
        return std::uint8_t(x * 16 + y);
    }

    std::array<std::uint8_t, 4> color_texel(unsigned x, unsigned y, bool translucent) {
        return {std::uint8_t(x * 15), std::uint8_t(255 - y * 15), 128,
                std::uint8_t(translucent ? 96 + x * 9 + y : 255)};
    }

    std::shared_ptr<const weft::paint::Asset> data_asset() {
        std::vector<std::uint8_t> bytes(256);
        for (unsigned y = 0; y < 16; ++y) {
            for (unsigned x = 0; x < 16; ++x) {
                bytes[y * 16 + x] = data_texel(x, y);
            }
        }
        return std::make_shared<const weft::paint::Asset>(
            weft::paint::Asset{16, 16, weft::paint::Encoding::Coverage, std::move(bytes)});
    }

    std::shared_ptr<const weft::paint::Asset> color_asset(bool translucent) {
        std::vector<std::uint8_t> bytes(256 * 4);
        for (unsigned y = 0; y < 16; ++y) {
            for (unsigned x = 0; x < 16; ++x) {
                const auto c = color_texel(x, y, translucent);
                std::copy(c.begin(), c.end(), bytes.begin() + (y * 16 + x) * 4);
            }
        }
        return std::make_shared<const weft::paint::Asset>(
            weft::paint::Asset{16, 16, weft::paint::Encoding::SrgbStraight, std::move(bytes)});
    }

    // The textures effect over a 16 x 16 box at (8, 8), texel per px, and the repeat effect over a translucent color
    // texture under a 32 x 32 box at (28, 8); at time 2.5 s the parameters effect shows parameter 2 over (4, 42, 8, 4).
    weft::SceneFrame textured() {
        weft::SceneFrame f;
        f.time_ms = 2500;
        f.root = node(1, {0, 0, float(width), float(height)}, {});
        auto data = node(2, {8, 8, 16, 16}, {});
        data.effect = effect(weft::effects::effect_textures(), {data_asset(), color_asset(false)}, {});
        auto repeat = node(3, {28, 8, 32, 32}, {});
        repeat.effect = effect(weft::effects::effect_repeat(), {color_asset(true)}, {});
        auto timed = node(4, {4, 42, 8, 4}, {});
        std::vector<std::array<float, 4>> parameters;
        for (unsigned i = 0; i < 16; ++i) {
            parameters.push_back({i / 15.f, 1 - i / 15.f, float((i * 7) % 16) / 15, 1});
        }
        timed.effect = effect(weft::effects::effect_parameters(), {}, parameters);
        // A box after the effects, in the same pass: drawn as a box.
        auto after = node(5, {14, 42, 8, 4}, {{"background-color", "#0000ff"}});
        f.root.children = {data, repeat, timed, after};
        return f;
    }

    // Encoded premultiplied BGRA bytes of a linear premultiplied color, as an encoded pass stores it.
    std::array<int, 4> encoded(const std::array<float, 4>& linear) {
        std::array<int, 4> out{};
        const float a = linear[3];
        for (unsigned c = 0; c < 3; ++c) {
            const float straight = a > 0 ? std::clamp(linear[c] / a, 0.f, 1.f) : 0;
            out[2 - c] = int(std::lround(weft::render::linear_to_srgb(straight) * a * 255));
        }
        out[3] = int(std::lround(a * 255));
        return out;
    }

    // A pixel an encoded pass antialiases: within a pixel of a box's edge (expected_texture leaves it out, negative).
    bool edge(unsigned x, unsigned y, unsigned left, unsigned top, unsigned right, unsigned bottom) {
        const bool near_x = x + 1 >= left && x <= right;
        const bool near_y = y + 1 >= top && y <= bottom;
        return (near_x && near_y) && (x + 1 == left || x == left || x + 1 == right || x == right || y + 1 == top ||
                                      y == top || y + 1 == bottom || y == bottom);
    }

    std::array<int, 4> expected_texture(unsigned x, unsigned y) {
        const auto light = [](std::uint8_t byte) {
            return weft::render::srgb_to_linear(byte / 255.f);
        };
        if (edge(x, y, 8, 8, 24, 24) || edge(x, y, 28, 8, 60, 40) || edge(x, y, 4, 42, 12, 46) ||
            edge(x, y, 14, 42, 22, 46)) {
            return {-1, -1, -1, -1};
        }
        if (x >= 8 && x < 24 && y >= 8 && y < 24) {
            const float d = data_texel(x - 8, y - 8) / 255.f;
            const auto c = color_texel(x - 8, y - 8, false);
            return encoded({light(c[0]) * d, light(c[1]) * d, light(c[2]) * d, d});
        }
        if (x >= 28 && x < 60 && y >= 8 && y < 40) {
            // The mean of the two premultiplied linear texels beside the sample, the last's neighbor the first.
            std::array<float, 4> mean{};
            for (const auto u : {(x - 28) % 16, (x - 27) % 16}) {
                const auto c = color_texel(u, (y - 8) % 16, true);
                const float a = c[3] / 255.f;
                for (unsigned k = 0; k < 3; ++k) {
                    mean[k] += light(c[k]) * a / 2;
                }
                mean[3] += a / 2;
            }
            return encoded(mean);
        }
        if (x >= 4 && x < 12 && y >= 42 && y < 46) {
            return encoded({2 / 15.f, 13 / 15.f, 14 / 15.f, 1});
        }
        if (x >= 14 && x < 22 && y >= 42 && y < 46) {
            return {255, 0, 0, 255};
        }
        return {};
    }

    // The largest channel difference of two targets, and how many pixels the first covers.
    std::array<int, 2> difference(const Pixels& a, const Pixels& b) {
        int worst{};
        int covered{};
        for (std::size_t i = 0; i < a.size(); i += 4) {
            for (unsigned c = 0; c < 4; ++c) {
                worst = std::max(worst, std::abs(int(a[i + c]) - int(b[i + c])));
            }
            covered += a[i + 3] != 0;
        }
        return {worst, covered};
    }

    // Each pixel within levels of expected(x, y); reports the first that is not.
    template <class Expected>
    bool matches(const char* what, const Pixels& pixels, Expected&& expected, int levels) {
        for (unsigned y = 0; y < height; ++y) {
            for (unsigned x = 0; x < width; ++x) {
                const auto want = expected(x, y);
                if (want[3] < 0) {
                    continue;
                }
                const auto* p = pixels.data() + (std::size_t(y) * width + x) * 4;
                for (unsigned c = 0; c < 4; ++c) {
                    if (std::abs(int(p[c]) - want[c]) > levels) {
                        std::cerr << what << ": pixel (" << x << ", " << y << ") is " << int(p[0]) << ' ' << int(p[1])
                                  << ' ' << int(p[2]) << ' ' << int(p[3]) << ", expected " << want[0] << ' ' << want[1]
                                  << ' ' << want[2] << ' ' << want[3] << '\n';
                        return false;
                    }
                }
            }
        }
        return true;
    }

    // The native D3D11 pass on a transparent target, read back as encoded premultiplied BGRA rows.
    struct D3D11Scene {
        weft::render::d3d11::Renderer gpu{true, width, height};
        weft::render::d3d11::SceneRenderer scene{gpu, weft::testing::thread_scene_text()};
        weft::render::d3d11::SceneRenderer::Slot slot;

        Pixels draw(const weft::SceneFrame& frame) {
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
            require(scene.paint(slot, frame, rtv.Get(), 1, width, height, std::nullopt, 0, {}), "native D3D11 pass");
            desc.BindFlags = 0;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            require(SUCCEEDED(gpu.device->CreateTexture2D(&desc, nullptr, &staging)), "D3D11 readback");
            gpu.immediate->CopyResource(staging.Get(), target.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            require(SUCCEEDED(gpu.immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "D3D11 map");
            Pixels out(std::size_t(width) * height * 4);
            for (unsigned y = 0; y < height; ++y) {
                std::memcpy(out.data() + std::size_t(y) * width * 4,
                            static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(y) * mapped.RowPitch,
                            width * 4);
            }
            gpu.immediate->Unmap(staging.Get(), 0);
            return out;
        }
    };

    // The checks every pass passes; the pixels of each frame for the comparison of passes.
    std::array<Pixels, 2> check(const char* pass, const std::function<Pixels(const weft::SceneFrame&)>& draw) {
        const auto effect_drawn = draw(composed(true));
        const auto against_box = difference(effect_drawn, draw(composed(false)));
        std::cout << pass << " effect against its box: worst " << against_box[0] << " over " << against_box[1]
                  << " pixels\n";
        require(against_box[0] <= 1 && against_box[1] > 300, std::string(pass) + " effect against its box");
        const auto textures = draw(textured());
        require(matches(pass, textures, expected_texture, 2), std::string(pass) + " textures and time");
        return {effect_drawn, textures};
    }
} // namespace

int main() {
    try {
        // The D3D11 pass keeps the D2D reference raster for color glyph images, which needs COM.
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM init");
        D3D11Scene d3d11;
        const auto warp = check("D3D11 WARP", [&](const weft::SceneFrame& f) {
            return d3d11.draw(f);
        });
        d3d11.gpu.check_debug_messages();
        require(d3d11.scene.effect_texture_builds == 3, "each effect texture built once");
        std::string vulkan = "skipped";
#ifdef WEFT_TEST_VULKAN
        namespace vk = weft::render::vulkan;
        std::string unavailable;
        if (const auto device = vk::Device::open({}, unavailable)) {
            vk::SceneRenderer scene(*device, weft::testing::thread_scene_text());
            const auto drawn = check("Vulkan", [&](const weft::SceneFrame& f) {
                auto pixels = scene.draw(f, 1, width, height, std::nullopt, 0, {});
                require(pixels.has_value(), "native Vulkan pass");
                return std::move(*pixels);
            });
            for (unsigned i = 0; i < 2; ++i) {
                const auto d = difference(drawn[i], warp[i]);
                std::cout << "Vulkan vs D3D11 WARP, frame " << i << ": worst " << d[0] << '\n';
                require(d[0] <= 2, "Vulkan against D3D11 WARP");
            }
            require(device->validation_messages == 0, "Vulkan validation");
            vulkan = device->name;
        } else {
            std::cout << "Vulkan skipped: " << unavailable << '\n';
        }
#endif
        std::cout << "PASS UI shader effects in native scene passes: D3D11 WARP, Vulkan " << vulkan
                  << "; effect against its box, data, sRGB and linear textures, repeat, time\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
