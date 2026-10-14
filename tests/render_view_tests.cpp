#define NOMINMAX
#include <weft/render_backend.h>
#include <weft/render_backend_d3d11.h>
#include <weft/weft.h>

#include "render/backends/d3d11/scene_renderer.hpp"
#include "render/backends/d3d11/view_backend.hpp"
#include "render/srgb.hpp"
#include "support/check.hpp"
#include "view/view_abi.hpp"
#ifdef WEFT_TEST_VULKAN
#include <weft/render_backend_vulkan.h>

#include "render/backends/vulkan/device.hpp"
#include "render/backends/vulkan/pipeline.hpp"
#include "render/backends/vulkan/scene_renderer.hpp"
#include "render/backends/vulkan/view_backend.hpp"
#endif

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
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
        explicit Page(const std::string& root = {}, weft_runtime* shared = nullptr) : runtime(shared), owner(!shared) {
            if (owner) {
                ok(weft_runtime_create(nullptr, &runtime), "runtime");
            }
            const weft_view_desc desc{sizeof(desc), 0, float(width), float(height), 1, 0};
            ok(weft_view_create(runtime, &desc, &view), "view");
            static const char script[] = R"JS(
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
            const std::string css =
                "#root{position:relative;width:100%;height:100%;font-family:sans-serif;" + root +
                "}"
                ".plate{position:absolute;left:0;top:0;width:60px;height:20px;background-color:rgba(200,40,40,0.8);"
                "border-radius:4px;color:#fff;font-size:12px;line-height:20px;text-align:center}"
                ".panel{position:absolute;right:10px;bottom:10px;width:80px;height:30px;background-color:#334;"
                "border:1px solid #aab;color:#eee;font-size:14px;line-height:28px}"
                ".glass{position:absolute;left:250px;top:10px;width:60px;height:40px;"
                "backdrop-filter:blur(2px) brightness(0.5)}";
            const weft_script s{"page.js", 7, script, sizeof script - 1};
            const weft_page_desc page{sizeof(page), 0, &s, 1, 0, css.data(), css.size(), nullptr, 0, nullptr, 0};
            ok(weft_view_load(runtime, view, &page), "load");
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
        bad.reserved = 1;
        require(weft_renderer_prepare_scene(renderer, &bad, &refused) == WEFT_RENDER_INVALID_ARGUMENT,
                "a reserved field set");
        bad.reserved = 0;
        bad.struct_size = sizeof(bad) - 4;
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

    // A frame of an unchanged revision redraws the View's kept pass; a changed one prepares again.
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
        require(backend.renderer()->passes == 3 && backend.renderer()->kept_passes == 1,
                "a changed scene prepares again");
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
#ifdef WEFT_TEST_VULKAN
        vulkan_frames();
#endif
        std::cout << "render view tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "render view tests failed: " << e.what() << '\n';
        return 1;
    }
}
