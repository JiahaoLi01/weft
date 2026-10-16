#include "constants/common.hpp"
#include "scene/scene.hpp"
#include "support/check.hpp"
#include "support/d3d11_scene.hpp"
#include "support/files.hpp"
#include "support/json_value.hpp"
#include "support/native_scene.hpp"
#include "support/png.hpp"
#include "support/scene_fonts.hpp"
#include "render/srgb.hpp"

#include <DirectXPackedVector.h>
#include <d3d11.h>
#include <wrl/client.h>
#ifdef WEFT_TEST_VULKAN
#include "render/backends/vulkan/scene_renderer.hpp"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

// The CSS the CRPG floating UI needs of scene pages (13.4a) against Chrome: each case of
// tests/fixtures/scene-paint-reference/cases.json (text-overflow: ellipsis, text-shadow, filter: drop-shadow() on svg
// icons, inset box-shadow, radial gradients of every shape, size and position, repeating gradients, calc() insets,
// transform-origin keywords, translate3d() and em lengths) built as the same nodes, laid out by the scene solver and
// drawn by the native D3D11 pass and by the Direct2D reference raster at the case's device scale, against Chrome's
// screenshot of it (tools/browser_scene_paint_reference.mjs), whose manifest must name the cases as they are. Each
// drawing stays within its case's limits of Chrome's: the mean RGB difference, the share of pixels more than 32 levels
// apart (glyph edges: FreeType's grayscale coverage against Chrome's subpixel text), and the largest difference once
// both are averaged over 5 x 5 pixels, where antialiasing evens out and only what is drawn where remains (a character
// more or less before an ellipsis, a shadow's offset or blur, a gradient's shape). Where Vulkan is built, the native
// Vulkan pass on this machine's device draws each case within 2 levels of the D3D11 one (skipped without a device).
// The host texture cases draw again with url() images of the same texels (the scene's images, as a page's files), which
// Chrome's capture of the same PNG images shows alike.
namespace {
    using weft::testing::JsonValue;
    using weft::testing::require;

    struct Stats {
        double mean{};
        double far_share{};
        int smooth{};
    };

    struct Limits {
        double mean{};
        double far_share{};
        int smooth{};
    };

    // The native pass's limits and the reference raster's, by case.
    const std::map<std::string, std::array<Limits, 2>>& limits() {
        static const std::map<std::string, std::array<Limits, 2>> cases{
            // Text: glyph edges as FreeType and Chrome draw them apart.
            {"ellipsis", {{{3, .06, 56}, {3, .06, 56}}}},
            {"ellipsis-plate", {{{3, .06, 56}, {3, .06, 56}}}},
            {"text-shadow", {{{3, .06, 56}, {3, .06, 56}}}},
            {"text-shadow-plate", {{{3, .06, 56}, {3, .06, 56}}}},
            {"text-shadow-glow", {{{3, .06, 56}, {3, .06, 56}}}},
            // Shapes, shadows and gradients: antialiased edges and rounding.
            {"drop-shadow", {{{.5, .005, 12}, {.5, .005, 12}}}},
            {"inset-bar", {{{.5, .005, 16}, {.5, .005, 16}}}},
            {"inset-shapes", {{{.5, .005, 12}, {.5, .005, 12}}}},
            {"radial", {{{.5, .005, 6}, {.5, .005, 6}}}},
            {"repeating", {{{.5, .005, 6}, {.5, .005, 6}}}},
            {"layout", {{{.5, .005, 8}, {.5, .005, 8}}}},
            // Host textures laid out as background images: Chrome filters the PNG's encoded values, the scene its
            // linear ones, and upscales with its own kernel; tiles' seams and edges.
            {"texture-size", {{{1, .01, 24}, {1, .01, 24}}}},
            {"texture-position", {{{1, .01, 24}, {1, .01, 24}}}},
            {"texture-repeat", {{{1, .01, 24}, {1, .01, 24}}}},
            {"texture-boxes", {{{1, .01, 24}, {1, .01, 24}}}},
        };
        return cases;
    }

    const std::set<std::string>& svg_tags() {
        static const std::set<std::string> tags{"svg",  "path",     "circle",  "ellipse", "line",
                                                "rect", "polyline", "polygon", "g",       "text"};
        return tags;
    }

    // The cases' textures as url() images: "<name>.png", straight sRGB RGBA8 as Chrome's PNG holds them.
    struct CaseImages final : weft::SceneImages {
        std::map<std::string, std::shared_ptr<const weft::paint::Asset>, std::less<>> images;

        std::shared_ptr<const weft::paint::Asset> image(std::string_view url) override {
            const auto found = images.find(url);
            return found == images.end() ? nullptr : found->second;
        }
    };

    // A style's -weft-texture(name) as url(name.png).
    std::string as_url(std::string style) {
        constexpr std::string_view open = "-weft-texture(";
        for (auto at = style.find(open); at != style.npos; at = style.find(open, at)) {
            const auto close = style.find(')', at);
            require(close != style.npos, "a texture's name");
            const auto name = style.substr(at + open.size(), close - at - open.size());
            style.replace(at, close + 1 - at, "url(" + name + ".png)");
        }
        return style;
    }

    void build(weft::testing::NativeScene& s, weft::dom::Node& parent, const JsonValue& node, bool urls) {
        if (node.kind == JsonValue::Kind::String) {
            auto& t = s.tree.create(weft::constants::text_node_type, "#text", "", s.document->identity);
            t.data = node.string;
            s.tree.insert(parent, t, nullptr);
            return;
        }
        const auto& tag = node["tag"].string;
        auto& e = svg_tags().contains(tag) ? s.svg(tag) : s.html(tag);
        if (const auto* style = node.find("style")) {
            s.set(e, "style", urls ? as_url(style->string) : style->string);
        }
        if (const auto* attributes = node.find("attributes")) {
            for (const auto& [name, value] : attributes->object) {
                s.set(e, name, value.string);
            }
        }
        s.tree.insert(parent, e, nullptr);
        if (const auto* children = node.find("children")) {
            for (const auto& c : children->array) {
                build(s, e, c, urls);
            }
        }
    }

    // RGB of a drawing at (x, y): the native pass's encoded premultiplied BGRA, the raster's and Chrome's RGBA, all over
    // an opaque root.
    struct View {
        const std::vector<std::uint8_t>& pixels;
        bool bgra{};

        int at(unsigned w, unsigned x, unsigned y, unsigned c) const {
            const auto* p = pixels.data() + (std::size_t(y) * w + x) * 4;
            return p[bgra ? 2 - c : c];
        }
    };

    Stats compare(const View& drawn, const View& chrome, unsigned w, unsigned h) {
        Stats s;
        double sum{};
        std::size_t apart{};
        for (unsigned y = 0; y < h; ++y) {
            for (unsigned x = 0; x < w; ++x) {
                int worst{};
                for (unsigned c = 0; c < 3; ++c) {
                    const int d = std::abs(drawn.at(w, x, y, c) - chrome.at(w, x, y, c));
                    sum += d;
                    worst = std::max(worst, d);
                }
                apart += worst > 32;
            }
        }
        s.mean = sum / (3.0 * w * h);
        s.far_share = double(apart) / (double(w) * h);
        // Each channel averaged over 5 x 5 pixels (clamped at the edges): summed area tables.
        for (unsigned c = 0; c < 3; ++c) {
            std::vector<long long> a((w + 1) * std::size_t(h + 1));
            std::vector<long long> b(a.size());
            for (unsigned y = 0; y < h; ++y) {
                for (unsigned x = 0; x < w; ++x) {
                    const auto i = (std::size_t(y) + 1) * (w + 1) + x + 1;
                    a[i] = drawn.at(w, x, y, c) + a[i - 1] + a[i - (w + 1)] - a[i - (w + 1) - 1];
                    b[i] = chrome.at(w, x, y, c) + b[i - 1] + b[i - (w + 1)] - b[i - (w + 1) - 1];
                }
            }
            const auto box = [&](const std::vector<long long>& t, int x0, int y0, int x1, int y1) {
                return t[std::size_t(y1) * (w + 1) + x1] - t[std::size_t(y0) * (w + 1) + x1] -
                       t[std::size_t(y1) * (w + 1) + x0] + t[std::size_t(y0) * (w + 1) + x0];
            };
            for (int y = 0; y < int(h); ++y) {
                for (int x = 0; x < int(w); ++x) {
                    const int x0 = std::max(0, x - 2);
                    const int y0 = std::max(0, y - 2);
                    const int x1 = std::min(int(w), x + 3);
                    const int y1 = std::min(int(h), y + 3);
                    const auto n = double((x1 - x0) * (y1 - y0));
                    const auto d = std::abs(double(box(a, x0, y0, x1, y1) - box(b, x0, y0, x1, y1))) / n;
                    s.smooth = std::max(s.smooth, int(std::lround(d)));
                }
            }
        }
        return s;
    }
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 3, "usage: weft_scene_paint_reference_tests <artifact folder> <cases.json sha256>");
        const std::filesystem::path folder = argv[1];
        std::filesystem::create_directories(folder);
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM init");
        const std::filesystem::path fixtures =
            std::filesystem::path(WEFT_SOURCE_DIR) / "tests/fixtures/scene-paint-reference";
        const auto spec = weft::testing::parse_json(weft::testing::read_file((fixtures / "cases.json").string()));
        // Chrome's captures are of these cases: the manifest records the hash of the cases they were taken from, and
        // one capture for each case, at its scale.
        const auto manifest =
            weft::testing::parse_json(weft::testing::read_file((fixtures / "manifest.json").string()));
        require(manifest["cases_sha256"].string == argv[2],
                "Chrome's captures are of other cases: record them again with tools/browser_scene_paint_reference.mjs");
        require(manifest["captures"].size() == spec["cases"].size(), "a capture for each case");
        const auto width = unsigned(spec["width"].number);
        const auto height = unsigned(spec["height"].number);
        // The cases' host textures: straight sRGB RGBA8 as Chrome's PNG holds them, as linear premultiplied texels;
        // and the same texels as url() images.
        std::map<std::string, weft::SceneRasterTexture> textures;
        CaseImages images;
        if (const auto* listed = spec.find("textures")) {
            for (const auto& [name, t] : listed->object) {
                weft::SceneRasterTexture texture{unsigned(t["width"].number), unsigned(t["height"].number), {}};
                weft::paint::Asset image{texture.width, texture.height, weft::paint::Encoding::SrgbStraight, {}, 0};
                const auto& hex = t["rgba"].string;
                require(hex.size() == std::size_t(texture.width) * texture.height * 8, name + ": the texture's texels");
                for (std::size_t k = 0; k < hex.size(); k += 8) {
                    std::array<float, 4> c{};
                    for (unsigned j = 0; j < 4; ++j) {
                        const auto byte = std::stoi(hex.substr(k + 2 * j, 2), nullptr, 16);
                        image.bytes.push_back(std::uint8_t(byte));
                        c[j] = float(byte) / 255;
                    }
                    for (unsigned j = 0; j < 3; ++j) {
                        texture.rgba.push_back(weft::render::srgb_to_linear(c[j]) * c[3]);
                    }
                    texture.rgba.push_back(c[3]);
                }
                textures.emplace(name, std::move(texture));
                images.images.emplace(name + ".png", std::make_shared<const weft::paint::Asset>(std::move(image)));
            }
        }
        auto& text = weft::testing::thread_scene_text();
#ifdef WEFT_TEST_VULKAN
        namespace vk = weft::render::vulkan;
        std::string unavailable;
        const auto device = vk::Device::open({}, unavailable);
        std::optional<vk::SceneRenderer> vulkan;
        if (device) {
            vulkan.emplace(*device, text);
        } else {
            std::cout << "Vulkan skipped: " << unavailable << '\n';
        }
#endif
        bool failed{};
        std::cout << "case                    native: mean >32 share 5x5 | raster: mean >32 share 5x5 (limits)\n";
        for (std::size_t i = 0; i < spec["cases"].size(); ++i) {
            const auto& c = spec["cases"][i];
            const auto name = c["name"].string;
            const auto scale = float(c["scale"].number);
            require(manifest["captures"][i]["name"].string == name && manifest["captures"][i]["scale"].number == scale,
                    name + ": Chrome's capture of the case");
            const auto limit = limits().find(name);
            require(limit != limits().end(), name + ": the case's limits");
            // A host texture case twice: as -weft-texture() and as url() images of the same texels.
            for (const bool urls : {false, true}) {
                if (urls && !name.starts_with("texture-")) {
                    continue;
                }
                const auto label = urls ? name + "-url" : name;
                weft::testing::NativeScene s;
                const auto* root = c.find("root");
                s.set(*s.root, "style", spec["root"].string + ";" + (root ? root->string : std::string()));
                for (const auto& n : c["nodes"].array) {
                    build(s, *s.root, n, urls);
                }
                weft::SceneSolver solver("", "", text);
                solver.set_images(&images);
                const auto frame = solver.frame(s.tree.snapshot(*s.root), float(width), float(height), 0);
                const auto w = unsigned(width * scale);
                const auto h = unsigned(height * scale);
                weft::testing::D3D11Scene d3d11{w, h};
                // The textures on the device: RGBA16F of their linear premultiplied texels.
                std::vector<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> views;
                std::vector<weft::SceneTexture> bound;
                for (const auto& [texture_name, t] : textures) {
                    std::vector<std::uint16_t> halves;
                    for (const float v : t.rgba) {
                        halves.push_back(DirectX::PackedVector::XMConvertFloatToHalf(v));
                    }
                    D3D11_TEXTURE2D_DESC desc{};
                    desc.Width = t.width;
                    desc.Height = t.height;
                    desc.MipLevels = desc.ArraySize = 1;
                    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                    desc.SampleDesc.Count = 1;
                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    const D3D11_SUBRESOURCE_DATA data{halves.data(), t.width * 8, 0};
                    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
                    auto& view = views.emplace_back();
                    require(SUCCEEDED(d3d11.gpu.device->CreateTexture2D(&desc, &data, &texture)) &&
                                SUCCEEDED(d3d11.gpu.device->CreateShaderResourceView(texture.Get(), nullptr, &view)),
                            texture_name + ": the host texture");
                    bound.push_back({texture_name, std::uint64_t(reinterpret_cast<std::uintptr_t>(view.Get())), false});
                }
                using Textures = std::span<const weft::SceneTexture>;
                const auto native = d3d11.draw(frame, scale, nullptr, {}, urls ? Textures{} : Textures(bound));
                d3d11.gpu.check_debug_messages();
                require(d3d11.scene.fallbacks == 0, label + ": the native pass drew the case");
#ifdef WEFT_TEST_VULKAN
                // Host textures are bound by the View backends on Vulkan (render_view_contract); these cases draw none.
                // url() images are the pass's own.
                if (vulkan && (urls || !name.starts_with("texture-"))) {
                    const auto drawn = vulkan->draw(frame, scale, w, h, std::nullopt, 0, {});
                    require(drawn.has_value() && drawn->size() == native.size(), label + ": the native Vulkan pass");
                    int worst{};
                    for (std::size_t k = 0; k < native.size(); ++k) {
                        worst = std::max(worst, std::abs(int((*drawn)[k]) - int(native[k])));
                    }
                    std::printf("%-22s Vulkan against D3D11 WARP: worst %d\n", label.c_str(), worst);
                    require(worst <= 2, label + ": Vulkan within 2 levels of D3D11 WARP");
                }
#endif
                weft::SceneRaster reference(text);
                if (!urls) {
                    reference.bind_textures(textures);
                }
                const auto raster = reference.raster(frame, scale);
                require(raster.width == w && raster.height == h, label + ": the reference raster's size");
                const auto chrome = weft::testing::read_png(fixtures / (name + ".png"));
                require(chrome.width == w && chrome.height == h, name + ": Chrome's capture at the case's scale");
                const auto a = compare({native, true}, {chrome.rgba}, w, h);
                const auto b = compare({raster.rgba}, {chrome.rgba}, w, h);
                const auto& [ln, lr] = limit->second;
                std::printf("%-22s %6.2f %.4f %4d | %6.2f %.4f %4d (%.2f %.3f %d | %.2f %.3f %d)\n", label.c_str(),
                            a.mean, a.far_share, a.smooth, b.mean, b.far_share, b.smooth, ln.mean, ln.far_share,
                            ln.smooth, lr.mean, lr.far_share, lr.smooth);
                // Evidence: the native pass, Chrome and the reference raster side by side.
                std::vector<std::uint8_t> trio(std::size_t(w) * 3 * h * 4);
                for (unsigned y = 0; y < h; ++y) {
                    for (unsigned x = 0; x < w; ++x) {
                        const auto* n = native.data() + (std::size_t(y) * w + x) * 4;
                        auto* left = trio.data() + (std::size_t(y) * w * 3 + x) * 4;
                        left[0] = n[2];
                        left[1] = n[1];
                        left[2] = n[0];
                        left[3] = 255;
                        const auto* cr = chrome.rgba.data() + (std::size_t(y) * w + x) * 4;
                        std::copy(cr, cr + 4, trio.data() + (std::size_t(y) * w * 3 + w + x) * 4);
                        const auto* r = raster.rgba.data() + (std::size_t(y) * w + x) * 4;
                        auto* right = trio.data() + (std::size_t(y) * w * 3 + 2 * w + x) * 4;
                        std::copy(r, r + 3, right);
                        right[3] = 255;
                    }
                }
                weft::testing::write_png(folder / (label + ".png"), w * 3, h, trio);
                const auto within = [](const Stats& s, const Limits& l) {
                    return s.mean <= l.mean && s.far_share <= l.far_share && s.smooth <= l.smooth;
                };
                if (!within(a, ln) || !within(b, lr)) {
                    std::cout << "  " << label << ": outside its limits of Chrome's\n";
                    failed = true;
                }
            }
        }
        require(!failed, "the scene paint cases within their limits of Chrome's");
#ifdef WEFT_TEST_VULKAN
        require(!device || device->validation_messages == 0, "Vulkan validation");
#endif
        std::cout << "PASS scene paint cases against Chrome\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
