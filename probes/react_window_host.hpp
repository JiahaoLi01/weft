#pragma once

#include "probe_common.hpp"
#include "probe_constants.hpp"

#include <dxgi1_5.h>

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

    // Presentation choice for every window this probe process opens; --no-vsync on the command line turns vertical
    // sync off, and --no-validation the API validation of the windows: the Vulkan validation layer, and the D3D11 debug
    // layer of the gallery window, as a shipped host runs them.
    struct WindowPresentSettings {
        bool vsync = true;
        bool validation = true;
    };

    inline WindowPresentSettings& window_present_settings() {
        static WindowPresentSettings settings;
        return settings;
    }

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
        // The HUD's display list and the views of its coverage assets.
        std::shared_ptr<const weft::paint::DisplayList> overlay_list;
        std::vector<ComPtr<ID3D11ShaderResourceView>> overlay_assets;
        std::shared_ptr<const std::vector<ComPtr<ID3D11ShaderResourceView>>> overlay_lease_;
        unsigned overlay_updates{};
        // Low latency: a frame-latency waitable swapchain with one queued frame. wait_for_frame() returns when DXGI
        // accepts the next frame, so each frame starts right after the previous one is taken and samples fresh input.
        HANDLE frame_latency{};
        UINT buffer_count = weft::probe_constants::swap_chain_buffer_count;
        UINT flags{};
        // Vertical sync. Off presents with sync interval 0 and, when the swapchain allows it, tearing, so the frame
        // rate is not capped at the display refresh rate. The gallery windows toggle it with F7.
        bool vsync = window_present_settings().vsync;
        // The swapchain was created with DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING (flip model, OS support).
        bool tearing{};

        WindowPresenter(Experiment& r, HWND hwnd, bool low_latency = false) : renderer(r) {
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
            if (low_latency) {
                buffer_count = weft::probe_constants::low_latency_buffer_count;
                flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            }
            ComPtr<IDXGIFactory5> tearing_factory;
            BOOL allow_tearing = FALSE;
            if (SUCCEEDED(factory.As(&tearing_factory)) &&
                SUCCEEDED(tearing_factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing,
                                                               sizeof(allow_tearing))) &&
                allow_tearing) {
                flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
            }
            desc.BufferCount = buffer_count;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            desc.Scaling = DXGI_SCALING_STRETCH;
            desc.Flags = flags;
            auto created = factory->CreateSwapChainForHwnd(r.device.Get(), hwnd, &desc, nullptr, nullptr, &swap);
            if (FAILED(created) && (flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)) {
                // Without tearing, vsync-off presents still do not wait, but the compositor shows one per refresh.
                flags &= ~UINT(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
                desc.Flags = flags;
                created = factory->CreateSwapChainForHwnd(r.device.Get(), hwnd, &desc, nullptr, nullptr, &swap);
            }
            check_hresult(created, "CreateSwapChainForHwnd");
            tearing = (flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
            if (low_latency) {
                ComPtr<IDXGISwapChain2> latency;
                check_hresult(swap.As(&latency), "IDXGISwapChain2");
                check_hresult(latency->SetMaximumFrameLatency(weft::probe_constants::low_latency_queued_frames),
                              "SetMaximumFrameLatency");
                frame_latency = latency->GetFrameLatencyWaitableObject();
                require(frame_latency != nullptr, "frame latency waitable object");
            }
            check_hresult(factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER), "window association");
        }

        ~WindowPresenter() {
            if (frame_latency) {
                CloseHandle(frame_latency);
            }
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
            check_hresult(swap->ResizeBuffers(buffer_count, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, flags),
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

        // Blocks until the swapchain accepts another frame (low latency only); bounded so a hidden or
        // occluded window cannot stall the loop.
        void wait_for_frame() {
            if (frame_latency) {
                WaitForSingleObjectEx(frame_latency, weft::probe_constants::frame_latency_wait_ms, TRUE);
            }
        }

        void overlay(const FramePacket& packet) {
            require(!renderer.submitted || renderer.acknowledged, "replace overlay only after actual host completion");
            require(packet.list && packet.list->assets.size() < weft::probe_constants::max_frame_assets &&
                        packet.list->items.size() < weft::probe_constants::overlay_max_draws,
                    "bounded diagnostic overlay");
            std::size_t bytes{};
            for (const auto& asset : packet.list->assets) {
                require(asset && asset->bytes.size() <= weft::probe_constants::overlay_max_bytes - bytes,
                        "diagnostic overlay byte bound");
                bytes += asset->bytes.size();
            }
            std::vector<ComPtr<ID3D11ShaderResourceView>> assets;
            for (const auto& value : packet.list->assets) {
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
            overlay_list = packet.list;
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
            const float background[]{0, 0, 0, 1};
            context->ClearRenderTargetView(rtv.Get(), background);
            const auto viewport =
                weft::Viewport::fit(float(width), float(height), float(renderer.width), float(renderer.height));
            // The UI output letterboxed into the window, and the HUD over it at the same scale. The output is the
            // list's external texture, after the HUD's coverage assets.
            weft::paint::DisplayList window;
            window.external_textures = 1;
            window.images.emplace_back();
            auto& output = window.items.emplace_back();
            output.kind = weft::paint::Item::Kind::Image;
            output.transform = {1, 0, 0, 1, viewport.x, viewport.y};
            output.clip = {0, 0, float(width), float(height)};
            output.width = viewport.width;
            output.height = viewport.height;
            if (overlay_list) {
                require(bool(weft::paint::append(window, *overlay_list,
                                                 {viewport.scale, 0, 0, viewport.scale, viewport.x, viewport.y})),
                        "HUD over the window");
            }
            std::vector<ImageView> images;
            for (const auto& view : overlay_assets) {
                images.push_back({view.Get()});
            }
            images.push_back({renderer.output.srv.Get()});
            renderer.record(context, window, rtv.Get(), width, height, images);
            context->ClearState();
            if (capture) {
                context->CopyResource(readback.Get(), back.Get());
            }
            const DXGI_PRESENT_PARAMETERS parameters{};
            const auto interval = static_cast<UINT>(vsync ? weft::probe_constants::present_sync_interval
                                                          : weft::probe_constants::present_immediate_interval);
            const auto result =
                swap->Present1(interval, !vsync && tearing ? DXGI_PRESENT_ALLOW_TEARING : 0, &parameters);
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
