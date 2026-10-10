#pragma once

#include "probe_common.hpp"
#include "probe_constants.hpp"

namespace probes {
    // Experimental Win32 sample host. Win32 messages only queue values. DOM/JS/layout/render run outside
    // WindowProc.

    struct WindowMouse {
        weft::MouseAction action;
        int x{};
        int y{};
        int screen_x{};
        int screen_y{};
        unsigned buttons{};
        bool ctrl{};
        bool shift{};
        bool alt{};
        bool meta{};
        weft::Viewport viewport; // Transform at message receipt, before a later resize.
        bool wheel{};
        double delta_x{};
        double delta_y{};
        std::chrono::steady_clock::time_point received = std::chrono::steady_clock::now();
    };

    struct WindowState {
        HWND hwnd{};
        UINT width{weft::constants::default_view_width};
        UINT height{weft::constants::default_view_height};
        UINT surface_width{weft::constants::default_view_width};
        UINT surface_height{weft::constants::default_view_height};
        float logical_width{weft::constants::default_view_width};
        float logical_height{weft::constants::default_view_height};
        bool closed{};
        bool close_requested{};
        bool repaint{true};
        bool resize{true};
        bool minimized{};
        bool overflow{};
        std::exception_ptr failure;
        std::deque<WindowMouse> input;
        unsigned mouse_messages{};
        unsigned capture_losses{};
        unsigned size_messages{};
        unsigned wheel_messages{};
        bool releasing_capture{};
        UINT wheel_lines{weft::probe_constants::default_wheel_scroll_lines};
        UINT wheel_chars{weft::probe_constants::default_wheel_scroll_chars};

        weft::Viewport viewport() const {
            return weft::Viewport::through_surface(float(width), float(height), float(surface_width),
                                                   float(surface_height), logical_width, logical_height);
        }

        void enqueue(WindowMouse value) {
            if (input.size() >= weft::probe_constants::max_queued_input) {
                input.clear();
                overflow = true;
                return;
            }
            if (!value.wheel && value.action == weft::MouseAction::Move && !input.empty() && !input.back().wheel &&
                input.back().action == weft::MouseAction::Move) {
                input.back() = value;
            } else {
                input.push_back(value);
            }
        }
    };

    inline LRESULT CALLBACK react_window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept {
        auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            state = static_cast<WindowState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            state->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        }
        if (!state) {
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        try {
            switch (msg) {
                case WM_GETMINMAXINFO:
                    if (state->surface_width == weft::constants::uhd_width) {
                        // The hidden 4K oracle needs an exact 2160px client even when
                        // desktop chrome leaves a shorter work area. Maximized bounds
                        // remain the OS work area; only the tracking limit is widened.
                        RECT extent{0, 0, weft::probe_constants::max_surface_dimension,
                                    weft::probe_constants::max_surface_dimension};
                        require(AdjustWindowRectExForDpi(&extent, WS_OVERLAPPEDWINDOW, FALSE, 0,
                                                         GetDpiForWindow(hwnd)) != 0,
                                "4K client tracking bounds");
                        auto* limits = reinterpret_cast<MINMAXINFO*>(lp);
                        limits->ptMaxTrackSize = {extent.right - extent.left, extent.bottom - extent.top};
                        return 0;
                    }
                    break;
                case WM_MOUSEWHEEL:
                case WM_MOUSEHWHEEL: {
                    POINT screen{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                    POINT point = screen;
                    require(ScreenToClient(hwnd, &point) != 0, "wheel signed screen-to-client coordinates");
                    WindowMouse input{weft::MouseAction::Move,
                                      point.x,
                                      point.y,
                                      screen.x,
                                      screen.y,
                                      unsigned((wp & MK_LBUTTON) != 0),
                                      bool(wp & MK_CONTROL),
                                      bool(wp & MK_SHIFT),
                                      GetKeyState(VK_MENU) < 0,
                                      GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0,
                                      state->viewport()};
                    input.wheel = true;
                    const double delta = GET_WHEEL_DELTA_WPARAM(wp) / double(WHEEL_DELTA);
                    // Sample-host logical units: line 24 px, char 8 px, page 420/640 px.
                    // Zero settings disable default movement; fractional ticks stay fractional.
                    if (msg == WM_MOUSEWHEEL) {
                        input.delta_y =
                            -delta *
                            (state->wheel_lines == WHEEL_PAGESCROLL
                                 ? weft::probe_constants::wheel_page_height_px
                                 : std::min(state->wheel_lines,
                                            static_cast<unsigned>(weft::probe_constants::max_wheel_scroll_setting)) *
                                       weft::probe_constants::wheel_line_px);
                    } else {
                        input.delta_x =
                            delta *
                            (state->wheel_chars == WHEEL_PAGESCROLL
                                 ? weft::probe_constants::wheel_page_width_px
                                 : std::min(state->wheel_chars,
                                            static_cast<unsigned>(weft::probe_constants::max_wheel_scroll_setting)) *
                                       weft::probe_constants::wheel_char_px);
                    }
                    state->enqueue(input);
                    ++state->wheel_messages;
                    return 0;
                }
                case WM_LBUTTONDOWN:
                case WM_LBUTTONUP:
                case WM_MOUSEMOVE: {
                    POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                    POINT screen = point;
                    require(ClientToScreen(hwnd, &screen) != 0, "mouse screen coordinates");
                    const auto action = msg == WM_LBUTTONDOWN ? weft::MouseAction::Down
                                        : msg == WM_LBUTTONUP ? weft::MouseAction::Up
                                                              : weft::MouseAction::Move;
                    state->enqueue({action, point.x, point.y, screen.x, screen.y, unsigned((wp & MK_LBUTTON) != 0),
                                    bool(wp & MK_CONTROL), bool(wp & MK_SHIFT), GetKeyState(VK_MENU) < 0,
                                    GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0, state->viewport()});
                    ++state->mouse_messages;
                    if (msg == WM_LBUTTONDOWN) {
                        SetCapture(hwnd);
                    }
                    if (msg == WM_LBUTTONUP && GetCapture() == hwnd) {
                        state->releasing_capture = true;
                        ReleaseCapture();
                        state->releasing_capture = false;
                    }
                    return 0;
                }
                case WM_CAPTURECHANGED:
                case WM_CANCELMODE:
                case WM_KILLFOCUS:
                    if (msg == WM_CAPTURECHANGED && state->releasing_capture) {
                        return 0;
                    }
                    ++state->capture_losses;
                    state->enqueue({weft::MouseAction::Cancel});
                    if (msg != WM_CAPTURECHANGED && GetCapture() == hwnd) {
                        state->releasing_capture = true;
                        ReleaseCapture();
                        state->releasing_capture = false;
                    }
                    return 0;
                case WM_SIZE:
                    state->width = LOWORD(lp);
                    state->height = HIWORD(lp);
                    state->minimized = wp == SIZE_MINIMIZED || !state->width || !state->height;
                    state->resize = state->repaint = true;
                    ++state->size_messages;
                    if (state->minimized) {
                        state->enqueue({weft::MouseAction::Cancel});
                    }
                    return 0;
                case WM_DPICHANGED: {
                    auto* r = reinterpret_cast<RECT*>(lp);
                    require(SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                                         SWP_NOZORDER | SWP_NOACTIVATE) != 0,
                            "DPI rectangle");
                    return 0;
                }
                case WM_PAINT: {
                    PAINTSTRUCT paint{};
                    BeginPaint(hwnd, &paint);
                    EndPaint(hwnd, &paint);
                    state->repaint = true;
                    return 0;
                }
                case WM_ERASEBKGND:
                    return 1;
                case WM_CLOSE:
                    state->close_requested = true;
                    return 0;
                case WM_DESTROY:
                    state->closed = true;
                    return 0;
                case WM_NCDESTROY:
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                    state->hwnd = nullptr;
                    break;
            }
        } catch (...) {
            state->failure = std::current_exception();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    class NativeReactWindow {
    public:
        WindowState state;

        NativeReactWindow(bool visible, bool high_resolution = false) {
            if (high_resolution) {
                state.surface_width = weft::constants::uhd_width;
                state.surface_height = weft::constants::uhd_height;
            }
            require(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != nullptr,
                    "per-monitor DPI context");
            require(SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &state.wheel_lines, 0) != 0, "read wheel lines");
            require(SystemParametersInfoW(SPI_GETWHEELSCROLLCHARS, 0, &state.wheel_chars, 0) != 0,
                    "read wheel characters");
            WNDCLASSEXW cls{sizeof(cls)};
            cls.hInstance = GetModuleHandleW(nullptr);
            cls.lpfnWndProc = react_window_proc;
            cls.lpszClassName = L"CaseElephantReactWindow";
            cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(weft::probe_constants::arrow_cursor_resource));
            atom = RegisterClassExW(&cls);
            require(atom != 0, "register native window");
            try {
                RECT r{0, 0, weft::constants::default_view_width, weft::constants::default_view_height};
                require(AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForSystem()) != 0,
                        "initial client rectangle");
                state.hwnd = CreateWindowExW(0, cls.lpszClassName, L"CaseElephant - React / native D3D11",
                                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                                             r.bottom - r.top, nullptr, nullptr, cls.hInstance, &state);
                require(state.hwnd != nullptr, "create native window");
                if (visible) {
                    ShowWindow(state.hwnd, high_resolution ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
                    UpdateWindow(state.hwnd);
                }
            } catch (...) {
                UnregisterClassW(cls.lpszClassName, cls.hInstance);
                throw;
            }
        }

        ~NativeReactWindow() {
            if (state.hwnd) {
                DestroyWindow(state.hwnd);
            }
            if (atom) {
                UnregisterClassW(L"CaseElephantReactWindow", GetModuleHandleW(nullptr));
            }
        }

        void messages() {
            MSG msg{};
            unsigned count{};
            while (count++ < weft::probe_constants::max_messages_per_pump &&
                   PeekMessageW(&msg, state.hwnd, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (state.closed) {
                    break;
                }
            }
            if (state.failure) {
                std::rethrow_exception(state.failure);
            }
            require(!state.overflow, "WEFT_INPUT_LIMIT: window queue overflow; cancel and reset sample required");
            if (state.close_requested && state.hwnd) {
                DestroyWindow(state.hwnd);
            }
        }

        void client_size(UINT w, UINT h) {
            RECT r{0, 0, LONG(w), LONG(h)};
            require(AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForWindow(state.hwnd)) != 0,
                    "resize client rectangle");
            require(SetWindowPos(state.hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != 0,
                    "test native resize");
            RECT actual{};
            require(GetClientRect(state.hwnd, &actual) != 0 && actual.right == LONG(w) && actual.bottom == LONG(h),
                    "actual client size");
        }

    private:
        ATOM atom{};
    };

    class WindowPresenter {
    public:
        Experiment& renderer;
        ComPtr<IDXGISwapChain1> swap;
        ComPtr<ID3D11Texture2D> back;
        ComPtr<ID3D11Texture2D> readback;
        ComPtr<ID3D11RenderTargetView> rtv;
        UINT width{};
        UINT height{};
        unsigned presents{};
        unsigned resizes{};
        unsigned occluded{};
        bool capture{};
        bool last_visible{};
        std::vector<Draw> overlay_draws;
        std::vector<ComPtr<ID3D11ShaderResourceView>> overlay_assets;
        std::shared_ptr<const std::vector<ComPtr<ID3D11ShaderResourceView>>> overlay_lease_;
        unsigned overlay_updates{};

        WindowPresenter(Experiment& r, HWND hwnd) : renderer(r) {
            ComPtr<IDXGIDevice> dxgi;
            check_hresult(r.device.As(&dxgi), "present DXGI device");
            ComPtr<IDXGIAdapter> adapter;
            check_hresult(dxgi->GetAdapter(&adapter), "present adapter");
            ComPtr<IDXGIFactory2> factory;
            check_hresult(adapter->GetParent(IID_PPV_ARGS(&factory)), "present DXGI factory");
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = weft::constants::default_view_width;
            desc.Height = weft::constants::default_view_height;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = weft::probe_constants::swap_chain_buffer_count;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            desc.Scaling = DXGI_SCALING_STRETCH;
            check_hresult(factory->CreateSwapChainForHwnd(r.device.Get(), hwnd, &desc, nullptr, nullptr, &swap),
                          "CreateSwapChainForHwnd");
            check_hresult(factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER), "window association");
        }

        ~WindowPresenter() {
            if (renderer.async_in_flight) {
                std::terminate(); // Pipeline must drain first.
            }
            // Drain a failed post-submit frame before releasing swapchain/backbuffer views.
            if (renderer.submitted && !renderer.acknowledged) {
                try {
                    renderer.complete();
                } catch (...) {
                    std::terminate();
                }
            }
            renderer.immediate->ClearState();
        }

        void resize(UINT w, UINT h) {
            require(!renderer.async_in_flight, "resize requires actual two-frame drain");
            require(w && h && w <= weft::probe_constants::max_surface_dimension &&
                        h <= weft::probe_constants::max_surface_dimension,
                    "bounded swapchain size");
            renderer.immediate->ClearState();
            rtv.Reset();
            back.Reset();
            readback.Reset();
            check_hresult(swap->ResizeBuffers(weft::probe_constants::swap_chain_buffer_count, w, h,
                                              DXGI_FORMAT_R8G8B8A8_UNORM, 0),
                          "ResizeBuffers after unbinding/releasing views");
            check_hresult(swap->GetBuffer(0, IID_PPV_ARGS(&back)), "swapchain backbuffer");
            D3D11_RENDER_TARGET_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            check_hresult(renderer.device->CreateRenderTargetView(back.Get(), &view, &rtv), "sRGB swapchain RTV");
            D3D11_TEXTURE2D_DESC staging{};
            back->GetDesc(&staging);
            staging.BindFlags = 0;
            staging.MiscFlags = 0;
            staging.Usage = D3D11_USAGE_STAGING;
            staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            check_hresult(renderer.device->CreateTexture2D(&staging, nullptr, &readback),
                          "swapchain diagnostic readback");
            width = w;
            height = h;
            ++resizes;
        }

        void overlay(const FramePacket& packet) {
            require(!renderer.submitted || renderer.acknowledged, "replace overlay only after actual host completion");
            require(packet.assets.size() < weft::probe_constants::max_frame_assets &&
                        packet.draws.size() < weft::probe_constants::overlay_max_draws,
                    "bounded diagnostic overlay");
            std::size_t bytes{};
            for (const auto& asset : packet.assets) {
                require(asset && asset->bytes.size() <= weft::probe_constants::overlay_max_bytes - bytes,
                        "diagnostic overlay byte bound");
                bytes += asset->bytes.size();
            }
            std::vector<ComPtr<ID3D11ShaderResourceView>> assets;
            for (const auto& value : packet.assets) {
                const auto& a = *value;
                require(a.encoding == Encoding::Coverage && a.width && a.height &&
                            a.bytes.size() == std::size_t(a.width) * a.height,
                        "overlay coverage dimensions");
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = a.width;
                desc.Height = a.height;
                desc.MipLevels = desc.ArraySize = 1;
                desc.SampleDesc.Count = 1;
                desc.Format = DXGI_FORMAT_R8_UNORM;
                desc.Usage = D3D11_USAGE_IMMUTABLE;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                const D3D11_SUBRESOURCE_DATA initial{a.bytes.data(), a.width, 0};
                ComPtr<ID3D11Texture2D> texture;
                check_hresult(renderer.device->CreateTexture2D(&desc, &initial, &texture), "HUD immutable coverage");
                ComPtr<ID3D11ShaderResourceView> srv;
                check_hresult(renderer.device->CreateShaderResourceView(texture.Get(), nullptr, &srv),
                              "HUD coverage SRV");
                assets.push_back(std::move(srv));
            }
            // Each async submission holds this immutable lease until its query.
            // Replacing the HUD cannot destroy assets of an older in-flight HUD.
            auto lease = std::make_shared<const std::vector<ComPtr<ID3D11ShaderResourceView>>>(assets);
            renderer.immediate->ClearState();
            overlay_draws = packet.draws;
            overlay_assets = std::move(assets);
            overlay_lease_ = std::move(lease);
            ++overlay_updates;
        }

        std::shared_ptr<const void> overlay_lease() const {
            return overlay_lease_;
        }

        void present() {
            auto* context = renderer.immediate.Get();
            context->ClearState();
            auto* target = rtv.Get();
            context->OMSetRenderTargets(1, &target, nullptr);
            const float background[]{0, 0, 0, 1};
            context->ClearRenderTargetView(target, background);
            const auto viewport =
                weft::Viewport::fit(float(width), float(height), float(renderer.width), float(renderer.height));
            QuadConstants constants{{viewport.x, viewport.y, viewport.width, viewport.height},
                                    {float(width), float(height)},
                                    weft::probe_constants::shader_material::texture,
                                    1,
                                    {1, 1, 1, 1},
                                    {0, 0, 1, 1},
                                    {},
                                    {}};
            context->UpdateSubresource(renderer.constants.Get(), 0, nullptr, &constants, 0, 0);
            D3D11_VIEWPORT full{0, 0, float(width), float(height), 0, 1};
            context->RSSetViewports(1, &full);
            D3D11_RECT clip{0, 0, LONG(width), LONG(height)};
            context->RSSetScissorRects(1, &clip);
            context->RSSetState(renderer.raster.Get());
            context->OMSetDepthStencilState(renderer.depth.Get(), 0);
            context->OMSetBlendState(renderer.blend.Get(), nullptr, D3D11_DEFAULT_SAMPLE_MASK);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(renderer.vs.Get(), nullptr, 0);
            context->PSSetShader(renderer.ps.Get(), nullptr, 0);
            auto* cb = renderer.constants.Get();
            context->VSSetConstantBuffers(0, 1, &cb);
            context->PSSetConstantBuffers(0, 1, &cb);
            auto* texture = renderer.output.srv.Get();
            context->PSSetShaderResources(0, 1, &texture);
            auto* sampler = renderer.sampler.Get();
            context->PSSetSamplers(0, 1, &sampler);
            context->Draw(weft::probe_constants::quad_vertex_count, 0);
            for (const auto& d : overlay_draws) {
                Color color = d.srgb;
                for (int c = 0; c < 3; ++c) {
                    color[c] = decode(color[c]) * color[3];
                }
                QuadConstants hud{{viewport.x + d.rect.x * viewport.scale, viewport.y + d.rect.y * viewport.scale,
                                   d.rect.w * viewport.scale, d.rect.h * viewport.scale},
                                  {float(width), float(height)},
                                  d.asset < 0 ? weft::probe_constants::shader_material::solid
                                              : weft::probe_constants::shader_material::coverage,
                                  d.opacity,
                                  color,
                                  d.uv,
                                  {},
                                  {}};
                context->UpdateSubresource(renderer.constants.Get(), 0, nullptr, &hud, 0, 0);
                ID3D11ShaderResourceView* asset = d.asset < 0 ? nullptr : overlay_assets.at(d.asset).Get();
                context->PSSetShaderResources(0, 1, &asset);
                context->Draw(weft::probe_constants::quad_vertex_count, 0);
            }
            context->ClearState();
            if (capture) {
                context->CopyResource(readback.Get(), back.Get());
            }
            const DXGI_PRESENT_PARAMETERS parameters{};
            const auto result = swap->Present1(weft::probe_constants::present_sync_interval, 0, &parameters);
            check_hresult(result, "Present1");
            last_visible = result != DXGI_STATUS_OCCLUDED;
            if (!last_visible) {
                ++occluded;
            }
            ++presents;
        }

        std::vector<Pixel> pixels() {
            require(!renderer.async_in_flight, "readback requires actual two-frame completion");
            require(capture, "readback only after captured completed frame");
            std::vector<Pixel> result(std::size_t(width) * height);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check_hresult(renderer.immediate->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                          "swapchain completed Map");
            for (UINT y = 0; y < height; ++y) {
                std::memcpy(result.data() + std::size_t(y) * width,
                            static_cast<std::uint8_t*>(mapped.pData) + std::size_t(y) * mapped.RowPitch,
                            width * sizeof(Pixel));
            }
            renderer.immediate->Unmap(readback.Get(), 0);
            return result;
        }
    };
} // namespace probes
