#define NOMINMAX
#include <weft/render_backend.h>
#include <weft/render_backend_d3d11.h>
#include <weft/weft.h>

#include "render/backends/d3d11/scene_renderer.hpp"
#include "render/backends/d3d11/view_backend.hpp"
#include "render/srgb.hpp"
#include "support/check.hpp"
#include "view/view_abi.hpp"

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#ifdef WEFT_TEST_VULKAN
#include <weft/render_backend_vulkan.h>

#include "render/backends/vulkan/device.hpp"
#include "render/backends/vulkan/pipeline.hpp"
#include "render/backends/vulkan/scene_renderer.hpp"
#include "render/backends/vulkan/view_backend.hpp"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// View frames through the renderer C ABI (render_backend.h, weft_renderer_prepare_scene): the built-in D3D11 backend
// (WARP) and Vulkan backend (this machine's device, with validation) draw a View's frame over the game's picture in
// the host's target exactly as their native scene passes draw it directly; a pass keeps the target's contents or
// clears it to the host's color; a frame whose revision is unchanged redraws the kept pass where the View's latest
// anchors are; and a backend refuses what is not its own.
namespace {
    using Microsoft::WRL::ComPtr;
    using weft::testing::require;
    using Pixels = std::vector<std::uint8_t>;

    constexpr unsigned width = 320;
    constexpr unsigned height = 200;
    // The game's picture under the UI, BGRA.
    constexpr std::array<std::uint8_t, 4> game{0x80, 0x60, 0x20, 0xff};
    constexpr std::uint64_t queue = 1;

    void ok(weft_render_result result, const char* what) {
        if (result != WEFT_RENDER_OK) {
            throw std::runtime_error(std::string(what) + " result=" + std::to_string(result));
        }
    }

    void ok(weft_result result, const char* what) {
        if (result != WEFT_OK) {
            throw std::runtime_error(std::string(what) + ": " + weft_result_string(result));
        }
    }

    // A View of two anchored plates with text, a static panel and a glass panel that darkens what is beneath it,
    // through the View's C ABI.
    struct Page {
        weft_runtime* runtime{};
        weft_view_handle view{};
        bool owner{};

        // root: the root's own style (a translucent background over the game, say); shared: another Page's runtime,
        // whose fonts this View lays its text out with too.
        explicit Page(const std::string& root = {}, weft_runtime* shared = nullptr)
            : Page(plates_script, plates_css(root), shared) {
        }

        // A page of its own script and stylesheet.
        Page(std::string_view script, const std::string& css, weft_runtime* shared = nullptr)
            : runtime(shared), owner(!shared) {
            if (owner) {
                ok(weft_runtime_create(nullptr, &runtime), "runtime");
            }
            const weft_view_desc desc{sizeof(desc), 0, float(width), float(height), 1, 0};
            ok(weft_view_create(runtime, &desc, &view), "view");
            const weft_script s{"page.js", 7, script.data(), script.size()};
            const weft_page_desc page{sizeof(page), 0, &s, 1, 0, css.data(), css.size(), nullptr, 0, nullptr, 0};
            ok(weft_view_load(runtime, view, &page), "load");
        }

        static constexpr std::string_view plates_script = R"JS(
              const root = document.getElementById('root');
              for (const [key, text] of [[1, 'Goblin'], [2, 'Orc']]) {
                const plate = document.createElement('div');
                plate.className = 'plate';
                plate.setAttribute('data-anchor', String(key));
                plate.textContent = text;
                root.appendChild(plate);
              }
              const panel = document.createElement('div');
              panel.className = 'panel';
              panel.textContent = 'Ready';
              root.appendChild(panel);
              const glass = document.createElement('div');
              glass.className = 'glass';
              root.appendChild(glass);
              weft.engine.on('rename', name => { panel.textContent = name; });
            )JS";

        static std::string plates_css(const std::string& root) {
            return "#root{position:relative;width:100%;height:100%;font-family:sans-serif;" + root +
                   "}"
                   ".plate{position:absolute;left:0;top:0;width:60px;height:20px;background-color:rgba(200,40,40,0.8);"
                   "border-radius:4px;color:#fff;font-size:12px;line-height:20px;text-align:center}"
                   ".panel{position:absolute;right:10px;bottom:10px;width:80px;height:30px;background-color:#334;"
                   "border:1px solid #aab;color:#eee;font-size:14px;line-height:28px}"
                   ".glass{position:absolute;left:250px;top:10px;width:60px;height:40px;"
                   "backdrop-filter:blur(2px) brightness(0.5)}";
        }

        ~Page() {
            weft_view_destroy(runtime, view);
            if (owner) {
                weft_runtime_destroy(runtime);
            }
        }

        void anchors(float x1, float y1, float x2, float y2) {
            const weft_anchor a[]{{1, x1, y1, 1, 0, 0, 0}, {2, x2, y2, 1, 1, 0, 0}};
            ok(weft_view_write_anchors(runtime, view, a, 2), "anchors");
        }

        weft_view_frame* frame(double time) {
            ok(weft_view_advance(runtime, view, time), "advance");
            weft_view_frame* f{};
            ok(weft_view_acquire_frame(runtime, view, &f), "frame");
            return f;
        }
    };

    std::array<std::uint8_t, 4> pixel(const Pixels& p, unsigned x, unsigned y) {
        const auto* at = p.data() + (std::size_t(y) * width + x) * 4;
        return {at[0], at[1], at[2], at[3]};
    }

    // The game's picture, darkened.
    bool darker(const std::array<std::uint8_t, 4>& p) {
        return p[0] < game[0] && p[1] < game[1] && p[2] <= game[2] && p[3] == game[3];
    }

    // A page of host textures: "portrait" over a box with round corners, "minimap" over a red background color and
    // under a white border, and "missing", which no frame binds.
    constexpr std::string_view textures_script = R"JS(
      const root = document.getElementById('root');
      for (const name of ['portrait', 'minimap', 'missing', 'tiled']) {
        const element = document.createElement('div');
        element.className = name;
        root.appendChild(element);
      }
    )JS";
    const std::string textures_css =
        "#root{position:relative;width:100%;height:100%}"
        ".portrait{position:absolute;left:20px;top:20px;width:64px;height:48px;border-radius:16px;"
        "background-image:-weft-texture(portrait);background-size:100% 100%}"
        ".minimap{position:absolute;left:120px;top:20px;width:80px;height:60px;background:-weft-texture(minimap) "
        "border-box;background-size:100% 100%;background-color:#ff0000;border:4px solid #ffffff}"
        ".missing{position:absolute;left:220px;top:20px;width:40px;height:40px;"
        "background-image:-weft-texture(missing)}"
        ".tiled{position:absolute;left:20px;top:100px;width:120px;height:60px;border:3px solid #ffffff;"
        "border-radius:10px;background:-weft-texture(portrait) right 5px center / 14px 10px space round;"
        "background-color:#202020}";

    // A page of one host texture on a plane that turns away from the viewer (perspective): the pass draws the plane's
    // content into an image of its own first.
    constexpr std::string_view tilted_script = R"JS(
      const tilted = document.createElement('div');
      tilted.className = 'tilted';
      document.getElementById('root').appendChild(tilted);
    )JS";
    const std::string tilted_css =
        "#root{position:relative;width:100%;height:100%}"
        ".tilted{position:absolute;left:20px;top:110px;width:64px;height:48px;transform:perspective(200px) "
        "rotateY(25deg);background-image:-weft-texture(tilted);background-size:100% 100%}";

    // A 4 x 4 texture of linear premultiplied RGBA8 in quadrants of 2 x 2 texels: red, green; a linear grey, and blue
    // at half opacity. The rows are top down; bottom up, the quadrants swap rows.
    constexpr std::array<std::array<std::uint8_t, 4>, 4> quadrant{
        {{255, 0, 0, 255}, {0, 255, 0, 255}, {128, 128, 128, 255}, {0, 0, 128, 128}}};

    // The texture page as the D3D11 backend (WARP) drew it, for the Vulkan backend's.
    std::vector<std::uint8_t> d3d11_textured;

    // A page of textures in other forms: an sRGB view, a BGRA one, and one of five levels shown at its size and at a
    // quarter of it.
    constexpr std::string_view formats_script = R"JS(
      const root = document.getElementById('root');
      for (const name of ['srgb', 'bgra', 'shrunk', 'full']) {
        const element = document.createElement('div');
        element.className = name;
        root.appendChild(element);
      }
    )JS";
    const std::string formats_css = "#root{position:relative;width:100%;height:100%}"
                                    ".srgb{position:absolute;left:10px;top:10px;width:40px;height:40px;"
                                    "background:-weft-texture(srgb) 0 0 / 100% 100% no-repeat}"
                                    ".bgra{position:absolute;left:60px;top:10px;width:40px;height:40px;"
                                    "background:-weft-texture(bgra) 0 0 / 100% 100% no-repeat}"
                                    ".shrunk{position:absolute;left:110px;top:10px;width:4px;height:4px;"
                                    "background:-weft-texture(levels) 0 0 / 100% 100% no-repeat}"
                                    ".full{position:absolute;left:130px;top:10px;width:16px;height:16px;"
                                    "background:-weft-texture(levels) 0 0 / 100% 100% no-repeat}";

    std::vector<std::uint8_t> solid_texels(unsigned w, unsigned h, std::array<std::uint8_t, 4> c) {
        std::vector<std::uint8_t> out;
        for (unsigned i = 0; i < w * h; ++i) {
            out.insert(out.end(), c.begin(), c.end());
        }
        return out;
    }

    // The texels: the sRGB view's encoded (200, 100, 50), the BGRA one's red (in B, G, R, A order), and the 16 x 16
    // texture's levels each its own color: red, green, blue, white, black.
    constexpr std::array<std::uint8_t, 4> srgb_texel{200, 100, 50, 255};
    constexpr std::array<std::uint8_t, 4> bgra_texel{0, 0, 255, 255};
    constexpr std::array<std::array<std::uint8_t, 4>, 5> level_colors{
        {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 255, 255}, {0, 0, 0, 255}}};

    std::vector<std::vector<std::uint8_t>> level_texels() {
        std::vector<std::vector<std::uint8_t>> out;
        for (unsigned k = 0; k < level_colors.size(); ++k) {
            out.push_back(solid_texels(16 >> k, 16 >> k, level_colors[k]));
        }
        return out;
    }

    bool close(const std::array<std::uint8_t, 4>& a, const std::array<std::uint8_t, 4>& b);

    // What the page shows (B, G, R, A): the sRGB view's encoded values again (decoded, then encoded as the page
    // composites), red from the BGRA view, the texture shrunk to a quarter its third level, at its size its first.
    void formats_shown(const Pixels& p) {
        require(close(pixel(p, 30, 30), {50, 100, 200, 255}), "an sRGB view's encoded values");
        require(pixel(p, 80, 30) == std::array<std::uint8_t, 4>{0, 0, 255, 255}, "a BGRA view's channels");
        require(close(pixel(p, 112, 12), {255, 0, 0, 255}), "a texture shrunk to a quarter shows its third level");
        require(pixel(p, 138, 18) == std::array<std::uint8_t, 4>{0, 0, 255, 255}, "a texture at its size, its first");
    }

    // The format page as the D3D11 backend (WARP) drew it, for the Vulkan backend's.
    std::vector<std::uint8_t> d3d11_formatted;

    std::vector<std::uint8_t> quadrants(bool bottom_up) {
        std::vector<std::uint8_t> out;
        for (unsigned y = 0; y < 4; ++y) {
            for (unsigned x = 0; x < 4; ++x) {
                const unsigned row = bottom_up ? 3 - y : y;
                const auto& texel = quadrant[(row < 2 ? 0 : 2) + (x < 2 ? 0 : 1)];
                out.insert(out.end(), texel.begin(), texel.end());
            }
        }
        return out;
    }

    // A linear premultiplied color as the encoded BGRA bytes a scene pass writes.
    std::array<std::uint8_t, 4> encoded_bgra(const float* linear) {
        const auto byte = [](float c) {
            return std::uint8_t(std::lround(std::clamp(c, 0.f, 1.f) * 255));
        };
        const float a = linear[3];
        const auto e = [&](float c) {
            return a > 0 ? weft::render::linear_to_srgb(c / a) * a : 0.f;
        };
        return {byte(e(linear[2])), byte(e(linear[1])), byte(e(linear[0])), byte(a)};
    }

    struct D3D11 {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;

        D3D11() {
            const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
            require(
                SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                            levels, 1, D3D11_SDK_VERSION, &device, nullptr, &context)),
                "a WARP device");
        }

        struct Target {
            ComPtr<ID3D11Texture2D> texture;
            ComPtr<ID3D11RenderTargetView> rtv;
        };

        Target target(DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            Target t;
            require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &t.texture)) &&
                        SUCCEEDED(device->CreateRenderTargetView(t.texture.Get(), nullptr, &t.rtv)),
                    "a target");
            const float picture[]{game[2] / 255.f, game[1] / 255.f, game[0] / 255.f, game[3] / 255.f};
            context->ClearRenderTargetView(t.rtv.Get(), picture);
            return t;
        }

        // A linear premultiplied RGBA8 texture of w x h texels (rgba, rows top down) a host draws and samples.
        struct Texture {
            ComPtr<ID3D11Texture2D> texture;
            ComPtr<ID3D11ShaderResourceView> srv;
            ComPtr<ID3D11RenderTargetView> rtv;
        };

        Texture texture(unsigned w, unsigned h, const std::vector<std::uint8_t>& rgba,
                        DXGI_FORMAT view = DXGI_FORMAT_R8G8B8A8_UNORM) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = w;
            desc.Height = h;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            const D3D11_SUBRESOURCE_DATA data{rgba.data(), w * 4, 0};
            Texture t;
            D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = view;
            srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = 1;
            D3D11_RENDER_TARGET_VIEW_DESC rtv{};
            rtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            require(SUCCEEDED(device->CreateTexture2D(&desc, &data, &t.texture)) &&
                        SUCCEEDED(device->CreateShaderResourceView(t.texture.Get(), &srv, &t.srv)) &&
                        SUCCEEDED(device->CreateRenderTargetView(t.texture.Get(), &rtv, &t.rtv)),
                    "a host texture");
            return t;
        }

        // A texture of a format and its levels' texels (4 bytes each, rows top down), sampled whole.
        Texture made(DXGI_FORMAT format, unsigned w, unsigned h, const std::vector<std::vector<std::uint8_t>>& levels) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = w;
            desc.Height = h;
            desc.MipLevels = UINT(levels.size());
            desc.ArraySize = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            std::vector<D3D11_SUBRESOURCE_DATA> data;
            for (std::size_t k = 0; k < levels.size(); ++k) {
                data.push_back({levels[k].data(), std::max(1u, w >> k) * 4, 0});
            }
            Texture t;
            require(SUCCEEDED(device->CreateTexture2D(&desc, data.data(), &t.texture)) &&
                        SUCCEEDED(device->CreateShaderResourceView(t.texture.Get(), nullptr, &t.srv)),
                    "a host texture of its format and levels");
            return t;
        }

        Pixels read(const Target& t) {
            D3D11_TEXTURE2D_DESC desc{};
            t.texture->GetDesc(&desc);
            desc.BindFlags = 0;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)), "readback");
            context->CopyResource(staging.Get(), t.texture.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            require(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "map");
            Pixels out(std::size_t(width) * height * 4);
            for (unsigned y = 0; y < height; ++y) {
                std::memcpy(out.data() + std::size_t(y) * width * 4,
                            static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(y) * mapped.RowPitch,
                            width * 4);
            }
            context->Unmap(staging.Get(), 0);
            return out;
        }
    };

    // Draws a prepared frame into target through the renderer: encode, submit, complete.
    weft_render_result draw(weft_renderer* renderer, weft_render_handle prepared, ID3D11RenderTargetView* target,
                            std::uint64_t& serial, std::uint32_t format = WEFT_RENDER_TARGET_LINEAR_BGRA8,
                            void* deferred = nullptr, const float* clear = nullptr) {
        weft_render_info info{};
        info.struct_size = sizeof(info);
        ok(weft_renderer_get_info(renderer, &info), "info");
        void* list = reinterpret_cast<void*>(1);
        weft_render_d3d11_pass payload{sizeof(payload), WEFT_RENDER_ABI_VERSION, deferred, target, &list, 0, 0, {}};
        if (clear) {
            payload.clear_target = 1;
            std::copy(clear, clear + 4, payload.clear_linear_premultiplied);
        }
        const weft_render_pass pass{sizeof(pass), WEFT_RENDER_ABI_VERSION, info.identity, width, height, format, 1,
                                    &payload,     sizeof(payload)};
        weft_render_handle use{};
        const auto result = weft_renderer_encode(renderer, prepared, &pass, &use);
        if (result == WEFT_RENDER_OK) {
            require(list == nullptr, "an immediate pass has no command list");
            ok(weft_renderer_notify_submitted(renderer, use, queue, ++serial), "submitted");
            ok(weft_renderer_notify_completed(renderer, info.identity.device_epoch, queue, serial), "completed");
        }
        return result;
    }

    weft_render_handle prepare(weft_renderer* renderer, const weft_view_frame* frame, float scale) {
        const weft_render_scene_frame f{sizeof(f), WEFT_RENDER_ABI_VERSION, frame, scale, 0};
        weft_render_handle out{};
        ok(weft_renderer_prepare_scene(renderer, &f, &out), "prepare scene");
        return out;
    }

    weft_render_handle prepare(weft_renderer* renderer, const weft_view_frame* frame) {
        return prepare(renderer, frame, 0);
    }

    // The frame drawn by a native D3D11 scene pass of its own, over the game's picture (or replacing it).
    Pixels direct(D3D11& gpu, const weft::ViewFrame& frame, float scale = 0, bool over = true) {
        weft::SceneText text(*frame.fonts);
        weft::render::d3d11::SceneRenderer scene(gpu.device.Get(), text);
        weft::render::d3d11::SceneRenderer::Slot slot;
        const auto target = gpu.target();
        require(scene.paint(slot, *frame.scene, target.rtv.Get(), scale ? scale : frame.scale, width, height,
                            std::nullopt, 1, {}, &frame.anchors->take(weft::SceneAnchorChannel::Reader::Render),
                            {over, std::nullopt}),
                "the native pass");
        return gpu.read(target);
    }

    void d3d11_frames() {
        D3D11 gpu;
        Page page;
        page.anchors(40, 30, 150, 90);
        auto* frame = page.frame(0.016);
        const auto held = frame->frame;
        weft_render_d3d11_desc desc{sizeof(desc), WEFT_RENDER_ABI_VERSION, gpu.device.Get(), queue, 0};
        weft_render_d3d11* backend{};
        weft_render_backend table{};
        ok(weft_render_d3d11_create(&desc, &backend, &table), "the D3D11 backend");
        require(table.capabilities.features == WEFT_RENDER_FEATURE_SCENE && table.prepare_scene && !table.prepare_list,
                "a backend of View frames");
        weft_renderer* renderer{};
        ok(weft_renderer_create(&table, nullptr, &renderer), "renderer");
        std::uint64_t serial{};

        // Through the C ABI, over the game's picture, as the native pass draws it directly; the frame's handle can go
        // once the frame is prepared.
        auto prepared = prepare(renderer, frame);
        weft_view_frame_release(frame);
        auto target = gpu.target();
        ok(draw(renderer, prepared, target.rtv.Get(), serial), "draw");
        ok(weft_renderer_release_prepared(renderer, prepared), "release");
        const auto through = gpu.read(target);
        require(through == direct(gpu, *held), "the C ABI draws as the native pass");
        require(pixel(through, 2, 2) == game && pixel(through, width - 2, 2) == game, "the game shows around the UI");
        require(pixel(through, 15, 22) != game, "a plate over the game");
        require(darker(pixel(through, 280, 30)), "the glass darkens the game beneath it");

        // A clear to the host's color first.
        const float blue[]{0.05f, 0.1f, 0.4f, 1};
        frame = page.frame(0.020);
        prepared = prepare(renderer, frame);
        weft_view_frame_release(frame);
        target = gpu.target();
        ok(draw(renderer, prepared, target.rtv.Get(), serial, WEFT_RENDER_TARGET_LINEAR_BGRA8, nullptr, blue), "clear");
        ok(weft_renderer_release_prepared(renderer, prepared), "release");
        const auto cleared = gpu.read(target);
        require(pixel(cleared, 2, 2) == encoded_bgra(blue) && pixel(cleared, 15, 22) != encoded_bgra(blue),
                "the host's clear, then the page");

        // The anchors move, the scene does not: the same frame draws where the anchors are now.
        page.anchors(200, 120, 60, 150);
        frame = page.frame(0.032);
        require(frame->frame == held, "an unchanged scene is the same frame");
        prepared = prepare(renderer, frame);
        weft_view_frame_release(frame);
        target = gpu.target();
        ok(draw(renderer, prepared, target.rtv.Get(), serial), "draw moved");
        ok(weft_renderer_release_prepared(renderer, prepared), "release");
        const auto moved = gpu.read(target);
        require(moved == direct(gpu, *held) && moved != through, "drawn where the anchors are now");

        // At half the View's scale: the page in the target's top-left quarter.
        frame = page.frame(0.036);
        prepared = prepare(renderer, frame, 0.5f);
        weft_view_frame_release(frame);
        target = gpu.target();
        ok(draw(renderer, prepared, target.rtv.Get(), serial), "draw at half scale");
        ok(weft_renderer_release_prepared(renderer, prepared), "release");
        const auto half = gpu.read(target);
        require(half == direct(gpu, *held, 0.5f) && half != moved, "a scale of the host's");

        // What is not this backend's.
        frame = page.frame(0.040);
        prepared = prepare(renderer, frame);
        ComPtr<ID3D11DeviceContext> deferred;
        require(SUCCEEDED(gpu.device->CreateDeferredContext(0, &deferred)), "a deferred context");
        target = gpu.target();
        require(draw(renderer, prepared, target.rtv.Get(), serial, WEFT_RENDER_TARGET_LINEAR_BGRA8, deferred.Get()) ==
                    WEFT_RENDER_UNSUPPORTED_FEATURE,
                "a deferred context");
        const auto srgb = gpu.target(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
        require(draw(renderer, prepared, srgb.rtv.Get(), serial, WEFT_RENDER_TARGET_SRGB_BGRA8) ==
                    WEFT_RENDER_UNSUPPORTED_FEATURE,
                "an sRGB target");
        require(draw(renderer, prepared, target.rtv.Get(), serial, WEFT_RENDER_TARGET_LINEAR_RGBA8) ==
                    WEFT_RENDER_UNSUPPORTED_FEATURE,
                "a format other than the view's");
        D3D11 other;
        const auto foreign = other.target();
        require(draw(renderer, prepared, foreign.rtv.Get(), serial) == WEFT_RENDER_WRONG_BACKEND,
                "a target of another device");
        {
            D3D11_TEXTURE2D_DESC texture{};
            texture.Width = width;
            texture.Height = height;
            texture.MipLevels = 2;
            texture.ArraySize = 1;
            texture.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            texture.SampleDesc.Count = 1;
            texture.BindFlags = D3D11_BIND_RENDER_TARGET;
            D3D11_RENDER_TARGET_VIEW_DESC second{};
            second.Format = texture.Format;
            second.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            second.Texture2D.MipSlice = 1;
            ComPtr<ID3D11Texture2D> mipped;
            ComPtr<ID3D11RenderTargetView> mip;
            require(SUCCEEDED(gpu.device->CreateTexture2D(&texture, nullptr, &mipped)) &&
                        SUCCEEDED(gpu.device->CreateRenderTargetView(mipped.Get(), &second, &mip)),
                    "a target of two mips");
            require(draw(renderer, prepared, mip.Get(), serial) == WEFT_RENDER_UNSUPPORTED_FEATURE, "a second mip");
            texture.Width = width + 1;
            texture.MipLevels = 1;
            ComPtr<ID3D11Texture2D> wider;
            ComPtr<ID3D11RenderTargetView> wider_view;
            require(SUCCEEDED(gpu.device->CreateTexture2D(&texture, nullptr, &wider)) &&
                        SUCCEEDED(gpu.device->CreateRenderTargetView(wider.Get(), nullptr, &wider_view)),
                    "a wider target");
            require(draw(renderer, prepared, wider_view.Get(), serial) == WEFT_RENDER_INVALID_ARGUMENT,
                    "a target of another size than the pass");
            const float straight[]{0.5f, 0, 0, 0.25f};
            require(draw(renderer, prepared, target.rtv.Get(), serial, WEFT_RENDER_TARGET_LINEAR_BGRA8, nullptr,
                         straight) == WEFT_RENDER_INVALID_ARGUMENT,
                    "a clear color that is not premultiplied");
        }
        const weft_render_frame quads{sizeof(quads), WEFT_RENDER_ABI_VERSION, nullptr, 0, 0, nullptr};
        weft_render_handle refused{};
        require(weft_renderer_prepare(renderer, &quads, &refused) == WEFT_RENDER_UNSUPPORTED_FEATURE,
                "quad frames are not this backend's");
        {
            Page another;
            another.anchors(0, 0, 0, 0);
            auto* theirs = another.frame(0.016);
            const weft_render_scene_frame f{sizeof(f), WEFT_RENDER_ABI_VERSION, theirs, 0, 0};
            require(weft_renderer_prepare_scene(renderer, &f, &refused) == WEFT_RENDER_INVALID_ARGUMENT,
                    "a frame of another runtime");
            weft_view_frame_release(theirs);
        }
        weft_render_scene_frame bad{sizeof(bad), WEFT_RENDER_ABI_VERSION, frame, -1, 0};
        require(weft_renderer_prepare_scene(renderer, &bad, &refused) == WEFT_RENDER_INVALID_ARGUMENT,
                "a negative scale");
        bad.scale = INFINITY;
        require(weft_renderer_prepare_scene(renderer, &bad, &refused) == WEFT_RENDER_INVALID_ARGUMENT,
                "an infinite scale");
        bad.scale = 0;
        bad.texture_count = 1;
        require(weft_renderer_prepare_scene(renderer, &bad, &refused) == WEFT_RENDER_INVALID_ARGUMENT,
                "textures counted but not given");
        bad.texture_count = 0;
        bad.struct_size = std::uint32_t(offsetof(weft_render_scene_frame, textures)) - 4;
        require(weft_renderer_prepare_scene(renderer, &bad, &refused) == WEFT_RENDER_INVALID_ARGUMENT,
                "a shorter frame description");
        bad.struct_size = sizeof(bad);
        bad.abi_version = WEFT_RENDER_ABI_VERSION + 1;
        require(weft_renderer_prepare_scene(renderer, &bad, &refused) == WEFT_RENDER_UNSUPPORTED_ABI,
                "a frame description of another version");
        bad.abi_version = WEFT_RENDER_ABI_VERSION;
        bad.frame = nullptr;
        require(weft_renderer_prepare_scene(renderer, &bad, &refused) == WEFT_RENDER_INVALID_ARGUMENT, "no frame");
        weft_view_frame_release(frame);
        // The View can go before its prepared frame is drawn; a renderer with a prepared frame is busy (and drains).
        ok(weft_view_destroy(page.runtime, page.view), "destroy the View");
        page.view = {};
        ok(draw(renderer, prepared, target.rtv.Get(), serial), "draw after the View went");
        require(weft_renderer_destroy(renderer) == WEFT_RENDER_BUSY, "busy with a prepared frame");
        ok(weft_renderer_release_prepared(renderer, prepared), "release");
        ok(weft_renderer_destroy(renderer), "destroy");
        weft_render_d3d11_destroy(backend);

        desc.device = nullptr;
        require(weft_render_d3d11_create(&desc, &backend, &table) == WEFT_RENDER_INVALID_ARGUMENT && !backend,
                "no device");
        desc = {sizeof(desc), WEFT_RENDER_ABI_VERSION + 1, gpu.device.Get(), queue, 0};
        require(weft_render_d3d11_create(&desc, &backend, &table) == WEFT_RENDER_UNSUPPORTED_ABI, "another version");
        desc = {sizeof(desc), WEFT_RENDER_ABI_VERSION, gpu.device.Get(), 0, 0};
        require(weft_render_d3d11_create(&desc, &backend, &table) == WEFT_RENDER_INVALID_ARGUMENT, "no queue");
    }

    // A root's own background over the game: a translucent one blends over it (or over the host's clear), an opaque
    // one hides it; neither is a clear that replaces the target. A renderer that drew the View's frame replacing its
    // target first draws it over the game afterwards as if it never had.
    void d3d11_root_backgrounds() {
        D3D11 gpu;
        Page page("background-color:rgba(10,20,30,0.5)");
        page.anchors(40, 30, 150, 90);
        auto* frame = page.frame(0.016);
        const auto held = frame->frame;
        weft::render::d3d11::ViewBackend backend(gpu.device.Get(), queue);
        weft_renderer* renderer{};
        ok(weft_renderer_create(&backend.table(), nullptr, &renderer), "renderer");
        std::uint64_t serial{};
        // The host's clear first: on the View's first pass, then on its kept pass.
        const float blue[]{0.05f, 0.1f, 0.4f, 1};
        Pixels cleared[2];
        for (auto& c : cleared) {
            const auto first = prepare(renderer, frame);
            const auto t = gpu.target();
            ok(draw(renderer, first, t.rtv.Get(), serial, WEFT_RENDER_TARGET_LINEAR_BGRA8, nullptr, blue), "clear");
            ok(weft_renderer_release_prepared(renderer, first), "release");
            c = gpu.read(t);
        }
        const auto prepared = prepare(renderer, frame);
        weft_view_frame_release(frame);
        auto target = gpu.target();
        ok(draw(renderer, prepared, target.rtv.Get(), serial), "draw");
        ok(weft_renderer_release_prepared(renderer, prepared), "release");
        const auto over = gpu.read(target);
        const auto replaced = direct(gpu, *held, 0, false);
        require(pixel(over, 2, 2) != game && pixel(over, 2, 2) != pixel(replaced, 2, 2) && over == direct(gpu, *held),
                "a translucent root blends over the game");
        require(cleared[0] == cleared[1] && pixel(cleared[0], 2, 2) != pixel(over, 2, 2) &&
                    pixel(cleared[0], 2, 2) != encoded_bgra(blue),
                "a translucent root over the host's clear, on the first pass and the kept one");
        // An opaque root hides the game: a box drawn over it, not folded into a clear the pass does not make.
        {
            Page opaque("background-color:#102030", page.runtime);
            opaque.anchors(40, 30, 150, 90);
            auto* f = opaque.frame(0.016);
            const auto p = prepare(renderer, f);
            weft_view_frame_release(f);
            const auto t = gpu.target();
            ok(draw(renderer, p, t.rtv.Get(), serial), "draw an opaque root");
            ok(weft_renderer_release_prepared(renderer, p), "release");
            const std::array<std::uint8_t, 4> root{0x30, 0x20, 0x10, 0xff};
            require(pixel(gpu.read(t), 2, 2) == root, "an opaque root hides the game");
        }
        // One renderer, one View: replacing first, then over the game; the kept pass is not the other mode's.
        weft::SceneText text(*held->fonts);
        weft::render::d3d11::SceneRenderer scene(gpu.device.Get(), text);
        weft::render::d3d11::SceneRenderer::Slot slot;
        const auto* anchors = &held->anchors->take(weft::SceneAnchorChannel::Reader::Render);
        target = gpu.target();
        require(scene.paint(slot, *held->scene, target.rtv.Get(), held->scale, width, height, std::nullopt, 1, {},
                            anchors, {false, std::nullopt}),
                "replacing");
        target = gpu.target();
        require(scene.paint(slot, *held->scene, target.rtv.Get(), held->scale, width, height, std::nullopt, 1, {},
                            anchors, {true, std::nullopt}),
                "over");
        require(gpu.read(target) == over, "over after replacing, as over alone");
        ok(weft_renderer_destroy(renderer), "destroy");
        // A table that claims View frames needs prepare_scene; one without them refuses them.
        auto table = backend.table();
        table.prepare_scene = nullptr;
        weft_renderer* refused{};
        require(weft_renderer_create(&table, nullptr, &refused) == WEFT_RENDER_INVALID_ARGUMENT,
                "a scene feature without prepare_scene");
        table.capabilities.features = WEFT_RENDER_FEATURE_IMAGE;
        weft_renderer* plain{};
        ok(weft_renderer_create(&table, nullptr, &plain), "a renderer without View frames");
        frame = page.frame(0.032);
        const weft_render_scene_frame f{sizeof(f), WEFT_RENDER_ABI_VERSION, frame, 0, 0};
        weft_render_handle none{};
        require(weft_renderer_prepare_scene(plain, &f, &none) == WEFT_RENDER_UNSUPPORTED_FEATURE,
                "View frames on a backend without them");
        weft_view_frame_release(frame);
        ok(weft_renderer_destroy(plain), "destroy");
    }

    // A frame of an unchanged revision redraws the View's kept pass; a changed one patches it where it can.
    void d3d11_kept() {
        D3D11 gpu;
        Page page;
        page.anchors(40, 30, 150, 90);
        weft::render::d3d11::ViewBackend backend(gpu.device.Get(), queue);
        weft_renderer* renderer{};
        ok(weft_renderer_create(&backend.table(), nullptr, &renderer), "renderer");
        std::uint64_t serial{};
        const auto once = [&](double time) {
            auto* frame = page.frame(time);
            const auto prepared = prepare(renderer, frame);
            weft_view_frame_release(frame);
            const auto target = gpu.target();
            ok(draw(renderer, prepared, target.rtv.Get(), serial), "draw");
            ok(weft_renderer_release_prepared(renderer, prepared), "release");
        };
        once(0.016);
        require(backend.renderer()->passes == 1 && backend.renderer()->kept_passes == 0, "the first pass prepares");
        page.anchors(100, 60, 20, 40);
        once(0.032);
        require(backend.renderer()->passes == 2 && backend.renderer()->kept_passes == 1, "moved anchors keep the pass");
        ok(weft_view_emit(page.runtime, page.view, "rename", 6, WEFT_VALUE_JSON, "\"Battle\"", 8, 0), "rename");
        once(0.048);
        require(backend.renderer()->passes == 3 && backend.renderer()->kept_passes == 2 &&
                    backend.renderer()->kept_patches == 1 && backend.renderer()->patched_records > 0,
                "a changed text patches the kept pass");
        // A second View of the runtime keeps a pass of its own; once the View has gone, the next frame's preparation
        // drops that pass and its slot.
        {
            Page second("", page.runtime);
            second.anchors(60, 40, 100, 120);
            auto* frame = second.frame(0.016);
            const auto prepared = prepare(renderer, frame);
            weft_view_frame_release(frame);
            const auto target = gpu.target();
            ok(draw(renderer, prepared, target.rtv.Get(), serial), "draw the second View");
            ok(weft_renderer_release_prepared(renderer, prepared), "release");
            require(backend.views() == 2 && backend.renderer()->kept_views() == 2, "two Views, two kept passes");
        }
        once(0.064);
        require(backend.views() == 1 && backend.renderer()->kept_views() == 1, "a View gone, its pass dropped");
        ok(weft_renderer_destroy(renderer), "destroy");
    }

    // How often the host's texture retired, and why.
    struct Retired {
        unsigned calls{};
        std::uint32_t reason{};

        static void callback(void* user, std::uint32_t reason) noexcept {
            auto& r = *static_cast<Retired*>(user);
            ++r.calls;
            r.reason = reason;
        }
    };

    // An import of a host texture of size w x h into renderer: native is its schema's payload.
    weft_render_result import(weft_renderer* renderer, const void* native, std::uint64_t native_size, unsigned w,
                              unsigned h, std::uint64_t id, std::uint32_t origin, std::uint32_t synchronization,
                              Retired* retired, weft_render_handle& out,
                              std::uint32_t format = WEFT_RENDER_TARGET_LINEAR_RGBA8, std::uint32_t levels = 0) {
        weft_render_info info{};
        info.struct_size = sizeof(info);
        ok(weft_renderer_get_info(renderer, &info), "info");
        const weft_render_external_texture d{sizeof(d),
                                             WEFT_RENDER_EXTERNAL_ABI_VERSION,
                                             info.identity,
                                             id,
                                             1,
                                             w,
                                             h,
                                             format,
                                             WEFT_RENDER_EXTERNAL_LINEAR_PREMULTIPLIED,
                                             origin,
                                             synchronization,
                                             levels,
                                             0,
                                             native,
                                             native_size,
                                             retired,
                                             retired ? &Retired::callback : nullptr};
        return weft_renderer_import_texture(renderer, &d, &out);
    }

    // A View frame with its textures bound.
    weft_render_result prepare_bound(weft_renderer* renderer, const weft_view_frame* frame,
                                     const std::vector<weft_render_scene_texture>& textures, weft_render_handle& out) {
        const weft_render_scene_frame f{sizeof(f), WEFT_RENDER_ABI_VERSION,        frame,
                                        0,         std::uint32_t(textures.size()), textures.data()};
        return weft_renderer_prepare_scene(renderer, &f, &out);
    }

    weft_render_scene_texture bound(const char* name, weft_render_handle texture) {
        return {name, std::uint32_t(std::strlen(name)), 0, texture};
    }

    // An encoded premultiplied BGRA pixel over the game's picture, as a scene pass composites it.
    std::array<std::uint8_t, 4> over_game(const std::array<std::uint8_t, 4>& linear_rgba) {
        const float a = linear_rgba[3] / 255.f;
        const float straight[3]{linear_rgba[0] / 255.f / a, linear_rgba[1] / 255.f / a, linear_rgba[2] / 255.f / a};
        const auto channel = [&](float s, std::uint8_t g) {
            return std::uint8_t(
                std::lround(std::clamp(weft::render::linear_to_srgb(s) * a + g / 255.f * (1 - a), 0.f, 1.f) * 255));
        };
        return {channel(straight[2], game[0]), channel(straight[1], game[1]), channel(straight[0], game[2]), 255};
    }

    // Whether two images differ by at most tolerance levels in every channel.
    bool within(const Pixels& a, const Pixels& b, int tolerance) {
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (std::abs(int(a[i]) - int(b[i])) > tolerance) {
                return false;
            }
        }
        return true;
    }

    bool close(const std::array<std::uint8_t, 4>& a, const std::array<std::uint8_t, 4>& b) {
        for (unsigned i = 0; i < 4; ++i) {
            if (std::abs(int(a[i]) - int(b[i])) > 1) {
                return false;
            }
        }
        return true;
    }

    // Host textures by name on the D3D11 backend: the page shows what its frame binds, as the native pass draws it
    // directly, in the page's encoding, over the background color, under the border, inside the corners; rows from the
    // bottom flip; an unbound name draws nothing; a live texture's new content shows on the kept pass; a released lease
    // lives until the frame's use completes; and what the C ABI refuses.
    // Views of other forms on the D3D11 backend (WARP): an sRGB view, a BGRA one, a texture's five levels; and what
    // it refuses: an sRGB view imported as a linear one, a view of five levels said to hold one.
    void d3d11_texture_formats() {
        D3D11 gpu;
        Page page(formats_script, formats_css);
        auto* frame = page.frame(0.016);
        weft::render::d3d11::ViewBackend backend(gpu.device.Get(), queue);
        weft_renderer* renderer{};
        ok(weft_renderer_create(&backend.table(), nullptr, &renderer), "renderer");
        const auto external = backend.external();
        for (const auto format :
             {WEFT_RENDER_TARGET_SRGB_RGBA8, WEFT_RENDER_TARGET_LINEAR_BGRA8, WEFT_RENDER_TARGET_SRGB_BGRA8}) {
            require(external.formats & (1u << format), "sRGB and BGRA views among the extension's formats");
        }
        ok(weft_renderer_install_external_backend(renderer, &external), "install the extension");
        auto srgb = gpu.made(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 2, 2, {solid_texels(2, 2, srgb_texel)});
        auto bgra = gpu.made(DXGI_FORMAT_B8G8R8A8_UNORM, 2, 2, {solid_texels(2, 2, bgra_texel)});
        auto levels = gpu.made(DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16, level_texels());
        const weft_render_d3d11_texture srgb_native{sizeof(srgb_native), WEFT_RENDER_EXTERNAL_ABI_VERSION,
                                                    srgb.srv.Get(), 0};
        const weft_render_d3d11_texture bgra_native{sizeof(bgra_native), WEFT_RENDER_EXTERNAL_ABI_VERSION,
                                                    bgra.srv.Get(), 0};
        const weft_render_d3d11_texture levels_native{sizeof(levels_native), WEFT_RENDER_EXTERNAL_ABI_VERSION,
                                                      levels.srv.Get(), 0};
        constexpr auto top = WEFT_RENDER_EXTERNAL_TOP_LEFT;
        constexpr auto ordered = WEFT_RENDER_EXTERNAL_SAME_QUEUE_ORDERED;
        weft_render_handle failed{};
        require(import(renderer, &srgb_native, sizeof(srgb_native), 2, 2, 1, top, ordered, nullptr, failed) ==
                    WEFT_RENDER_UNSUPPORTED_FEATURE,
                "an sRGB view imported as a linear one");
        require(import(renderer, &levels_native, sizeof(levels_native), 16, 16, 2, top, ordered, nullptr, failed) ==
                    WEFT_RENDER_UNSUPPORTED_FEATURE,
                "a view of five levels said to hold one");
        weft_render_handle srgb_lease{};
        weft_render_handle bgra_lease{};
        weft_render_handle levels_lease{};
        ok(import(renderer, &srgb_native, sizeof(srgb_native), 2, 2, 3, top, ordered, nullptr, srgb_lease,
                  WEFT_RENDER_TARGET_SRGB_RGBA8),
           "import the sRGB view");
        ok(import(renderer, &bgra_native, sizeof(bgra_native), 2, 2, 4, top, ordered, nullptr, bgra_lease,
                  WEFT_RENDER_TARGET_LINEAR_BGRA8),
           "import the BGRA view");
        ok(import(renderer, &levels_native, sizeof(levels_native), 16, 16, 5, top, ordered, nullptr, levels_lease,
                  WEFT_RENDER_TARGET_LINEAR_RGBA8, 5),
           "import the levels");
        weft_render_handle prepared{};
        ok(prepare_bound(renderer, frame,
                         {bound("srgb", srgb_lease), bound("bgra", bgra_lease), bound("levels", levels_lease)},
                         prepared),
           "prepare");
        std::uint64_t serial{};
        auto target = gpu.target();
        ok(draw(renderer, prepared, target.rtv.Get(), serial), "draw");
        ok(weft_renderer_release_prepared(renderer, prepared), "release");
        d3d11_formatted = gpu.read(target);
        formats_shown(d3d11_formatted);
        weft_view_frame_release(frame);
        for (const auto lease : {srgb_lease, bgra_lease, levels_lease}) {
            ok(weft_renderer_release_texture(renderer, lease), "release");
        }
        ok(weft_renderer_destroy(renderer), "destroy");
    }

    // A texture another API writes: D3D12 on the same adapter clears a texture it shares through an NT handle,
    // signalling a fence it shares too; the host opens both on the backend's D3D11 device, waits on the fence in the
    // immediate context before the UI's pass and signals it after, so that D3D12 writes the next content only after the
    // UI read the last. Imported live; each frame shows the content D3D12 wrote for it.
    void d3d11_shared_texture() {
        D3D11 gpu;
        ComPtr<ID3D11Device5> device5;
        ComPtr<ID3D11DeviceContext4> context4;
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        require(SUCCEEDED(gpu.device.As(&device5)) && SUCCEEDED(gpu.context.As(&context4)) &&
                    SUCCEEDED(gpu.device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)),
                "D3D11.4 fences on the device");
        ComPtr<ID3D12Device> d12;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12)))) {
            std::cout << "D3D12 skipped: no D3D12 device on the D3D11 device's adapter\n";
            return;
        }
        // The shared texture and fence, and what D3D12 draws with.
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 2;
        desc.Height = 2;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> shared;
        ComPtr<ID3D12Fence> fence12;
        HANDLE texture_handle{};
        HANDLE fence_handle{};
        require(SUCCEEDED(d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
                                                       D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&shared))) &&
                    SUCCEEDED(d12->CreateSharedHandle(shared.Get(), nullptr, GENERIC_ALL, nullptr, &texture_handle)) &&
                    SUCCEEDED(d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence12))) &&
                    SUCCEEDED(d12->CreateSharedHandle(fence12.Get(), nullptr, GENERIC_ALL, nullptr, &fence_handle)),
                "a shared D3D12 texture and fence");
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue12;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12DescriptorHeap> rtvs;
        D3D12_DESCRIPTOR_HEAP_DESC rtv_heap{};
        rtv_heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv_heap.NumDescriptors = 1;
        require(SUCCEEDED(d12->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue12))) &&
                    SUCCEEDED(d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
                    SUCCEEDED(d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                     IID_PPV_ARGS(&list))) &&
                    SUCCEEDED(list->Close()) && SUCCEEDED(d12->CreateDescriptorHeap(&rtv_heap, IID_PPV_ARGS(&rtvs))),
                "D3D12 commands");
        const auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
        d12->CreateRenderTargetView(shared.Get(), nullptr, rtv);
        // D3D12 clears the texture to color once the fence reaches wait, then signals done.
        const auto write = [&](std::array<float, 4> color, std::uint64_t wait, std::uint64_t done) {
            require(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(), nullptr)), "record");
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = shared.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            list->ResourceBarrier(1, &barrier);
            list->ClearRenderTargetView(rtv, color.data(), 0, nullptr);
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            list->ResourceBarrier(1, &barrier);
            require(SUCCEEDED(list->Close()), "close");
            if (wait) {
                require(SUCCEEDED(queue12->Wait(fence12.Get(), wait)), "D3D12 waits for the UI's reads");
            }
            ID3D12CommandList* lists[]{list.Get()};
            queue12->ExecuteCommandLists(1, lists);
            require(SUCCEEDED(queue12->Signal(fence12.Get(), done)), "D3D12 signals its writes");
        };
        write({1, 0, 0, 1}, 0, 1);
        // The host opens both on the D3D11 device.
        ComPtr<ID3D11Device1> device1;
        ComPtr<ID3D11Texture2D> opened;
        ComPtr<ID3D11Fence> fence11;
        ComPtr<ID3D11ShaderResourceView> view;
        require(SUCCEEDED(gpu.device.As(&device1)) &&
                    SUCCEEDED(device1->OpenSharedResource1(texture_handle, IID_PPV_ARGS(&opened))) &&
                    SUCCEEDED(device5->OpenSharedFence(fence_handle, IID_PPV_ARGS(&fence11))) &&
                    SUCCEEDED(gpu.device->CreateShaderResourceView(opened.Get(), nullptr, &view)),
                "the texture and fence opened on the D3D11 device");
        D3D11_TEXTURE2D_DESC opened_desc{};
        opened->GetDesc(&opened_desc);
        require(opened_desc.MiscFlags & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE),
                "a shared texture");
        CloseHandle(texture_handle);
        CloseHandle(fence_handle);

        Page page(textures_script, textures_css);
        weft::render::d3d11::ViewBackend backend(gpu.device.Get(), queue);
        weft_renderer* renderer{};
        ok(weft_renderer_create(&backend.table(), nullptr, &renderer), "renderer");
        const auto external = backend.external();
        ok(weft_renderer_install_external_backend(renderer, &external), "install the extension");
        const weft_render_d3d11_texture native{sizeof(native), WEFT_RENDER_EXTERNAL_ABI_VERSION, view.Get(), 0};
        weft_render_handle lease{};
        ok(import(renderer, &native, sizeof(native), 2, 2, 1, WEFT_RENDER_EXTERNAL_TOP_LEFT,
                  WEFT_RENDER_EXTERNAL_SAME_QUEUE_LIVE, nullptr, lease),
           "import the shared texture, live");
        std::uint64_t serial{};
        // A frame: the host waits for D3D12's writes, the UI draws, the host signals its reads.
        const auto drawn = [&](double time, std::uint64_t written, std::uint64_t read) {
            auto* frame = page.frame(time);
            weft_render_handle prepared{};
            ok(prepare_bound(renderer, frame, {bound("minimap", lease)}, prepared), "prepare");
            weft_view_frame_release(frame);
            auto target = gpu.target();
            require(SUCCEEDED(context4->Wait(fence11.Get(), written)), "the UI waits for D3D12's writes");
            ok(draw(renderer, prepared, target.rtv.Get(), serial), "draw");
            require(SUCCEEDED(context4->Signal(fence11.Get(), read)), "the UI signals its reads");
            ok(weft_renderer_release_prepared(renderer, prepared), "release");
            return pixel(gpu.read(target), 160, 50);
        };
        require(drawn(0.016, 1, 2) == std::array<std::uint8_t, 4>{0, 0, 255, 255}, "what D3D12 wrote: red");
        write({0, 0, 1, 1}, 2, 3);
        require(drawn(0.032, 3, 4) == std::array<std::uint8_t, 4>{255, 0, 0, 255}, "what D3D12 wrote next: blue");
        ok(weft_renderer_release_texture(renderer, lease), "release");
        ok(weft_renderer_destroy(renderer), "destroy");
        // D3D12 finishes before its objects go.
        HANDLE idle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        require(SUCCEEDED(queue12->Signal(fence12.Get(), 5)) && SUCCEEDED(fence12->SetEventOnCompletion(5, idle)) &&
                    WaitForSingleObject(idle, 5000) == WAIT_OBJECT_0,
                "D3D12 idle");
        CloseHandle(idle);
    }

    void d3d11_textures() {
        D3D11 gpu;
        Page page(textures_script, textures_css);
        auto* frame = page.frame(0.016);
        const auto held = frame->frame;
        weft::render::d3d11::ViewBackend backend(gpu.device.Get(), queue);
        weft_renderer* renderer{};
        ok(weft_renderer_create(&backend.table(), nullptr, &renderer), "renderer");
        weft_render_handle refused{};
        {
            const auto none = bound("portrait", {});
            require(prepare_bound(renderer, frame, {none}, refused) == WEFT_RENDER_UNSUPPORTED_FEATURE,
                    "textures without the extension");
        }
        const auto external = backend.external();
        require(external.formats & (1u << WEFT_RENDER_TARGET_LINEAR_RGBA8), "the extension's formats");
        ok(weft_renderer_install_external_backend(renderer, &external), "install the extension");
        std::uint64_t serial{};

        // The portrait's texture top down, the minimap's live: a solid color the host draws again between frames.
        auto portrait = gpu.texture(4, 4, quadrants(false));
        auto minimap = gpu.texture(2, 2, std::vector<std::uint8_t>(16, 0));
        const float green[]{0, 1, 0, 1};
        gpu.context->ClearRenderTargetView(minimap.rtv.Get(), green);
        Retired portrait_retired;
        Retired minimap_retired;
        const weft_render_d3d11_texture portrait_native{sizeof(portrait_native), WEFT_RENDER_EXTERNAL_ABI_VERSION,
                                                        portrait.srv.Get(), 0};
        const weft_render_d3d11_texture minimap_native{sizeof(minimap_native), WEFT_RENDER_EXTERNAL_ABI_VERSION,
                                                       minimap.srv.Get(), 0};
        weft_render_handle portrait_lease{};
        weft_render_handle minimap_lease{};
        ok(import(renderer, &portrait_native, sizeof(portrait_native), 4, 4, 1, WEFT_RENDER_EXTERNAL_TOP_LEFT,
                  WEFT_RENDER_EXTERNAL_SAME_QUEUE_ORDERED, &portrait_retired, portrait_lease),
           "import the portrait");
        ok(import(renderer, &minimap_native, sizeof(minimap_native), 2, 2, 2, WEFT_RENDER_EXTERNAL_TOP_LEFT,
                  WEFT_RENDER_EXTERNAL_SAME_QUEUE_LIVE, &minimap_retired, minimap_lease),
           "import the live minimap");
        const std::vector<weft_render_scene_texture> both{bound("portrait", portrait_lease),
                                                          bound("minimap", minimap_lease)};
        weft_render_handle prepared{};
        ok(prepare_bound(renderer, frame, both, prepared), "prepare with textures");
        auto target = gpu.target();
        ok(draw(renderer, prepared, target.rtv.Get(), serial), "draw");
        ok(weft_renderer_release_prepared(renderer, prepared), "release");
        const auto first = gpu.read(target);
        // The native pass, the same textures bound by name.
        const auto directly = [&](ID3D11ShaderResourceView* portrait_view, bool bottom_up_portrait) {
            weft::SceneText text(*held->fonts);
            weft::render::d3d11::SceneRenderer scene(gpu.device.Get(), text);
            weft::render::d3d11::SceneRenderer::Slot slot;
            const auto t = gpu.target();
            const std::vector<weft::SceneTexture> textures{
                {"portrait", std::uint64_t(reinterpret_cast<std::uintptr_t>(portrait_view)), bottom_up_portrait},
                {"minimap", std::uint64_t(reinterpret_cast<std::uintptr_t>(minimap.srv.Get())), false}};
            require(scene.paint(slot, *held->scene, t.rtv.Get(), held->scale, width, height, std::nullopt, 1, {},
                                &held->anchors->take(weft::SceneAnchorChannel::Reader::Render), {true, std::nullopt},
                                textures),
                    "the native pass");
            return gpu.read(t);
        };
        require(first == directly(portrait.srv.Get(), false), "the C ABI draws textures as the native pass");
        // The portrait's quadrants in the page's encoding, the game where its corners are round; the minimap over its
        // red background color, under its white border; nothing of the missing one.
        require(close(pixel(first, 36, 32), over_game(quadrant[0])) &&
                    close(pixel(first, 68, 32), over_game(quadrant[1])) &&
                    close(pixel(first, 36, 56), over_game(quadrant[2])) &&
                    close(pixel(first, 68, 56), over_game(quadrant[3])) && pixel(first, 21, 21) == game,
                "the portrait's quadrants, inside its corners");
        require(pixel(first, 160, 50) == std::array<std::uint8_t, 4>{0, 255, 0, 255} &&
                    pixel(first, 121, 50) == std::array<std::uint8_t, 4>{255, 255, 255, 255} &&
                    pixel(first, 240, 40) == game,
                "the minimap under its border; the missing texture draws nothing");
        require(backend.renderer()->texture_draws == 3 && backend.renderer()->unbound_textures == 1,
                "three textures drawn, one name unbound");
        d3d11_textured = first;

        // The caller's leases can go once the frame is prepared: the use pins them until it completes.
        ok(prepare_bound(renderer, frame, both, prepared), "prepare again");
        ok(weft_renderer_release_texture(renderer, portrait_lease), "release the portrait's lease");
        require(prepare_bound(renderer, frame, both, refused) == WEFT_RENDER_STALE_HANDLE, "a released lease");
        target = gpu.target();
        {
            weft_render_info info{};
            info.struct_size = sizeof(info);
            ok(weft_renderer_get_info(renderer, &info), "info");
            void* list = nullptr;
            weft_render_d3d11_pass payload{
                sizeof(payload), WEFT_RENDER_ABI_VERSION, nullptr, target.rtv.Get(), &list, 0, 0, {}};
            const weft_render_pass pass{
                sizeof(pass), WEFT_RENDER_ABI_VERSION, info.identity, width, height, WEFT_RENDER_TARGET_LINEAR_BGRA8, 1,
                &payload,     sizeof(payload)};
            weft_render_handle use{};
            ok(weft_renderer_encode(renderer, prepared, &pass, &use), "encode");
            ok(weft_renderer_release_prepared(renderer, prepared), "release");
            ok(weft_renderer_notify_submitted(renderer, use, queue, ++serial), "submitted");
            require(!portrait_retired.calls, "pinned by the submitted use");
            ok(weft_renderer_notify_completed(renderer, info.identity.device_epoch, queue, serial), "completed");
            require(portrait_retired.calls == 1 && portrait_retired.reason == WEFT_RENDER_EXTERNAL_RETIRED,
                    "retired once the use completed");
        }
        require(gpu.read(target) == first, "the same frame with the lease released");

        // Rows from the bottom: the same quadrants, upside down, as the native pass flips them.
        auto upside = gpu.texture(4, 4, quadrants(true));
        const weft_render_d3d11_texture upside_native{sizeof(upside_native), WEFT_RENDER_EXTERNAL_ABI_VERSION,
                                                      upside.srv.Get(), 0};
        weft_render_handle upside_lease{};
        ok(import(renderer, &upside_native, sizeof(upside_native), 4, 4, 3, WEFT_RENDER_EXTERNAL_BOTTOM_LEFT,
                  WEFT_RENDER_EXTERNAL_SAME_QUEUE_ORDERED, nullptr, upside_lease),
           "import rows from the bottom");
        ok(prepare_bound(renderer, frame, {bound("portrait", upside_lease), bound("minimap", minimap_lease)}, prepared),
           "prepare");
        target = gpu.target();
        ok(draw(renderer, prepared, target.rtv.Get(), serial), "draw");
        ok(weft_renderer_release_prepared(renderer, prepared), "release");
        const auto flipped = gpu.read(target);
        // Bit for bit as the native pass flips them; as the rows top down within the sampler's subtexel precision
        // (render_backend_api.md), which the sRGB encoding steepens near black.
        require(flipped == directly(upside.srv.Get(), true) && within(flipped, first, 4),
                "rows from the bottom, flipped back");

        // The live minimap drawn again by the host: the kept pass shows its new content.
        const auto kept = backend.renderer()->kept_passes;
        const float magenta[]{1, 0, 1, 1};
        gpu.context->ClearRenderTargetView(minimap.rtv.Get(), magenta);
        weft_view_frame_release(frame);
        frame = page.frame(0.032);
        ok(prepare_bound(renderer, frame, {bound("portrait", upside_lease), bound("minimap", minimap_lease)}, prepared),
           "prepare");
        target = gpu.target();
        ok(draw(renderer, prepared, target.rtv.Get(), serial), "draw");
        ok(weft_renderer_release_prepared(renderer, prepared), "release");
        const auto live = gpu.read(target);
        require(pixel(live, 160, 50) == std::array<std::uint8_t, 4>{255, 0, 255, 255} &&
                    pixel(live, 36, 32) == pixel(first, 36, 32) && backend.renderer()->kept_passes == kept + 1,
                "the live texture's new content on the kept pass");
        // On a plane, which a kept pass would keep the content image of: the texture's content of each frame.
        {
            Page plane(tilted_script, tilted_css, page.runtime);
            const auto on_plane = [&](double time) {
                auto* f = plane.frame(time);
                ok(prepare_bound(renderer, f, {bound("tilted", minimap_lease)}, prepared), "prepare the plane");
                weft_view_frame_release(f);
                const auto t = gpu.target();
                ok(draw(renderer, prepared, t.rtv.Get(), serial), "draw the plane");
                ok(weft_renderer_release_prepared(renderer, prepared), "release");
                return pixel(gpu.read(t), 52, 134);
            };
            require(on_plane(0.016) == std::array<std::uint8_t, 4>{255, 0, 255, 255}, "the texture on its plane");
            gpu.context->ClearRenderTargetView(minimap.rtv.Get(), green);
            require(on_plane(0.032) == std::array<std::uint8_t, 4>{0, 255, 0, 255},
                    "the texture's new content on its plane");
        }
        // A frame the backend refuses after the core pinned its textures: the pins go with it.
        {
            Page another(textures_script, textures_css);
            auto* theirs = another.frame(0.016);
            require(prepare_bound(renderer, theirs, {bound("minimap", minimap_lease)}, refused) ==
                        WEFT_RENDER_INVALID_ARGUMENT,
                    "a frame of another runtime, with textures");
            weft_view_frame_release(theirs);
        }

        // What the C ABI refuses: names a page cannot give or given twice, a reserved field, textures without their
        // array or in a first-version description; imports of an sRGB view said to be linear, of a texture imported
        // already, of another device's; a pass into a texture its frame binds.
        const auto refuses = [&](std::vector<weft_render_scene_texture> textures, weft_render_result expected,
                                 const char* what) {
            require(prepare_bound(renderer, frame, textures, refused) == expected, what);
        };
        refuses({bound("portrait", upside_lease), bound("portrait", minimap_lease)}, WEFT_RENDER_INVALID_ARGUMENT,
                "a name bound twice");
        refuses({bound("a.b", upside_lease)}, WEFT_RENDER_INVALID_ARGUMENT, "a name with a dot");
        refuses({bound("", upside_lease)}, WEFT_RENDER_INVALID_ARGUMENT, "an empty name");
        const std::string long_name(65, 'a');
        refuses({bound(long_name.c_str(), upside_lease)}, WEFT_RENDER_INVALID_ARGUMENT, "a name of 65 bytes");
        auto reserved = bound("portrait", upside_lease);
        reserved.reserved = 1;
        refuses({reserved}, WEFT_RENDER_INVALID_ARGUMENT, "a binding's reserved field");
        {
            weft_render_scene_frame f{sizeof(f), WEFT_RENDER_ABI_VERSION, frame, 0, 1, nullptr};
            require(weft_renderer_prepare_scene(renderer, &f, &refused) == WEFT_RENDER_INVALID_ARGUMENT,
                    "textures without their array");
            const auto one = bound("portrait", upside_lease);
            f.textures = &one;
            f.struct_size = std::uint32_t(offsetof(weft_render_scene_frame, textures));
            require(weft_renderer_prepare_scene(renderer, &f, &refused) == WEFT_RENDER_INVALID_ARGUMENT,
                    "textures in a first-version description");
            f.texture_count = 0;
            ok(weft_renderer_prepare_scene(renderer, &f, &prepared), "a first-version description");
            ok(weft_renderer_release_prepared(renderer, prepared), "release");
        }
        auto srgb = gpu.texture(2, 2, std::vector<std::uint8_t>(16, 255), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        const weft_render_d3d11_texture srgb_native{sizeof(srgb_native), WEFT_RENDER_EXTERNAL_ABI_VERSION,
                                                    srgb.srv.Get(), 0};
        weft_render_handle failed{};
        require(import(renderer, &srgb_native, sizeof(srgb_native), 2, 2, 4, WEFT_RENDER_EXTERNAL_TOP_LEFT,
                       WEFT_RENDER_EXTERNAL_SAME_QUEUE_ORDERED, nullptr, failed) == WEFT_RENDER_UNSUPPORTED_FEATURE,
                "an sRGB view said to be linear");
        require(import(renderer, &minimap_native, sizeof(minimap_native), 2, 2, 5, WEFT_RENDER_EXTERNAL_TOP_LEFT,
                       WEFT_RENDER_EXTERNAL_SAME_QUEUE_LIVE, nullptr, failed) == WEFT_RENDER_INVALID_STATE,
                "a texture imported already");
        D3D11 other;
        auto foreign = other.texture(2, 2, std::vector<std::uint8_t>(16, 255));
        const weft_render_d3d11_texture foreign_native{sizeof(foreign_native), WEFT_RENDER_EXTERNAL_ABI_VERSION,
                                                       foreign.srv.Get(), 0};
        require(import(renderer, &foreign_native, sizeof(foreign_native), 2, 2, 6, WEFT_RENDER_EXTERNAL_TOP_LEFT,
                       WEFT_RENDER_EXTERNAL_SAME_QUEUE_ORDERED, nullptr, failed) == WEFT_RENDER_WRONG_BACKEND,
                "another device's texture");
        auto canvas = gpu.texture(width, height, std::vector<std::uint8_t>(std::size_t(width) * height * 4, 0));
        const weft_render_d3d11_texture canvas_native{sizeof(canvas_native), WEFT_RENDER_EXTERNAL_ABI_VERSION,
                                                      canvas.srv.Get(), 0};
        weft_render_handle canvas_lease{};
        ok(import(renderer, &canvas_native, sizeof(canvas_native), width, height, 7, WEFT_RENDER_EXTERNAL_TOP_LEFT,
                  WEFT_RENDER_EXTERNAL_SAME_QUEUE_LIVE, nullptr, canvas_lease),
           "import a texture of the target's size");
        ok(prepare_bound(renderer, frame, {bound("minimap", canvas_lease)}, prepared), "prepare");
        require(draw(renderer, prepared, canvas.rtv.Get(), serial, WEFT_RENDER_TARGET_LINEAR_RGBA8) ==
                    WEFT_RENDER_INVALID_STATE,
                "a pass into a texture its frame binds");
        ok(weft_renderer_release_prepared(renderer, prepared), "release");
        ok(weft_renderer_release_texture(renderer, canvas_lease), "release");
        weft_view_frame_release(frame);
        ok(weft_renderer_release_texture(renderer, upside_lease), "release");
        ok(weft_renderer_release_texture(renderer, minimap_lease), "release");
        require(minimap_retired.calls == 1, "the live texture retired");
        ok(weft_renderer_destroy(renderer), "destroy");
    }

#ifdef WEFT_TEST_VULKAN
    namespace vk = weft::render::vulkan;

    std::uint64_t u64(const void* handle) {
        return std::uint64_t(reinterpret_cast<std::uintptr_t>(handle));
    }

    // A host on this machine's Vulkan device, made for the test as an engine makes its own: a target of the game's
    // picture, the commands that fill it, a pass over it and its readback, one submission.
    struct VulkanHost {
        std::unique_ptr<vk::Device> device;
        VkCommandPool pool{};
        VkCommandBuffer command{};
        VkFence fence{};
        vk::Image target;
        vk::Buffer readback;

        explicit VulkanHost(std::unique_ptr<vk::Device> d) : device(std::move(d)) {
            auto& v = *device;
            VkCommandPoolCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            info.queueFamilyIndex = v.queue_family;
            vk::check(v.vkd.vkCreateCommandPool(v.device, &info, nullptr, &pool), "vkCreateCommandPool");
            VkCommandBufferAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocate.commandPool = pool;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            vk::check(v.vkd.vkAllocateCommandBuffers(v.device, &allocate, &command), "vkAllocateCommandBuffers");
            VkFenceCreateInfo fence_info{};
            fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            vk::check(v.vkd.vkCreateFence(v.device, &fence_info, nullptr, &fence), "vkCreateFence");
            target = v.image(VK_FORMAT_B8G8R8A8_UNORM, width, height,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            readback = v.buffer(VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        }

        ~VulkanHost() {
            auto& v = *device;
            v.vkd.vkDeviceWaitIdle(v.device);
            v.destroy(target);
            v.destroy(readback);
            v.vkd.vkDestroyFence(v.device, fence, nullptr);
            v.vkd.vkDestroyCommandPool(v.device, pool, nullptr);
        }

        // One submission: the game's picture into the target, record(command) over it, the readback.
        template <class Record, class Submitted>
        Pixels frame(Record&& record, Submitted&& submitted) {
            auto& v = *device;
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vk::check(v.vkd.vkBeginCommandBuffer(command, &begin), "begin");
            vk::transition(v, command, target.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                           VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT);
            // A clear value is RGBA whatever the format's byte order.
            const VkClearColorValue picture{{game[2] / 255.f, game[1] / 255.f, game[0] / 255.f, game[3] / 255.f}};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            v.vkd.vkCmdClearColorImage(command, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &picture, 1,
                                       &range);
            vk::transition(v, command, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            record(command);
            vk::transition(v, command, target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {width, height, 1};
            v.vkd.vkCmdCopyImageToBuffer(command, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer,
                                         1, &copy);
            VkBufferMemoryBarrier host{};
            host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host.buffer = readback.buffer;
            host.size = VK_WHOLE_SIZE;
            v.vkd.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                                       nullptr, 1, &host, 0, nullptr);
            vk::check(v.vkd.vkEndCommandBuffer(command), "end");
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &command;
            vk::check(v.vkd.vkQueueSubmit(v.queue, 1, &submit, fence), "submit");
            submitted(false);
            v.wait(fence);
            vk::check(v.vkd.vkResetFences(v.device, 1, &fence), "fence reset");
            submitted(true);
            Pixels out(std::size_t(width) * height * 4);
            std::memcpy(out.data(), readback.mapped, out.size());
            return out;
        }
    };

    void vulkan_frames() {
        std::string unavailable;
        auto device = vk::Device::open({}, unavailable);
        if (!device) {
            std::cout << "Vulkan skipped: " << unavailable << '\n';
            return;
        }
        VulkanHost host(std::move(device));
        auto& d = *host.device;
        Page page;
        page.anchors(40, 30, 150, 90);
        auto* frame = page.frame(0.016);
        const auto held = frame->frame;
        constexpr std::uint64_t identity = 7;
        const weft_render_vulkan_desc desc{sizeof(desc),
                                           WEFT_RENDER_ABI_VERSION,
                                           u64(d.instance),
                                           u64(d.physical),
                                           u64(d.device),
                                           u64(d.queue),
                                           d.queue_family,
                                           0,
                                           reinterpret_cast<void*>(d.get_instance_proc),
                                           identity};
        weft_render_vulkan* backend{};
        weft_render_backend table{};
        ok(weft_render_vulkan_create(&desc, &backend, &table), "the Vulkan backend");
        // Up to four recordings that are not submitted yet.
        const weft_renderer_desc limits{sizeof(limits), WEFT_RENDER_ABI_VERSION, 4, 4, 64, 65536, 512, 0, 16u << 20};
        weft_renderer* renderer{};
        ok(weft_renderer_create(&table, &limits, &renderer), "renderer");
        weft_render_info info{};
        info.struct_size = sizeof(info);
        ok(weft_renderer_get_info(renderer, &info), "info");
        std::uint64_t serial{};
        // Through the C ABI.
        const auto draw_with = [&](weft_renderer* r, weft_view_frame* f, const float* clear, float scale = 0,
                                   std::uint32_t size = sizeof(weft_render_vulkan_pass)) {
            weft_render_info i{};
            i.struct_size = sizeof(i);
            ok(weft_renderer_get_info(r, &i), "info");
            const auto prepared = prepare(r, f, scale);
            weft_render_handle use{};
            auto pixels = host.frame(
                [&](VkCommandBuffer command) {
                    weft_render_vulkan_pass payload{sizeof(payload),
                                                    WEFT_RENDER_ABI_VERSION,
                                                    u64(d.device),
                                                    u64(command),
                                                    u64(host.target.view),
                                                    0,
                                                    0,
                                                    {},
                                                    u64(host.target.image)};
                    if (clear) {
                        payload.clear_target = 1;
                        std::copy(clear, clear + 4, payload.clear_linear_premultiplied);
                    }
                    payload.struct_size = size;
                    const weft_render_pass pass{sizeof(pass), WEFT_RENDER_ABI_VERSION,         i.identity, width,
                                                height,       WEFT_RENDER_TARGET_LINEAR_BGRA8, 1,          &payload,
                                                size};
                    ok(weft_renderer_encode(r, prepared, &pass, &use), "encode");
                },
                [&](bool done) {
                    if (!done) {
                        ok(weft_renderer_notify_submitted(r, use, identity, ++serial), "submitted");
                    } else {
                        ok(weft_renderer_notify_completed(r, i.identity.device_epoch, identity, serial), "completed");
                    }
                });
            ok(weft_renderer_release_prepared(r, prepared), "release");
            return pixels;
        };
        const auto through_abi = [&](weft_view_frame* f, const float* clear) {
            return draw_with(renderer, f, clear);
        };
        // The native pass directly, on the same device.
        weft::SceneText text(*held->fonts);
        vk::SceneRenderer scene(d, text);
        vk::SceneRenderer::Slot slot;
        const auto directly = [&](bool over = true, unsigned index = 1, float scale = 0) {
            return host.frame(
                [&](VkCommandBuffer command) {
                    require(scene.encode(
                                slot, *held->scene, command, {host.target.view, width, height, host.target.image},
                                scale ? scale : held->scale, std::nullopt, index, {},
                                &held->anchors->take(weft::SceneAnchorChannel::Reader::Render), {over, std::nullopt}),
                            "the native pass");
                },
                [&](bool done) {
                    if (!done) {
                        scene.submitted(slot);
                    } else {
                        scene.retire(slot);
                    }
                });
        };
        const auto through = through_abi(frame, nullptr);
        weft_view_frame_release(frame);
        require(through == directly(), "the C ABI records as the native pass");
        require(pixel(through, 2, 2) == game && pixel(through, 15, 22) != game, "the page over the game");
        require(darker(pixel(through, 280, 30)), "the glass darkens the game beneath it");
        const float blue[]{0.05f, 0.1f, 0.4f, 1};
        frame = page.frame(0.020);
        const auto cleared = through_abi(frame, blue);
        weft_view_frame_release(frame);
        require(pixel(cleared, 2, 2) == encoded_bgra(blue), "the host's clear");
        page.anchors(200, 120, 60, 150);
        frame = page.frame(0.032);
        const auto moved = through_abi(frame, nullptr);
        weft_view_frame_release(frame);
        require(moved == directly() && moved != through, "drawn where the anchors are now");
        // A renderer's kept pass replacing its target is not the one drawn over it.
        require(pixel(directly(false, 2), 2, 2) != game && directly(true, 2) == moved,
                "over after replacing, as over alone");
        // At half the View's scale: the page in the target's top-left quarter.
        frame = page.frame(0.034);
        const auto half = draw_with(renderer, frame, nullptr, 0.5f);
        weft_view_frame_release(frame);
        require(half == directly(true, 1, 0.5f) && half != moved, "a scale of the host's");
        // A payload of the first version, which ends before target_image, draws as before: the glass sees nothing
        // beneath it (whatever follows the payload in memory).
        frame = page.frame(0.035);
        const auto first_version =
            draw_with(renderer, frame, nullptr, 0, std::uint32_t(offsetof(weft_render_vulkan_pass, target_image)));
        weft_view_frame_release(frame);
        bool same_outside_glass = true;
        for (unsigned y = 0; y < height; ++y) {
            for (unsigned x = 0; x < width; ++x) {
                if ((x < 250 || x >= 310 || y < 10 || y >= 50) && pixel(first_version, x, y) != pixel(moved, x, y)) {
                    same_outside_glass = false;
                }
            }
        }
        require(same_outside_glass && pixel(first_version, 280, 30) == game && darker(pixel(moved, 280, 30)),
                "a first-version payload, without the target's image");

        // A recording the host discards gives back its slot; the next records as before.
        {
            frame = page.frame(0.036);
            const auto prepared = prepare(renderer, frame);
            weft_view_frame_release(frame);
            auto& v = *host.device;
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vk::check(v.vkd.vkBeginCommandBuffer(host.command, &begin), "begin");
            weft_render_vulkan_pass payload{sizeof(payload),
                                            WEFT_RENDER_ABI_VERSION,
                                            u64(d.device),
                                            u64(host.command),
                                            u64(host.target.view),
                                            0,
                                            0,
                                            {},
                                            u64(host.target.image)};
            const weft_render_pass pass{
                sizeof(pass), WEFT_RENDER_ABI_VERSION, info.identity, width, height, WEFT_RENDER_TARGET_LINEAR_BGRA8, 1,
                &payload,     sizeof(payload)};
            weft_render_handle use{};
            ok(weft_renderer_encode(renderer, prepared, &pass, &use), "encode to discard");
            vk::check(v.vkd.vkEndCommandBuffer(host.command), "end");
            vk::check(v.vkd.vkResetCommandBuffer(host.command, 0), "reset");
            ok(weft_renderer_discard_encoded(renderer, use), "discard");
            ok(weft_renderer_release_prepared(renderer, prepared), "release");
            frame = page.frame(0.040);
            require(through_abi(frame, nullptr) == moved, "after a discarded recording");
            weft_view_frame_release(frame);
        }

        // Two frames in flight: the earlier one's completion keeps the later recording, which is not submitted yet.
        {
            auto& v = *host.device;
            VkCommandBuffer second{};
            VkCommandBufferAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocate.commandPool = host.pool;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            vk::check(v.vkd.vkAllocateCommandBuffers(v.device, &allocate, &second), "second command buffer");
            VkFence second_fence{};
            VkFenceCreateInfo fence_info{};
            fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            vk::check(v.vkd.vkCreateFence(v.device, &fence_info, nullptr, &second_fence), "fence");
            const auto record = [&](VkCommandBuffer command, weft_render_handle& use) {
                auto* f = page.frame(0.044);
                const auto prepared = prepare(renderer, f);
                weft_view_frame_release(f);
                VkCommandBufferBeginInfo begin{};
                begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vk::check(v.vkd.vkBeginCommandBuffer(command, &begin), "begin");
                vk::transition(
                    v, command, host.target.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    0, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                weft_render_vulkan_pass payload{
                    sizeof(payload), WEFT_RENDER_ABI_VERSION, u64(d.device), u64(command), u64(host.target.view), 1, 0,
                    {0, 0, 0, 1},    u64(host.target.image)};
                const weft_render_pass pass{sizeof(pass),
                                            WEFT_RENDER_ABI_VERSION,
                                            info.identity,
                                            width,
                                            height,
                                            WEFT_RENDER_TARGET_LINEAR_BGRA8,
                                            1,
                                            &payload,
                                            sizeof(payload)};
                ok(weft_renderer_encode(renderer, prepared, &pass, &use), "encode in flight");
                ok(weft_renderer_release_prepared(renderer, prepared), "release");
                vk::check(v.vkd.vkEndCommandBuffer(command), "end");
            };
            const auto submit = [&](VkCommandBuffer command, VkFence fence, weft_render_handle use) {
                VkSubmitInfo info_submit{};
                info_submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                info_submit.commandBufferCount = 1;
                info_submit.pCommandBuffers = &command;
                vk::check(v.vkd.vkQueueSubmit(v.queue, 1, &info_submit, fence), "submit");
                ok(weft_renderer_notify_submitted(renderer, use, identity, ++serial), "submitted");
            };
            weft_render_handle first{};
            weft_render_handle later{};
            record(host.command, first);
            submit(host.command, host.fence, first);
            const auto first_serial = serial;
            record(second, later);
            v.wait(host.fence);
            vk::check(v.vkd.vkResetFences(v.device, 1, &host.fence), "reset");
            ok(weft_renderer_notify_completed(renderer, info.identity.device_epoch, identity, first_serial),
               "the first completed");
            // Two more recordings while the later one waits for its submission: neither takes its slot.
            {
                VkCommandBuffer third{};
                vk::check(v.vkd.vkAllocateCommandBuffers(v.device, &allocate, &third), "third command buffer");
                weft_render_handle more[2]{};
                record(host.command, more[0]);
                record(third, more[1]);
                for (const auto use : more) {
                    ok(weft_renderer_discard_encoded(renderer, use), "discard");
                }
                vk::check(v.vkd.vkResetCommandBuffer(host.command, 0), "reset");
                vk::check(v.vkd.vkResetCommandBuffer(third, 0), "reset");
                v.vkd.vkFreeCommandBuffers(v.device, host.pool, 1, &third);
            }
            submit(second, second_fence, later);
            v.wait(second_fence);
            ok(weft_renderer_notify_completed(renderer, info.identity.device_epoch, identity, serial),
               "the later completed");
            v.vkd.vkDestroyFence(v.device, second_fence, nullptr);
            v.vkd.vkFreeCommandBuffers(v.device, host.pool, 1, &second);
        }

        // What is not this backend's.
        {
            frame = page.frame(0.048);
            const auto prepared = prepare(renderer, frame);
            weft_view_frame_release(frame);
            const auto encode = [&](std::uint64_t device, const float* clear, std::uint32_t size,
                                    std::uint32_t payload_size) {
                weft_render_vulkan_pass payload{sizeof(payload),
                                                WEFT_RENDER_ABI_VERSION,
                                                device,
                                                u64(host.command),
                                                u64(host.target.view),
                                                0,
                                                0,
                                                {},
                                                u64(host.target.image)};
                if (clear) {
                    payload.clear_target = 1;
                    std::copy(clear, clear + 4, payload.clear_linear_premultiplied);
                }
                payload.struct_size = size;
                const weft_render_pass pass{sizeof(pass),
                                            WEFT_RENDER_ABI_VERSION,
                                            info.identity,
                                            width,
                                            height,
                                            WEFT_RENDER_TARGET_LINEAR_BGRA8,
                                            1,
                                            &payload,
                                            payload_size};
                weft_render_handle use{};
                return weft_renderer_encode(renderer, prepared, &pass, &use);
            };
            constexpr auto full = std::uint32_t(sizeof(weft_render_vulkan_pass));
            constexpr auto short_of_v1 = std::uint32_t(offsetof(weft_render_vulkan_pass, target_image)) - 8;
            require(encode(u64(d.device) + 64, nullptr, full, full) == WEFT_RENDER_WRONG_BACKEND,
                    "a command buffer of another device");
            const float straight[]{0.5f, 0, 0, 0.25f};
            require(encode(u64(d.device), straight, full, full) == WEFT_RENDER_INVALID_ARGUMENT,
                    "a clear color that is not premultiplied");
            require(encode(u64(d.device), nullptr, short_of_v1, full) == WEFT_RENDER_INVALID_ARGUMENT,
                    "a pass that says it is shorter than its first version");
            require(encode(u64(d.device), nullptr, full, short_of_v1) == WEFT_RENDER_INVALID_ARGUMENT,
                    "a payload shorter than the pass's first version");
            ok(weft_renderer_release_prepared(renderer, prepared), "release");
        }

        // A second View of the runtime, on a backend of the same device: once the View has gone, the next frame's
        // preparation drops its pass.
        {
            vk::ViewBackend views({d.instance, d.physical, d.device, d.queue, d.queue_family, d.get_instance_proc},
                                  identity);
            weft_renderer* second_renderer{};
            ok(weft_renderer_create(&views.table(), nullptr, &second_renderer), "a renderer of two Views");
            {
                Page second("", page.runtime);
                second.anchors(60, 40, 100, 120);
                frame = page.frame(0.052);
                draw_with(second_renderer, frame, nullptr);
                weft_view_frame_release(frame);
                frame = second.frame(0.016);
                draw_with(second_renderer, frame, nullptr);
                weft_view_frame_release(frame);
                require(views.views() == 2 && views.renderer()->kept_views() == 2, "two Views, two kept passes");
            }
            frame = page.frame(0.056);
            draw_with(second_renderer, frame, nullptr);
            weft_view_frame_release(frame);
            require(views.views() == 1 && views.renderer()->kept_views() == 1, "a View gone, its pass dropped");
            ok(weft_renderer_destroy(second_renderer), "destroy");
        }
        ok(weft_renderer_destroy(renderer), "destroy");
        weft_render_vulkan_destroy(backend);
        require(d.validation_messages == 0, "validation: " + d.first_validation_message);
        weft_render_vulkan_desc bad = desc;
        bad.device = 0;
        require(weft_render_vulkan_create(&bad, &backend, &table) == WEFT_RENDER_INVALID_ARGUMENT && !backend,
                "no device");
    }

    // Host textures by name on the Vulkan backend: the host writes its textures in the frame's command buffer before
    // the pass, which records them as the native pass does directly: the quadrants in the page's encoding, the minimap
    // under its border, nothing for a missing name, a live texture's new content on the kept pass; no validation
    // message.
    void vulkan_textures() {
        std::string unavailable;
        auto device = vk::Device::open({}, unavailable);
        if (!device) {
            std::cout << "Vulkan skipped: " << unavailable << '\n';
            return;
        }
        VulkanHost host(std::move(device));
        auto& d = *host.device;
        Page page(textures_script, textures_css);
        auto* frame = page.frame(0.016);
        const auto held = frame->frame;
        constexpr std::uint64_t identity = 9;
        vk::ViewBackend backend({d.instance, d.physical, d.device, d.queue, d.queue_family, d.get_instance_proc},
                                identity);
        weft_renderer* renderer{};
        ok(weft_renderer_create(&backend.table(), nullptr, &renderer), "renderer");
        const auto external = backend.external();
        ok(weft_renderer_install_external_backend(renderer, &external), "install the extension");
        weft_render_info info{};
        info.struct_size = sizeof(info);
        ok(weft_renderer_get_info(renderer, &info), "info");

        // The host's textures and what it writes into them: the portrait's quadrants, the minimap's solid color.
        const auto make = [&](unsigned w, unsigned h) {
            return d.image(VK_FORMAT_R8G8B8A8_UNORM, w, h,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        };
        auto portrait = make(4, 4);
        auto minimap = make(2, 2);
        auto portrait_pixels = d.buffer(64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        auto minimap_pixels = d.buffer(16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        const auto quads = quadrants(false);
        std::memcpy(portrait_pixels.mapped, quads.data(), quads.size());
        const auto solid = [&](std::array<std::uint8_t, 4> rgba) {
            for (unsigned i = 0; i < 4; ++i) {
                std::memcpy(static_cast<std::uint8_t*>(minimap_pixels.mapped) + 4 * i, rgba.data(), 4);
            }
        };
        solid({0, 255, 0, 255});
        // The host's writes, in the frame's commands before the pass: copies, then shader-readable for the fragment
        // stage.
        const auto write = [&](VkCommandBuffer command, vk::Image& image, vk::Buffer& pixels) {
            vk::transition(d, command, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                           VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {image.width, image.height, 1};
            d.vkd.vkCmdCopyBufferToImage(command, pixels.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                         &copy);
            vk::transition(d, command, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        };
        Retired portrait_retired;
        Retired minimap_retired;
        const weft_render_vulkan_texture portrait_native{sizeof(portrait_native), WEFT_RENDER_EXTERNAL_ABI_VERSION,
                                                         u64(d.device), u64(portrait.view)};
        const weft_render_vulkan_texture minimap_native{sizeof(minimap_native), WEFT_RENDER_EXTERNAL_ABI_VERSION,
                                                        u64(d.device), u64(minimap.view)};
        weft_render_handle portrait_lease{};
        weft_render_handle minimap_lease{};
        ok(import(renderer, &portrait_native, sizeof(portrait_native), 4, 4, 1, WEFT_RENDER_EXTERNAL_TOP_LEFT,
                  WEFT_RENDER_EXTERNAL_SAME_QUEUE_ORDERED, &portrait_retired, portrait_lease),
           "import the portrait");
        ok(import(renderer, &minimap_native, sizeof(minimap_native), 2, 2, 2, WEFT_RENDER_EXTERNAL_TOP_LEFT,
                  WEFT_RENDER_EXTERNAL_SAME_QUEUE_LIVE, &minimap_retired, minimap_lease),
           "import the live minimap");
        weft_render_handle failed{};
        require(import(renderer, &minimap_native, sizeof(minimap_native), 2, 2, 3, WEFT_RENDER_EXTERNAL_TOP_LEFT,
                       WEFT_RENDER_EXTERNAL_SAME_QUEUE_LIVE, nullptr, failed) == WEFT_RENDER_INVALID_STATE,
                "a view imported already");
        const weft_render_vulkan_texture foreign{sizeof(foreign), WEFT_RENDER_EXTERNAL_ABI_VERSION, u64(d.device) + 64,
                                                 u64(minimap.view)};
        require(import(renderer, &foreign, sizeof(foreign), 2, 2, 4, WEFT_RENDER_EXTERNAL_TOP_LEFT,
                       WEFT_RENDER_EXTERNAL_SAME_QUEUE_ORDERED, nullptr, failed) == WEFT_RENDER_WRONG_BACKEND,
                "another device's view");
        std::uint64_t serial{};
        // A frame through the C ABI: the host's writes (both textures, or the minimap alone), then the pass.
        const auto through_abi = [&](weft_view_frame* f, bool both) {
            weft_render_handle prepared{};
            ok(prepare_bound(renderer, f, {bound("portrait", portrait_lease), bound("minimap", minimap_lease)},
                             prepared),
               "prepare with textures");
            weft_render_handle use{};
            auto pixels = host.frame(
                [&](VkCommandBuffer command) {
                    if (both) {
                        write(command, portrait, portrait_pixels);
                    }
                    write(command, minimap, minimap_pixels);
                    weft_render_vulkan_pass payload{sizeof(payload),
                                                    WEFT_RENDER_ABI_VERSION,
                                                    u64(d.device),
                                                    u64(command),
                                                    u64(host.target.view),
                                                    0,
                                                    0,
                                                    {},
                                                    u64(host.target.image)};
                    const weft_render_pass pass{sizeof(pass),
                                                WEFT_RENDER_ABI_VERSION,
                                                info.identity,
                                                width,
                                                height,
                                                WEFT_RENDER_TARGET_LINEAR_BGRA8,
                                                1,
                                                &payload,
                                                sizeof(payload)};
                    ok(weft_renderer_encode(renderer, prepared, &pass, &use), "encode");
                },
                [&](bool done) {
                    if (!done) {
                        ok(weft_renderer_notify_submitted(renderer, use, identity, ++serial), "submitted");
                    } else {
                        ok(weft_renderer_notify_completed(renderer, info.identity.device_epoch, identity, serial),
                           "completed");
                    }
                });
            ok(weft_renderer_release_prepared(renderer, prepared), "release");
            return pixels;
        };
        // The native pass directly, the same textures bound by name, as they hold now.
        weft::SceneText text(*held->fonts);
        vk::SceneRenderer scene(d, text);
        vk::SceneRenderer::Slot slot;
        const std::vector<weft::SceneTexture> textures{{"portrait", u64(portrait.view), false},
                                                       {"minimap", u64(minimap.view), false}};
        const auto directly = [&] {
            return host.frame(
                [&](VkCommandBuffer command) {
                    require(scene.encode(slot, *held->scene, command,
                                         {host.target.view, width, height, host.target.image}, held->scale,
                                         std::nullopt, 1, {},
                                         &held->anchors->take(weft::SceneAnchorChannel::Reader::Render),
                                         {true, std::nullopt}, textures),
                            "the native pass");
                },
                [&](bool done) {
                    if (!done) {
                        scene.submitted(slot);
                    } else {
                        scene.retire(slot);
                    }
                });
        };
        const auto first = through_abi(frame, true);
        weft_view_frame_release(frame);
        require(first == directly(), "the C ABI records textures as the native pass");
        // The same page as D3D11 WARP draws it, the laid out texture's tiles too: within 2 levels but where the device
        // filters texels of high contrast with its own subtexel weights (8 bits on the hardware), which the encoding
        // of dark texels magnifies.
        if (!d3d11_textured.empty()) {
            int worst{};
            std::size_t at{};
            std::size_t apart{};
            for (std::size_t k = 0; k < first.size(); ++k) {
                const int difference = std::abs(int(first[k]) - int(d3d11_textured[k]));
                apart += difference > 2;
                if (difference > worst) {
                    worst = difference;
                    at = k / 4;
                }
            }
            require(worst <= 8 && apart <= first.size() / 200,
                    "Vulkan draws the textures as D3D11 WARP does, worst " + std::to_string(worst) + " at " +
                        std::to_string(at % width) + ", " + std::to_string(at / width) + "; " + std::to_string(apart) +
                        " channels more than 2 apart");
        }
        require(close(pixel(first, 36, 32), over_game(quadrant[0])) &&
                    close(pixel(first, 68, 32), over_game(quadrant[1])) &&
                    close(pixel(first, 36, 56), over_game(quadrant[2])) &&
                    close(pixel(first, 68, 56), over_game(quadrant[3])) && pixel(first, 21, 21) == game,
                "the portrait's quadrants, inside its corners");
        require(pixel(first, 160, 50) == std::array<std::uint8_t, 4>{0, 255, 0, 255} &&
                    pixel(first, 121, 50) == std::array<std::uint8_t, 4>{255, 255, 255, 255} &&
                    pixel(first, 240, 40) == game,
                "the minimap under its border; the missing texture draws nothing");
        // The live minimap written again: the kept pass shows its new content.
        const auto kept = backend.renderer()->kept_passes;
        solid({255, 0, 255, 255});
        frame = page.frame(0.032);
        const auto live = through_abi(frame, false);
        weft_view_frame_release(frame);
        require(pixel(live, 160, 50) == std::array<std::uint8_t, 4>{255, 0, 255, 255} &&
                    pixel(live, 36, 32) == pixel(first, 36, 32) && backend.renderer()->kept_passes == kept + 1,
                "the live texture's new content on the kept pass");
        // Rows from the bottom: the same quadrants upside down, flipped back as the native pass flips them.
        {
            auto upside = make(4, 4);
            auto upside_pixels = d.buffer(64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
            const auto bottom_up = quadrants(true);
            std::memcpy(upside_pixels.mapped, bottom_up.data(), bottom_up.size());
            const weft_render_vulkan_texture upside_native{sizeof(upside_native), WEFT_RENDER_EXTERNAL_ABI_VERSION,
                                                           u64(d.device), u64(upside.view)};
            weft_render_handle upside_lease{};
            ok(import(renderer, &upside_native, sizeof(upside_native), 4, 4, 5, WEFT_RENDER_EXTERNAL_BOTTOM_LEFT,
                      WEFT_RENDER_EXTERNAL_SAME_QUEUE_ORDERED, nullptr, upside_lease),
               "import rows from the bottom");
            frame = page.frame(0.040);
            weft_render_handle prepared{};
            ok(prepare_bound(renderer, frame, {bound("portrait", upside_lease), bound("minimap", minimap_lease)},
                             prepared),
               "prepare");
            weft_view_frame_release(frame);
            weft_render_handle use{};
            const auto flipped = host.frame(
                [&](VkCommandBuffer command) {
                    write(command, upside, upside_pixels);
                    weft_render_vulkan_pass payload{sizeof(payload),
                                                    WEFT_RENDER_ABI_VERSION,
                                                    u64(d.device),
                                                    u64(command),
                                                    u64(host.target.view),
                                                    0,
                                                    0,
                                                    {},
                                                    u64(host.target.image)};
                    const weft_render_pass pass{sizeof(pass),
                                                WEFT_RENDER_ABI_VERSION,
                                                info.identity,
                                                width,
                                                height,
                                                WEFT_RENDER_TARGET_LINEAR_BGRA8,
                                                1,
                                                &payload,
                                                sizeof(payload)};
                    ok(weft_renderer_encode(renderer, prepared, &pass, &use), "encode");
                },
                [&](bool done) {
                    if (!done) {
                        ok(weft_renderer_notify_submitted(renderer, use, identity, ++serial), "submitted");
                    } else {
                        ok(weft_renderer_notify_completed(renderer, info.identity.device_epoch, identity, serial),
                           "completed");
                    }
                });
            ok(weft_renderer_release_prepared(renderer, prepared), "release");
            const std::vector<weft::SceneTexture> upside_textures{{"portrait", u64(upside.view), true},
                                                                  {"minimap", u64(minimap.view), false}};
            const auto natively = host.frame(
                [&](VkCommandBuffer command) {
                    require(scene.encode(slot, *held->scene, command,
                                         {host.target.view, width, height, host.target.image}, held->scale,
                                         std::nullopt, 2, {},
                                         &held->anchors->take(weft::SceneAnchorChannel::Reader::Render),
                                         {true, std::nullopt}, upside_textures),
                            "the native pass");
                },
                [&](bool done) {
                    if (!done) {
                        scene.submitted(slot);
                    } else {
                        scene.retire(slot);
                    }
                });
            require(flipped == natively && within(flipped, live, 4), "rows from the bottom, flipped back");
            ok(weft_renderer_release_texture(renderer, upside_lease), "release");
            d.destroy(upside);
            d.destroy(upside_pixels);
        }
        // On a plane: the texture's content of each frame.
        {
            Page plane(tilted_script, tilted_css, page.runtime);
            const auto on_plane = [&](double time) {
                auto* f = plane.frame(time);
                weft_render_handle prepared{};
                ok(prepare_bound(renderer, f, {bound("tilted", minimap_lease)}, prepared), "prepare the plane");
                weft_view_frame_release(f);
                weft_render_handle use{};
                const auto pixels = host.frame(
                    [&](VkCommandBuffer command) {
                        write(command, minimap, minimap_pixels);
                        weft_render_vulkan_pass payload{sizeof(payload),
                                                        WEFT_RENDER_ABI_VERSION,
                                                        u64(d.device),
                                                        u64(command),
                                                        u64(host.target.view),
                                                        0,
                                                        0,
                                                        {},
                                                        u64(host.target.image)};
                        const weft_render_pass pass{sizeof(pass),
                                                    WEFT_RENDER_ABI_VERSION,
                                                    info.identity,
                                                    width,
                                                    height,
                                                    WEFT_RENDER_TARGET_LINEAR_BGRA8,
                                                    1,
                                                    &payload,
                                                    sizeof(payload)};
                        ok(weft_renderer_encode(renderer, prepared, &pass, &use), "encode");
                    },
                    [&](bool done) {
                        if (!done) {
                            ok(weft_renderer_notify_submitted(renderer, use, identity, ++serial), "submitted");
                        } else {
                            ok(weft_renderer_notify_completed(renderer, info.identity.device_epoch, identity, serial),
                               "completed");
                        }
                    });
                ok(weft_renderer_release_prepared(renderer, prepared), "release");
                return pixel(pixels, 52, 134);
            };
            require(on_plane(0.016) == std::array<std::uint8_t, 4>{255, 0, 255, 255}, "the texture on its plane");
            solid({0, 255, 0, 255});
            require(on_plane(0.032) == std::array<std::uint8_t, 4>{0, 255, 0, 255},
                    "the texture's new content on its plane");
        }
        // A texture that is the target itself, its image named: the pass into it is refused, as on D3D11, and nothing
        // is recorded; the first version of the structure (no image) is still taken.
        {
            const weft_render_vulkan_texture target_native{sizeof(target_native), WEFT_RENDER_EXTERNAL_ABI_VERSION,
                                                           u64(d.device), u64(host.target.view),
                                                           u64(host.target.image)};
            weft_render_handle target_lease{};
            ok(import(renderer, &target_native, sizeof(target_native), width, height, 8, WEFT_RENDER_EXTERNAL_TOP_LEFT,
                      WEFT_RENDER_EXTERNAL_SAME_QUEUE_LIVE, nullptr, target_lease),
               "import the target as a texture");
            frame = page.frame(0.048);
            weft_render_handle prepared{};
            ok(prepare_bound(renderer, frame, {bound("minimap", target_lease)}, prepared), "prepare");
            weft_view_frame_release(frame);
            const auto recorded = backend.renderer()->recordings;
            (void)host.frame(
                [&](VkCommandBuffer command) {
                    weft_render_vulkan_pass payload{sizeof(payload),
                                                    WEFT_RENDER_ABI_VERSION,
                                                    u64(d.device),
                                                    u64(command),
                                                    u64(host.target.view),
                                                    0,
                                                    0,
                                                    {},
                                                    u64(host.target.image)};
                    const weft_render_pass pass{sizeof(pass),
                                                WEFT_RENDER_ABI_VERSION,
                                                info.identity,
                                                width,
                                                height,
                                                WEFT_RENDER_TARGET_LINEAR_BGRA8,
                                                1,
                                                &payload,
                                                sizeof(payload)};
                    weft_render_handle use{};
                    require(weft_renderer_encode(renderer, prepared, &pass, &use) == WEFT_RENDER_INVALID_STATE,
                            "a pass into a texture its frame binds");
                },
                [](bool) {
                });
            require(backend.renderer()->recordings == recorded, "nothing recorded");
            ok(weft_renderer_release_prepared(renderer, prepared), "release");
            ok(weft_renderer_release_texture(renderer, target_lease), "release");
            weft_render_handle first_version{};
            auto version_one = target_native;
            version_one.struct_size = std::uint32_t(offsetof(weft_render_vulkan_texture, image));
            ok(import(renderer, &version_one, version_one.struct_size, width, height, 9, WEFT_RENDER_EXTERNAL_TOP_LEFT,
                      WEFT_RENDER_EXTERNAL_SAME_QUEUE_LIVE, nullptr, first_version),
               "a first-version texture");
            ok(weft_renderer_release_texture(renderer, first_version), "release");
        }
        ok(weft_renderer_release_texture(renderer, portrait_lease), "release");
        ok(weft_renderer_release_texture(renderer, minimap_lease), "release");
        require(portrait_retired.calls == 1 && minimap_retired.calls == 1, "both retired");
        ok(weft_renderer_destroy(renderer), "destroy");
        d.destroy(portrait);
        d.destroy(minimap);
        d.destroy(portrait_pixels);
        d.destroy(minimap_pixels);
        require(d.validation_messages == 0, "validation: " + d.first_validation_message);
    }

    // The same views on the Vulkan backend: an sRGB image view, a BGRA one, an image's five levels, each level written
    // in the frame's commands; drawn within 2 levels of D3D11 WARP; no validation message.
    void vulkan_texture_formats() {
        std::string unavailable;
        auto device = vk::Device::open({}, unavailable);
        if (!device) {
            std::cout << "Vulkan skipped: " << unavailable << '\n';
            return;
        }
        VulkanHost host(std::move(device));
        auto& d = *host.device;
        Page page(formats_script, formats_css);
        auto* frame = page.frame(0.016);
        constexpr std::uint64_t identity = 9;
        vk::ViewBackend backend({d.instance, d.physical, d.device, d.queue, d.queue_family, d.get_instance_proc},
                                identity);
        weft_renderer* renderer{};
        ok(weft_renderer_create(&backend.table(), nullptr, &renderer), "renderer");
        const auto external = backend.external();
        for (const auto format :
             {WEFT_RENDER_TARGET_SRGB_RGBA8, WEFT_RENDER_TARGET_LINEAR_BGRA8, WEFT_RENDER_TARGET_SRGB_BGRA8}) {
            require(external.formats & (1u << format), "sRGB and BGRA views among the extension's formats");
        }
        ok(weft_renderer_install_external_backend(renderer, &external), "install the extension");
        weft_render_info info{};
        info.struct_size = sizeof(info);
        ok(weft_renderer_get_info(renderer, &info), "info");

        // The images and their texels, every level's in one buffer.
        struct Texture {
            vk::Image image;
            vk::Buffer pixels;
            std::vector<std::vector<std::uint8_t>> levels;
        };

        const auto make = [&](VkFormat format, unsigned size, std::vector<std::vector<std::uint8_t>> levels) {
            Texture t;
            t.image = d.image(format, size, size, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              std::uint32_t(levels.size()));
            std::size_t bytes{};
            for (const auto& l : levels) {
                bytes += l.size();
            }
            t.pixels = d.buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
            std::size_t at{};
            for (const auto& l : levels) {
                std::memcpy(static_cast<std::uint8_t*>(t.pixels.mapped) + at, l.data(), l.size());
                at += l.size();
            }
            t.levels = std::move(levels);
            return t;
        };
        auto srgb = make(VK_FORMAT_R8G8B8A8_SRGB, 2, {solid_texels(2, 2, srgb_texel)});
        auto bgra = make(VK_FORMAT_B8G8R8A8_UNORM, 2, {solid_texels(2, 2, bgra_texel)});
        auto levels = make(VK_FORMAT_R8G8B8A8_UNORM, 16, level_texels());
        // The host's writes in the frame's commands before the pass: every level, then shader-readable.
        const auto write = [&](VkCommandBuffer command, Texture& t) {
            vk::transition(d, command, t.image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkDeviceSize at{};
            for (std::uint32_t k = 0; k < t.levels.size(); ++k) {
                VkBufferImageCopy copy{};
                copy.bufferOffset = at;
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, k, 0, 1};
                copy.imageExtent = {std::max(1u, t.image.width >> k), std::max(1u, t.image.height >> k), 1};
                d.vkd.vkCmdCopyBufferToImage(command, t.pixels.buffer, t.image.image,
                                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                at += t.levels[k].size();
            }
            vk::transition(d, command, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        };
        const auto native = [&](const Texture& t) {
            return weft_render_vulkan_texture{sizeof(weft_render_vulkan_texture), WEFT_RENDER_EXTERNAL_ABI_VERSION,
                                              u64(d.device), u64(t.image.view)};
        };
        const auto srgb_native = native(srgb);
        const auto bgra_native = native(bgra);
        const auto levels_native = native(levels);
        constexpr auto top = WEFT_RENDER_EXTERNAL_TOP_LEFT;
        constexpr auto ordered = WEFT_RENDER_EXTERNAL_SAME_QUEUE_ORDERED;
        weft_render_handle srgb_lease{};
        weft_render_handle bgra_lease{};
        weft_render_handle levels_lease{};
        ok(import(renderer, &srgb_native, sizeof(srgb_native), 2, 2, 1, top, ordered, nullptr, srgb_lease,
                  WEFT_RENDER_TARGET_SRGB_RGBA8),
           "import the sRGB view");
        ok(import(renderer, &bgra_native, sizeof(bgra_native), 2, 2, 2, top, ordered, nullptr, bgra_lease,
                  WEFT_RENDER_TARGET_LINEAR_BGRA8),
           "import the BGRA view");
        ok(import(renderer, &levels_native, sizeof(levels_native), 16, 16, 3, top, ordered, nullptr, levels_lease,
                  WEFT_RENDER_TARGET_LINEAR_RGBA8, 5),
           "import the levels");
        weft_render_handle prepared{};
        ok(prepare_bound(renderer, frame,
                         {bound("srgb", srgb_lease), bound("bgra", bgra_lease), bound("levels", levels_lease)},
                         prepared),
           "prepare");
        std::uint64_t serial{};
        weft_render_handle use{};
        const auto pixels = host.frame(
            [&](VkCommandBuffer command) {
                for (auto* t : {&srgb, &bgra, &levels}) {
                    write(command, *t);
                }
                weft_render_vulkan_pass payload{sizeof(payload),
                                                WEFT_RENDER_ABI_VERSION,
                                                u64(d.device),
                                                u64(command),
                                                u64(host.target.view),
                                                0,
                                                0,
                                                {},
                                                u64(host.target.image)};
                const weft_render_pass pass{sizeof(pass),
                                            WEFT_RENDER_ABI_VERSION,
                                            info.identity,
                                            width,
                                            height,
                                            WEFT_RENDER_TARGET_LINEAR_BGRA8,
                                            1,
                                            &payload,
                                            sizeof(payload)};
                ok(weft_renderer_encode(renderer, prepared, &pass, &use), "encode");
            },
            [&](bool done) {
                if (!done) {
                    ok(weft_renderer_notify_submitted(renderer, use, identity, ++serial), "submitted");
                } else {
                    ok(weft_renderer_notify_completed(renderer, info.identity.device_epoch, identity, serial),
                       "completed");
                }
            });
        ok(weft_renderer_release_prepared(renderer, prepared), "release");
        formats_shown(pixels);
        if (!d3d11_formatted.empty()) {
            require(within(pixels, d3d11_formatted, 2), "the views drawn as D3D11 WARP draws them");
        }
        weft_view_frame_release(frame);
        for (const auto lease : {srgb_lease, bgra_lease, levels_lease}) {
            ok(weft_renderer_release_texture(renderer, lease), "release");
        }
        ok(weft_renderer_destroy(renderer), "destroy");
        for (auto* t : {&srgb, &bgra, &levels}) {
            d.destroy(t->image);
            d.destroy(t->pixels);
        }
        require(d.validation_messages == 0, "validation: " + d.first_validation_message);
    }
#endif
} // namespace

int main() {
    try {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
            throw std::runtime_error("COM");
        }
        d3d11_frames();
        d3d11_root_backgrounds();
        d3d11_kept();
        d3d11_textures();
        d3d11_texture_formats();
        d3d11_shared_texture();
#ifdef WEFT_TEST_VULKAN
        vulkan_frames();
        vulkan_textures();
        vulkan_texture_formats();
#endif
        std::cout << "render view tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "render view tests failed: " << e.what() << '\n';
        return 1;
    }
}
