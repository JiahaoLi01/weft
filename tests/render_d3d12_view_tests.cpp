#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <weft/render_backend.h>
#include <weft/render_backend_d3d11.h>
#include <weft/render_backend_d3d12.h>
#include <weft/render_external_texture.h>
#include <weft/weft.h>

#include "render/srgb.hpp"
#include "support/check.hpp"

#include <d3d11.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// View frames through the renderer C ABI on the built-in D3D12 backend (WARP, with the debug layer when it is
// installed), against the D3D11 backend (WARP) drawing the same frames, byte for byte: over the game's picture in the host's target
// and replacing it with the host's clear; anchored elements where the View's latest anchors are, the kept pass drawn
// again; backdrops that read the target and a group; host textures by name; BGRA8, RGBA8 and typeless targets; a
// recording the host discards; and what the backend refuses. Every frame's commands pass the debug layer without
// errors.
namespace {
    using Microsoft::WRL::ComPtr;
    using weft::testing::require;
    using Pixels = std::vector<std::uint8_t>;

    constexpr unsigned width = 320;
    constexpr unsigned height = 200;
    // The game's picture under the UI, BGRA.
    constexpr std::array<std::uint8_t, 4> game{0x80, 0x60, 0x20, 0xff};
    constexpr std::uint64_t queue = 1;
    // Levels by which the two WARP devices' pixels may differ: none (the same shaders and records, rasterized alike).
    constexpr int tolerance = 0;
    constexpr int skipped = 77;

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

    // A View of its page through the View's C ABI: anchored plates with text, a static panel and a glass panel that
    // darkens what is beneath it; a translucent group with a glass of its own inside; host textures by name.
    struct Page {
        weft_runtime* runtime{};
        weft_view_handle view{};

        Page(std::string_view script, const std::string& css) {
            ok(weft_runtime_create(nullptr, &runtime), "runtime");
            const weft_view_desc desc{sizeof(desc), 0, float(width), float(height), 1, 0};
            ok(weft_view_create(runtime, &desc, &view), "view");
            const weft_script s{"page.js", 7, script.data(), script.size()};
            const weft_page_desc page{sizeof(page), 0, &s, 1, 0, css.data(), css.size(), nullptr, 0, nullptr, 0};
            ok(weft_view_load(runtime, view, &page), "load");
        }

        ~Page() {
            weft_view_destroy(runtime, view);
            weft_runtime_destroy(runtime);
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

    constexpr std::string_view plates_script = R"JS(
      const root = document.getElementById('root');
      for (const [key, text] of [[1, 'Goblin'], [2, '兽人']]) {
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
      const group = document.createElement('div');
      group.className = 'group';
      group.textContent = 'Group';
      const inner = document.createElement('div');
      inner.className = 'inner';
      group.appendChild(inner);
      root.appendChild(group);
    )JS";
    const std::string plates_css =
        "#root{position:relative;width:100%;height:100%;font-family:sans-serif}"
        ".plate{position:absolute;left:0;top:0;width:60px;height:20px;background-color:rgba(200,40,40,0.8);"
        "border-radius:4px;color:#fff;font-size:12px;line-height:20px;text-align:center}"
        ".panel{position:absolute;right:10px;bottom:10px;width:80px;height:30px;background-color:#334;"
        "border:1px solid #aab;color:#eee;font-size:14px;line-height:28px}"
        ".glass{position:absolute;left:250px;top:10px;width:60px;height:40px;"
        "backdrop-filter:blur(2px) brightness(0.5)}"
        ".group{position:absolute;left:20px;top:120px;width:120px;height:60px;opacity:0.6;"
        "background-color:#204080;border-radius:8px;color:#fff;font-size:14px}"
        ".inner{position:absolute;left:60px;top:10px;width:50px;height:40px;"
        "backdrop-filter:blur(3px) brightness(1.5);border:1px solid #fff}";

    // Host textures: "portrait" over a box with round corners, "minimap" over a red background color and under a white
    // border, and "missing", which no frame binds.
    constexpr std::string_view textures_script = R"JS(
      const root = document.getElementById('root');
      for (const name of ['portrait', 'minimap', 'missing']) {
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
        "background-image:-weft-texture(missing)}";

    // A 4 x 4 texture of linear premultiplied RGBA8 in quadrants of 2 x 2 texels: red, green; a linear grey, and blue
    // at half opacity.
    std::vector<std::uint8_t> quadrants() {
        constexpr std::array<std::array<std::uint8_t, 4>, 4> colors{
            {{255, 0, 0, 255}, {0, 255, 0, 255}, {128, 128, 128, 255}, {0, 0, 128, 128}}};
        std::vector<std::uint8_t> out;
        for (unsigned y = 0; y < 4; ++y) {
            for (unsigned x = 0; x < 4; ++x) {
                const auto& c = colors[(y / 2) * 2 + x / 2];
                out.insert(out.end(), c.begin(), c.end());
            }
        }
        return out;
    }

    std::vector<std::uint8_t> solid(unsigned w, unsigned h, std::array<std::uint8_t, 4> c) {
        std::vector<std::uint8_t> out;
        for (unsigned i = 0; i < w * h; ++i) {
            out.insert(out.end(), c.begin(), c.end());
        }
        return out;
    }

    std::array<std::uint8_t, 4> pixel(const Pixels& p, unsigned x, unsigned y) {
        const auto* at = p.data() + (std::size_t(y) * width + x) * 4;
        return {at[0], at[1], at[2], at[3]};
    }

    // The game's picture, darkened.
    bool darker(const std::array<std::uint8_t, 4>& p) {
        return p[0] < game[0] && p[1] < game[1] && p[2] <= game[2] && p[3] == game[3];
    }

    // How far two images are apart: the largest difference of a channel, and where.
    int difference(const Pixels& a, const Pixels& b, std::size_t* where = nullptr) {
        require(a.size() == b.size(), "images of one size");
        int most{};
        for (std::size_t i = 0; i < a.size(); ++i) {
            const int d = std::abs(int(a[i]) - int(b[i]));
            if (d > most) {
                most = d;
                if (where) {
                    *where = i;
                }
            }
        }
        return most;
    }

    // With WEFT_D3D12_DUMP set to a folder: an image as a BMP of BGRA rows there.
    void dump(const Pixels& p, const std::string& name) {
        const char* folder = std::getenv("WEFT_D3D12_DUMP");
        if (!folder) {
            return;
        }
        const std::string path = std::string(folder) + "/" + name + ".bmp";
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) {
            return;
        }
        const std::uint32_t bytes = std::uint32_t(p.size());
        std::uint8_t header[54]{'B', 'M'};
        const auto put = [&](std::size_t at, std::uint32_t v) {
            std::memcpy(header + at, &v, 4);
        };
        put(2, 54 + bytes);
        put(10, 54);
        put(14, 40);
        put(18, width);
        put(22, std::uint32_t(-std::int32_t(height)));
        header[26] = 1;
        header[28] = 32;
        put(34, bytes);
        std::fwrite(header, 1, sizeof(header), f);
        std::fwrite(p.data(), 1, p.size(), f);
        std::fclose(f);
    }

    void same(const Pixels& d3d12, const Pixels& d3d11, const char* what) {
        dump(d3d12, std::string(what) + "-d3d12");
        dump(d3d11, std::string(what) + "-d3d11");
        std::size_t where{};
        const int d = difference(d3d12, d3d11, &where);
        if (d > tolerance) {
            const auto x = (where / 4) % width;
            const auto y = (where / 4) / width;
            throw std::runtime_error(std::string(what) + ": D3D12 and D3D11 differ by " + std::to_string(d) + " at (" +
                                     std::to_string(x) + ", " + std::to_string(y) + ")");
        }
        std::cout << "  " << what << ": at most " << d << " levels apart" << std::endl;
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

    weft_render_handle prepare(weft_renderer* renderer, const weft_view_frame* frame,
                               const std::vector<weft_render_scene_texture>& textures = {}) {
        const weft_render_scene_frame f{sizeof(f), WEFT_RENDER_ABI_VERSION,        frame,
                                        0,         std::uint32_t(textures.size()), textures.data()};
        weft_render_handle out{};
        ok(weft_renderer_prepare_scene(renderer, &f, &out), "prepare scene");
        return out;
    }

    weft_render_info info_of(weft_renderer* renderer) {
        weft_render_info info{};
        info.struct_size = sizeof(info);
        ok(weft_renderer_get_info(renderer, &info), "info");
        return info;
    }

    weft_render_result import(weft_renderer* renderer, const void* native, std::uint64_t native_size, unsigned w,
                              unsigned h, std::uint64_t id, weft_render_handle& out,
                              std::uint32_t format = WEFT_RENDER_TARGET_LINEAR_RGBA8) {
        const weft_render_external_texture d{sizeof(d),
                                             WEFT_RENDER_EXTERNAL_ABI_VERSION,
                                             info_of(renderer).identity,
                                             id,
                                             1,
                                             w,
                                             h,
                                             format,
                                             WEFT_RENDER_EXTERNAL_LINEAR_PREMULTIPLIED,
                                             WEFT_RENDER_EXTERNAL_TOP_LEFT,
                                             WEFT_RENDER_EXTERNAL_SAME_QUEUE_ORDERED,
                                             0,
                                             0,
                                             native,
                                             native_size,
                                             nullptr,
                                             nullptr};
        return weft_renderer_import_texture(renderer, &d, &out);
    }

    weft_render_scene_texture bound(const char* name, weft_render_handle texture) {
        return {name, std::uint32_t(std::strlen(name)), 0, texture};
    }

    // The reference: the D3D11 backend on WARP drawing into targets cleared to the game's picture.
    struct D3D11 {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        weft_render_d3d11* backend{};
        weft_renderer* renderer{};
        std::uint64_t serial{};

        D3D11() {
            const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
            require(
                SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                            levels, 1, D3D11_SDK_VERSION, &device, nullptr, &context)),
                "a D3D11 WARP device");
            const weft_render_d3d11_desc desc{sizeof(desc), WEFT_RENDER_ABI_VERSION, device.Get(), queue, 0};
            weft_render_backend table{};
            ok(weft_render_d3d11_create(&desc, &backend, &table), "the D3D11 backend");
            ok(weft_renderer_create(&table, nullptr, &renderer), "the D3D11 renderer");
            weft_render_external_backend external{};
            ok(weft_render_d3d11_external(backend, &external), "the D3D11 external textures");
            ok(weft_renderer_install_external_backend(renderer, &external), "install");
        }

        ~D3D11() {
            weft_renderer_destroy(renderer);
            weft_render_d3d11_destroy(backend);
        }

        // A texture of linear premultiplied RGBA8 texels (rows top down).
        ComPtr<ID3D11ShaderResourceView> texture(unsigned w, unsigned h, const std::vector<std::uint8_t>& rgba) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = w;
            desc.Height = h;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            const D3D11_SUBRESOURCE_DATA data{rgba.data(), w * 4, 0};
            ComPtr<ID3D11Texture2D> t;
            ComPtr<ID3D11ShaderResourceView> srv;
            require(SUCCEEDED(device->CreateTexture2D(&desc, &data, &t)) &&
                        SUCCEEDED(device->CreateShaderResourceView(t.Get(), nullptr, &srv)),
                    "a D3D11 host texture");
            return srv;
        }

        // The frame drawn over the game's picture in a target of format, cleared first to clear when given.
        Pixels draw(weft_render_handle prepared, DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM,
                    const float* clear = nullptr) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            ComPtr<ID3D11Texture2D> target;
            ComPtr<ID3D11RenderTargetView> rtv;
            require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &target)) &&
                        SUCCEEDED(device->CreateRenderTargetView(target.Get(), nullptr, &rtv)),
                    "a D3D11 target");
            const bool bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM;
            const float picture[]{game[2] / 255.f, game[1] / 255.f, game[0] / 255.f, 1};
            context->ClearRenderTargetView(rtv.Get(), picture);
            const auto info = info_of(renderer);
            void* list = reinterpret_cast<void*>(1);
            weft_render_d3d11_pass payload{
                sizeof(payload), WEFT_RENDER_ABI_VERSION, nullptr, rtv.Get(), &list, 0, 0, {}};
            if (clear) {
                payload.clear_target = 1;
                std::copy(clear, clear + 4, payload.clear_linear_premultiplied);
            }
            const weft_render_pass pass{
                sizeof(pass),
                WEFT_RENDER_ABI_VERSION,
                info.identity,
                width,
                height,
                std::uint32_t(bgra ? WEFT_RENDER_TARGET_LINEAR_BGRA8 : WEFT_RENDER_TARGET_LINEAR_RGBA8),
                1,
                &payload,
                sizeof(payload)};
            weft_render_handle use{};
            ok(weft_renderer_encode(renderer, prepared, &pass, &use), "D3D11 encode");
            ok(weft_renderer_notify_submitted(renderer, use, queue, ++serial), "D3D11 submitted");
            ok(weft_renderer_notify_completed(renderer, info.identity.device_epoch, queue, serial), "D3D11 completed");
            desc.BindFlags = 0;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)), "D3D11 readback");
            context->CopyResource(staging.Get(), target.Get());
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

    // A D3D12 WARP device with its direct queue, one command list and a fence, the debug layer's messages checked
    // after every execution when it is installed.
    struct D3D12 {
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Fence> fence;
        ComPtr<ID3D12InfoQueue> messages;
        HANDLE event{};
        std::uint64_t value{};

        // False without a WARP adapter of feature level 11 (or, not warp, a hardware one).
        bool open(bool warp_adapter = true) {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
                debug->EnableDebugLayer();
                // GPU-based validation (slow): the commands' descriptors, resource states and reads checked as they run.
                ComPtr<ID3D12Debug1> gpu_validation;
                if (std::getenv("WEFT_D3D12_GPU_VALIDATION") && SUCCEEDED(debug.As(&gpu_validation))) {
                    gpu_validation->SetEnableGPUBasedValidation(TRUE);
                }
            }
            ComPtr<IDXGIFactory4> factory;
            ComPtr<IDXGIAdapter> warp;
            if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
                return false;
            }
            if (warp_adapter) {
                if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) {
                    return false;
                }
            } else {
                ComPtr<IDXGIAdapter1> adapter;
                for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                    DXGI_ADAPTER_DESC1 desc{};
                    adapter->GetDesc1(&desc);
                    if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                        warp = adapter;
                        break;
                    }
                }
                if (!warp) {
                    return false;
                }
            }
            if (FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
                return false;
            }
            if (debug && SUCCEEDED(device.As(&messages))) {
                std::cout << "D3D12 debug layer on\n";
            }
            D3D12_COMMAND_QUEUE_DESC desc{};
            desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            require(SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))) &&
                        SUCCEEDED(
                            device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
                        SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                            IID_PPV_ARGS(&list))) &&
                        SUCCEEDED(list->Close()) &&
                        SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))),
                    "the D3D12 queue, command list and fence");
            event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            return true;
        }

        ~D3D12() {
            if (event) {
                CloseHandle(event);
            }
        }

        ID3D12GraphicsCommandList* begin() {
            require(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(), nullptr)), "begin");
            return list.Get();
        }

        // Executes the recording and waits for it.
        void finish() {
            require(SUCCEEDED(list->Close()), "close the command list");
            ID3D12CommandList* lists[]{list.Get()};
            queue->ExecuteCommandLists(1, lists);
            wait();
        }

        void wait() {
            require(SUCCEEDED(queue->Signal(fence.Get(), ++value)), "signal");
            require(SUCCEEDED(fence->SetEventOnCompletion(value, event)), "fence event");
            WaitForSingleObject(event, INFINITE);
            check_messages();
        }

        // Fails on any error the debug layer reported.
        void check_messages() {
            if (!messages) {
                return;
            }
            std::string errors;
            for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
                SIZE_T size{};
                messages->GetMessage(i, nullptr, &size);
                std::vector<std::byte> bytes(size);
                auto* m = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
                messages->GetMessage(i, m, &size);
                if (m->Severity == D3D12_MESSAGE_SEVERITY_ERROR || m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
                    errors.append(m->pDescription, m->DescriptionByteLength ? m->DescriptionByteLength - 1 : 0);
                    errors += '\n';
                }
            }
            messages->ClearStoredMessages();
            require(errors.empty(), "D3D12 debug layer errors:\n" + errors);
        }

        // A render target of format (or its typeless resource format) cleared to the game's picture, in the render
        // target state.
        ComPtr<ID3D12Resource> target(DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT resource = {},
                                      unsigned w = width, unsigned h = height) {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = w;
            desc.Height = h;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = resource ? resource : format;
            desc.SampleDesc.Count = 1;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            ComPtr<ID3D12Resource> out;
            require(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                              D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                              IID_PPV_ARGS(&out))),
                    "a D3D12 target");
            D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
            heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            heap_desc.NumDescriptors = 1;
            ComPtr<ID3D12DescriptorHeap> rtvs;
            require(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtvs))), "a view heap");
            D3D12_RENDER_TARGET_VIEW_DESC view{};
            view.Format = format;
            view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            const auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
            device->CreateRenderTargetView(out.Get(), &view, rtv);
            const float picture[]{game[2] / 255.f, game[1] / 255.f, game[0] / 255.f, 1};
            begin()->ClearRenderTargetView(rtv, picture, 0, nullptr);
            finish();
            return out;
        }

        // A texture of linear premultiplied RGBA8 texels (rows top down) in the pixel shader resource state.
        ComPtr<ID3D12Resource> texture(unsigned w, unsigned h, const std::vector<std::uint8_t>& rgba) {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = w;
            desc.Height = h;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            ComPtr<ID3D12Resource> out;
            require(
                SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out))),
                "a D3D12 host texture");
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT64 bytes{};
            device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
            auto upload = buffer(bytes, D3D12_HEAP_TYPE_UPLOAD);
            void* mapped{};
            require(SUCCEEDED(upload->Map(0, nullptr, &mapped)), "map upload");
            for (unsigned y = 0; y < h; ++y) {
                std::memcpy(static_cast<std::uint8_t*>(mapped) + y * footprint.Footprint.RowPitch,
                            rgba.data() + y * w * 4, w * 4);
            }
            upload->Unmap(0, nullptr);
            auto* command = begin();
            D3D12_TEXTURE_COPY_LOCATION from{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
            from.PlacedFootprint = footprint;
            D3D12_TEXTURE_COPY_LOCATION to{out.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
            to.SubresourceIndex = 0;
            command->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            transition(out.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            finish();
            return out;
        }

        ComPtr<ID3D12Resource> buffer(UINT64 size, D3D12_HEAP_TYPE type) {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = type;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = size;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> out;
            const auto state =
                type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST;
            require(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                              IID_PPV_ARGS(&out))),
                    "a D3D12 buffer");
            return out;
        }

        void transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = resource;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = from;
            b.Transition.StateAfter = to;
            list->ResourceBarrier(1, &b);
        }

        // The target's top-left width x height pixels (4 bytes each).
        Pixels read(ID3D12Resource* target) {
            auto desc = target->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT64 bytes{};
            device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
            auto readback = buffer(bytes, D3D12_HEAP_TYPE_READBACK);
            begin();
            transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION from{target, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
            D3D12_TEXTURE_COPY_LOCATION to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
            to.PlacedFootprint = footprint;
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            transition(target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            finish();
            void* mapped{};
            const auto mapping = readback->Map(0, nullptr, &mapped);
            if (FAILED(mapping)) {
                char text[96];
                std::snprintf(text, sizeof(text), "map readback: 0x%08lx, device 0x%08lx", unsigned long(mapping),
                              unsigned long(device->GetDeviceRemovedReason()));
                throw std::runtime_error(text);
            }
            Pixels out(std::size_t(width) * height * 4);
            for (unsigned y = 0; y < height; ++y) {
                std::memcpy(out.data() + std::size_t(y) * width * 4,
                            static_cast<const std::uint8_t*>(mapped) + std::size_t(y) * footprint.Footprint.RowPitch,
                            width * 4);
            }
            readback->Unmap(0, nullptr);
            return out;
        }
    };

    // The D3D12 backend on a device, its renderer and the serials of its submissions.
    struct Backend {
        D3D12& gpu;
        weft_render_d3d12* backend{};
        weft_render_backend table{};
        weft_renderer* renderer{};
        std::uint64_t serial{};

        explicit Backend(D3D12& device) : gpu(device) {
            const weft_render_d3d12_desc desc{sizeof(desc), WEFT_RENDER_ABI_VERSION, gpu.device.Get(), queue, 0};
            ok(weft_render_d3d12_create(&desc, &backend, &table), "the D3D12 backend");
            ok(weft_renderer_create(&table, nullptr, &renderer), "the D3D12 renderer");
            weft_render_external_backend external{};
            ok(weft_render_d3d12_external(backend, &external), "the D3D12 external textures");
            ok(weft_renderer_install_external_backend(renderer, &external), "install");
        }

        ~Backend() {
            weft_renderer_destroy(renderer);
            weft_render_d3d12_destroy(backend);
        }

        weft_render_d3d12_pass payload(ID3D12Resource* target, const float* clear = nullptr) {
            weft_render_d3d12_pass p{sizeof(p), WEFT_RENDER_ABI_VERSION, gpu.list.Get(), target, 0, 0, {}};
            if (clear) {
                p.clear_target = 1;
                std::copy(clear, clear + 4, p.clear_linear_premultiplied);
            }
            return p;
        }

        // Encodes prepared into a recording of the device's command list; a failed one is closed unexecuted.
        weft_render_result encode(weft_render_handle prepared, const weft_render_d3d12_pass& p, weft_render_handle& use,
                                  std::uint32_t format = WEFT_RENDER_TARGET_LINEAR_BGRA8, unsigned w = width,
                                  unsigned h = height) {
            const weft_render_pass pass{
                sizeof(pass), WEFT_RENDER_ABI_VERSION, info_of(renderer).identity, w, h, format, 1, &p, sizeof(p)};
            return weft_renderer_encode(renderer, prepared, &pass, &use);
        }

        // prepared drawn into target: encode, execute, submitted, wait, completed; then its pixels.
        Pixels draw(weft_render_handle prepared, ID3D12Resource* target,
                    std::uint32_t format = WEFT_RENDER_TARGET_LINEAR_BGRA8, const float* clear = nullptr) {
            gpu.begin();
            weft_render_handle use{};
            const auto result = encode(prepared, payload(target, clear), use, format);
            if (result != WEFT_RENDER_OK) {
                gpu.list->Close();
                ok(result, "D3D12 encode");
            }
            require(SUCCEEDED(gpu.list->Close()), "close");
            ID3D12CommandList* lists[]{gpu.list.Get()};
            gpu.queue->ExecuteCommandLists(1, lists);
            ok(weft_renderer_notify_submitted(renderer, use, queue, ++serial), "D3D12 submitted");
            gpu.wait();
            ok(weft_renderer_notify_completed(renderer, info_of(renderer).identity.device_epoch, queue, serial),
               "D3D12 completed");
            return gpu.read(target);
        }
    };

    // The plates page on both backends: over the game, the host's clear, anchors moved (the kept pass), RGBA8 and
    // typeless targets, and a recording discarded.
    void frames(D3D12& gpu) {
        D3D11 reference;
        Backend d3d12(gpu);
        Page page(plates_script, plates_css);
        page.anchors(40, 30, 150, 90);
        auto* frame = page.frame(0.016);

        // Over the game's picture, as the D3D11 backend draws it.
        auto ours = prepare(d3d12.renderer, frame);
        auto theirs = prepare(reference.renderer, frame);
        weft_view_frame_release(frame);
        auto target = gpu.target();
        const auto drawn = d3d12.draw(ours, target.Get());
        const auto expected = reference.draw(theirs);
        same(drawn, expected, "over the game");
        require(pixel(drawn, 2, 2) == game && pixel(drawn, width - 2, 2) == game, "the game shows around the UI");
        require(pixel(drawn, 15, 22) != game, "a plate over the game");
        require(darker(pixel(drawn, 280, 30)), "the glass darkens the game beneath it");

        // The same prepared frame again (the kept pass), then cleared to the host's color first.
        target = gpu.target();
        same(d3d12.draw(ours, target.Get()), expected, "the kept pass");
        const float blue[]{0.05f, 0.1f, 0.4f, 1};
        target = gpu.target();
        const auto cleared = d3d12.draw(ours, target.Get(), WEFT_RENDER_TARGET_LINEAR_BGRA8, blue);
        same(cleared, reference.draw(theirs, DXGI_FORMAT_B8G8R8A8_UNORM, blue), "the host's clear");
        require(pixel(cleared, 2, 2) == encoded_bgra(blue), "the host's clear, then the page");
        ok(weft_renderer_release_prepared(d3d12.renderer, ours), "release");
        ok(weft_renderer_release_prepared(reference.renderer, theirs), "release");

        // The anchors move, the scene does not: the kept pass draws where the anchors are now.
        page.anchors(200, 120, 60, 150);
        frame = page.frame(0.032);
        ours = prepare(d3d12.renderer, frame);
        theirs = prepare(reference.renderer, frame);
        weft_view_frame_release(frame);
        target = gpu.target();
        const auto moved = d3d12.draw(ours, target.Get());
        same(moved, reference.draw(theirs), "anchors moved");
        require(difference(moved, drawn) > tolerance, "drawn where the anchors are now");

        // An RGBA8 target, and a typeless BGRA8 one (a game's swap chain buffer, say).
        target = gpu.target(DXGI_FORMAT_R8G8B8A8_UNORM);
        same(d3d12.draw(ours, target.Get(), WEFT_RENDER_TARGET_LINEAR_RGBA8),
             reference.draw(theirs, DXGI_FORMAT_R8G8B8A8_UNORM), "an RGBA8 target");
        target = gpu.target(DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_TYPELESS);
        same(d3d12.draw(ours, target.Get()), moved, "a typeless target");

        // A recording the host never executes: discarded, and the next one draws as before.
        target = gpu.target();
        gpu.begin();
        weft_render_handle use{};
        ok(d3d12.encode(ours, d3d12.payload(target.Get()), use), "encode to discard");
        require(SUCCEEDED(gpu.list->Close()), "close");
        ok(weft_renderer_discard_encoded(d3d12.renderer, use), "discard");
        same(d3d12.draw(ours, target.Get()), moved, "after a discarded recording");

        // What is not this backend's, or not a target it draws: nothing is recorded. Another device's target, when
        // this machine has a hardware adapter (D3D12 gives one device per adapter).
        D3D12 other;
        if (other.open(false)) {
            const auto foreign = other.target();
            gpu.begin();
            require(d3d12.encode(ours, d3d12.payload(foreign.Get()), use) == WEFT_RENDER_WRONG_BACKEND,
                    "another device's target");
            std::cout << "  another device's target refused" << std::endl;
            require(SUCCEEDED(gpu.list->Close()), "close");
        }
        gpu.begin();
        require(d3d12.encode(ours, d3d12.payload(target.Get()), use, WEFT_RENDER_TARGET_LINEAR_RGBA8) ==
                    WEFT_RENDER_UNSUPPORTED_FEATURE,
                "a BGRA8 target said to be RGBA8");
        require(d3d12.encode(ours, d3d12.payload(target.Get()), use, WEFT_RENDER_TARGET_LINEAR_BGRA8, width + 1) ==
                    WEFT_RENDER_INVALID_ARGUMENT,
                "a pass larger than its target");
        auto empty = d3d12.payload(target.Get());
        empty.command_list = nullptr;
        require(d3d12.encode(ours, empty, use) == WEFT_RENDER_INVALID_ARGUMENT, "no command list");
        auto clear_wrong = d3d12.payload(target.Get(), blue);
        clear_wrong.clear_linear_premultiplied[0] = 2;
        require(d3d12.encode(ours, clear_wrong, use) == WEFT_RENDER_INVALID_ARGUMENT, "a clear past premultiplied");
        require(SUCCEEDED(gpu.list->Close()), "close");
        ok(weft_renderer_release_prepared(d3d12.renderer, ours), "release");
        ok(weft_renderer_release_prepared(reference.renderer, theirs), "release");
        std::cout << "PASS D3D12 View frames as D3D11 draws them\n";
    }

    // Host textures by name, as the D3D11 backend draws them; a texture the pass would draw into is refused.
    void textures(D3D12& gpu) {
        D3D11 reference;
        Backend d3d12(gpu);
        Page page(textures_script, textures_css);
        auto* frame = page.frame(0.016);
        const auto texels = quadrants();
        const auto map_texels = solid(8, 6, {0, 96, 0, 255});

        const auto portrait12 = gpu.texture(4, 4, texels);
        const auto minimap12 = gpu.texture(8, 6, map_texels);
        const weft_render_d3d12_texture p12{sizeof(p12), WEFT_RENDER_EXTERNAL_ABI_VERSION, portrait12.Get(), 0};
        const weft_render_d3d12_texture m12{sizeof(m12), WEFT_RENDER_EXTERNAL_ABI_VERSION, minimap12.Get(), 0};
        weft_render_handle portrait{};
        weft_render_handle minimap{};
        ok(import(d3d12.renderer, &p12, sizeof(p12), 4, 4, 1, portrait), "import portrait");
        ok(import(d3d12.renderer, &m12, sizeof(m12), 8, 6, 2, minimap), "import minimap");
        weft_render_handle twice{};
        require(import(d3d12.renderer, &p12, sizeof(p12), 4, 4, 3, twice) == WEFT_RENDER_INVALID_STATE,
                "one import of a resource at a time");
        require(import(d3d12.renderer, &p12, sizeof(p12), 5, 4, 3, twice) == WEFT_RENDER_INVALID_ARGUMENT,
                "the texture's own size");
        require(import(d3d12.renderer, &p12, sizeof(p12), 4, 4, 3, twice, WEFT_RENDER_TARGET_LINEAR_BGRA8) ==
                    WEFT_RENDER_UNSUPPORTED_FEATURE,
                "an RGBA8 texture said to be BGRA8");

        const auto portrait11 = reference.texture(4, 4, texels);
        const auto minimap11 = reference.texture(8, 6, map_texels);
        const weft_render_d3d11_texture p11{sizeof(p11), WEFT_RENDER_EXTERNAL_ABI_VERSION, portrait11.Get(), 0};
        const weft_render_d3d11_texture m11{sizeof(m11), WEFT_RENDER_EXTERNAL_ABI_VERSION, minimap11.Get(), 0};
        weft_render_handle portrait_ref{};
        weft_render_handle minimap_ref{};
        ok(import(reference.renderer, &p11, sizeof(p11), 4, 4, 1, portrait_ref), "import portrait");
        ok(import(reference.renderer, &m11, sizeof(m11), 8, 6, 2, minimap_ref), "import minimap");

        const auto ours = prepare(d3d12.renderer, frame, {bound("portrait", portrait), bound("minimap", minimap)});
        const auto theirs =
            prepare(reference.renderer, frame, {bound("portrait", portrait_ref), bound("minimap", minimap_ref)});
        weft_view_frame_release(frame);
        const auto target = gpu.target();
        const auto drawn = d3d12.draw(ours, target.Get());
        same(drawn, reference.draw(theirs), "host textures");
        require(pixel(drawn, 50, 45) != game && pixel(drawn, 240, 40) == game,
                "a bound texture shows, an unbound name draws nothing");

        // A frame that binds its own target as a texture.
        const auto target_texture = gpu.target(DXGI_FORMAT_R8G8B8A8_UNORM);
        const weft_render_d3d12_texture t12{sizeof(t12), WEFT_RENDER_EXTERNAL_ABI_VERSION, target_texture.Get(), 0};
        weft_render_handle itself{};
        ok(import(d3d12.renderer, &t12, sizeof(t12), width, height, 4, itself), "import the target");
        frame = page.frame(0.032);
        const auto reading = prepare(d3d12.renderer, frame, {bound("portrait", itself)});
        weft_view_frame_release(frame);
        gpu.begin();
        weft_render_handle use{};
        require(d3d12.encode(reading, d3d12.payload(target_texture.Get()), use, WEFT_RENDER_TARGET_LINEAR_RGBA8) ==
                    WEFT_RENDER_INVALID_STATE,
                "a pass does not draw into a texture its frame binds");
        require(SUCCEEDED(gpu.list->Close()), "close");

        ok(weft_renderer_release_prepared(d3d12.renderer, reading), "release");
        ok(weft_renderer_release_prepared(d3d12.renderer, ours), "release");
        ok(weft_renderer_release_prepared(reference.renderer, theirs), "release");
        for (const auto h : {portrait, minimap, itself}) {
            ok(weft_renderer_release_texture(d3d12.renderer, h), "release texture");
        }
        for (const auto h : {portrait_ref, minimap_ref}) {
            ok(weft_renderer_release_texture(reference.renderer, h), "release texture");
        }
        std::cout << "PASS D3D12 host textures as D3D11 draws them\n";
    }
} // namespace

int main() {
    try {
        D3D12 gpu;
        if (!gpu.open()) {
            std::cout << "SKIP no D3D12 WARP device\n";
            return skipped;
        }
        frames(gpu);
        textures(gpu);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << "\n";
        return 1;
    }
}
