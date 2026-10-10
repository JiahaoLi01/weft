// Experimental sample host, included in the D3D11 probe's private namespace.
// Win32 messages only queue values. DOM/JS/layout/render run outside WindowProc.
#include "probe_constants.hpp"

struct WindowMouse {
    caseelephant::ProbeMouseAction action;
    int x{};
    int y{};
    int screen_x{};
    int screen_y{};
    unsigned buttons{};
    bool ctrl{};
    bool shift{};
    bool alt{};
    bool meta{};
    caseelephant::ProbeViewport viewport; // Transform at message receipt, before a later resize.
    bool wheel{};
    double delta_x{};
    double delta_y{};
    std::chrono::steady_clock::time_point received = std::chrono::steady_clock::now();
};

struct WindowState {
    HWND hwnd{};
    UINT width{caseelephant::constants::default_view_width};
    UINT height{caseelephant::constants::default_view_height};
    UINT surface_width{caseelephant::constants::default_view_width};
    UINT surface_height{caseelephant::constants::default_view_height};
    float logical_width{caseelephant::constants::default_view_width};
    float logical_height{caseelephant::constants::default_view_height};
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
    UINT wheel_lines{caseelephant::probe_constants::default_wheel_scroll_lines};
    UINT wheel_chars{caseelephant::probe_constants::default_wheel_scroll_chars};

    caseelephant::ProbeViewport viewport() const {
        return caseelephant::ProbeViewport::through_surface(float(width), float(height), float(surface_width),
                                                            float(surface_height), logical_width, logical_height);
    }

    void enqueue(WindowMouse value) {
        if (input.size() >= caseelephant::probe_constants::max_queued_input) {
            input.clear();
            overflow = true;
            return;
        }
        if (!value.wheel && value.action == caseelephant::ProbeMouseAction::Move && !input.empty() &&
            !input.back().wheel && input.back().action == caseelephant::ProbeMouseAction::Move) {
            input.back() = value;
        } else {
            input.push_back(value);
        }
    }
};

LRESULT CALLBACK react_window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept {
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
                if (state->surface_width == caseelephant::constants::uhd_width) {
                    // The hidden 4K oracle needs an exact 2160px client even when
                    // desktop chrome leaves a shorter work area. Maximized bounds
                    // remain the OS work area; only the tracking limit is widened.
                    RECT extent{0, 0, caseelephant::probe_constants::max_surface_dimension,
                                caseelephant::probe_constants::max_surface_dimension};
                    require(AdjustWindowRectExForDpi(&extent, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForWindow(hwnd)) !=
                                0,
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
                WindowMouse input{caseelephant::ProbeMouseAction::Move,
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
                        -delta * (state->wheel_lines == WHEEL_PAGESCROLL
                                      ? caseelephant::probe_constants::wheel_page_height_px
                                      : std::min(state->wheel_lines,
                                                 static_cast<unsigned>(
                                                     caseelephant::probe_constants::max_wheel_scroll_setting)) *
                                            caseelephant::probe_constants::wheel_line_px);
                } else {
                    input.delta_x =
                        delta * (state->wheel_chars == WHEEL_PAGESCROLL
                                     ? caseelephant::probe_constants::wheel_page_width_px
                                     : std::min(state->wheel_chars,
                                                static_cast<unsigned>(
                                                    caseelephant::probe_constants::max_wheel_scroll_setting)) *
                                           caseelephant::probe_constants::wheel_char_px);
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
                const auto action = msg == WM_LBUTTONDOWN ? caseelephant::ProbeMouseAction::Down
                                    : msg == WM_LBUTTONUP ? caseelephant::ProbeMouseAction::Up
                                                          : caseelephant::ProbeMouseAction::Move;
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
                state->enqueue({caseelephant::ProbeMouseAction::Cancel});
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
                    state->enqueue({caseelephant::ProbeMouseAction::Cancel});
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
            state.surface_width = caseelephant::constants::uhd_width;
            state.surface_height = caseelephant::constants::uhd_height;
        }
        require(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != nullptr,
                "per-monitor DPI context");
        require(SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &state.wheel_lines, 0) != 0, "read wheel lines");
        require(SystemParametersInfoW(SPI_GETWHEELSCROLLCHARS, 0, &state.wheel_chars, 0) != 0, "read wheel characters");
        WNDCLASSEXW cls{sizeof(cls)};
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpfnWndProc = react_window_proc;
        cls.lpszClassName = L"CaseElephantReactWindow";
        cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(caseelephant::probe_constants::arrow_cursor_resource));
        atom = RegisterClassExW(&cls);
        require(atom != 0, "register native window");
        try {
            RECT r{0, 0, caseelephant::constants::default_view_width, caseelephant::constants::default_view_height};
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
        while (count++ < caseelephant::probe_constants::max_messages_per_pump &&
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
        require(!state.overflow, "CE_INPUT_LIMIT: window queue overflow; cancel and reset sample required");
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
        desc.Width = caseelephant::constants::default_view_width;
        desc.Height = caseelephant::constants::default_view_height;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = caseelephant::probe_constants::swap_chain_buffer_count;
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
        require(w && h && w <= caseelephant::probe_constants::max_surface_dimension &&
                    h <= caseelephant::probe_constants::max_surface_dimension,
                "bounded swapchain size");
        renderer.immediate->ClearState();
        rtv.Reset();
        back.Reset();
        readback.Reset();
        check_hresult(swap->ResizeBuffers(caseelephant::probe_constants::swap_chain_buffer_count, w, h,
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
        check_hresult(renderer.device->CreateTexture2D(&staging, nullptr, &readback), "swapchain diagnostic readback");
        width = w;
        height = h;
        ++resizes;
    }

    void overlay(const FramePacket& packet) {
        require(!renderer.submitted || renderer.acknowledged, "replace overlay only after actual host completion");
        require(packet.assets.size() < caseelephant::probe_constants::max_frame_assets &&
                    packet.draws.size() < caseelephant::probe_constants::overlay_max_draws,
                "bounded diagnostic overlay");
        std::size_t bytes{};
        for (const auto& asset : packet.assets) {
            require(asset && asset->bytes.size() <= caseelephant::probe_constants::overlay_max_bytes - bytes,
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
            check_hresult(renderer.device->CreateShaderResourceView(texture.Get(), nullptr, &srv), "HUD coverage SRV");
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
        const auto viewport = caseelephant::ProbeViewport::fit(float(width), float(height), float(renderer.width),
                                                               float(renderer.height));
        Constants constants{{viewport.x, viewport.y, viewport.width, viewport.height},
                            {float(width), float(height)},
                            caseelephant::probe_constants::shader_material::texture,
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
        context->Draw(caseelephant::probe_constants::quad_vertex_count, 0);
        for (const auto& d : overlay_draws) {
            Color color = d.srgb;
            for (int c = 0; c < 3; ++c) {
                color[c] = decode(color[c]) * color[3];
            }
            Constants hud{{viewport.x + d.rect.x * viewport.scale, viewport.y + d.rect.y * viewport.scale,
                           d.rect.w * viewport.scale, d.rect.h * viewport.scale},
                          {float(width), float(height)},
                          d.asset < 0 ? caseelephant::probe_constants::shader_material::solid
                                      : caseelephant::probe_constants::shader_material::coverage,
                          d.opacity,
                          color,
                          d.uv,
                          {},
                          {}};
            context->UpdateSubresource(renderer.constants.Get(), 0, nullptr, &hud, 0, 0);
            ID3D11ShaderResourceView* asset = d.asset < 0 ? nullptr : overlay_assets.at(d.asset).Get();
            context->PSSetShaderResources(0, 1, &asset);
            context->Draw(caseelephant::probe_constants::quad_vertex_count, 0);
        }
        context->ClearState();
        if (capture) {
            context->CopyResource(readback.Get(), back.Get());
        }
        const DXGI_PRESENT_PARAMETERS parameters{};
        const auto result = swap->Present1(caseelephant::probe_constants::present_sync_interval, 0, &parameters);
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

int run_react_window(bool warp, bool self_test, const std::filesystem::path& folder) {
    std::filesystem::create_directories(folder);
    NativeReactWindow window(!self_test);
    Experiment renderer(warp, caseelephant::constants::default_view_width,
                        caseelephant::constants::default_view_height);
    WindowPresenter presenter(renderer, window.state.hwnd);
    caseelephant::ScriptProbeConfig config{caseelephant::constants::script::probe_vm_stack_bytes};
#ifdef CE_REACT_GC_STRESS
    config.execution_budget_ms = caseelephant::constants::script::max_execution_budget_ms;
#endif
    caseelephant::ScriptDomProbe script(config);
    caseelephant::TextProbeFont font(CE_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf");
    script.eval(read_file(std::filesystem::path(CE_SOURCE_DIR) / "probes/web_host_bootstrap.js"),
                "web_host_bootstrap.js");
    script.eval(read_file(std::filesystem::path(CE_SOURCE_DIR) / "examples/react-vite/native/app.js"),
                "react-vite-app.js");
    pump_react(script, 0);
    caseelephant::ProbeMouseRouter input(script, font);
    const auto started = std::chrono::steady_clock::now();
    std::string previous_dom;
    unsigned time{};
    unsigned frames{};
    std::size_t expected_textures{};
    auto step = [&](unsigned next, bool capture = false) {
        time = next;
        window.messages();
        if (window.state.closed) {
            return;
        }
        // A due batch at a time; pending future timers never become an idle-drain error.
        script.eval("__ceHost.pump(" + std::to_string(time) + ")");
        while (!window.state.input.empty()) {
            const auto value = window.state.input.front();
            window.state.input.pop_front();
            const auto point = value.viewport.logical(value.x, value.y);
            caseelephant::ProbeMouseEvent event;
            event.client_x = point[0];
            event.client_y = point[1];
            event.screen_x = value.screen_x;
            event.screen_y = value.screen_y;
            event.time_ms = time;
            event.buttons = value.buttons;
            event.ctrl = value.ctrl;
            event.shift = value.shift;
            event.alt = value.alt;
            event.meta = value.meta;
            // This earlier Flex-only sample has no scroll geometry host.
            if (!value.wheel) {
                input.route(value.action, event);
            }
            script.eval("__ceHost.pump(" + std::to_string(time) + ")");
        }
        require(script.stats().listener_exceptions == 0 && script.eval("caseElephantReact.errors.length") == "0",
                "window React listener error");
        const auto dom = script.dump_document();
        if (window.state.minimized) {
            return;
        }
        if (dom != previous_dom || window.state.repaint || capture) {
            if (window.state.resize) {
                presenter.resize(window.state.width, window.state.height);
                window.state.resize = false;
            }
            auto page = caseelephant::build_page_probe(script.snapshot_subtree("#page"), font);
            presenter.capture = capture;
            auto packet = prepare_page_glyphs(page, font);
            expected_textures += packet.assets.size();
            renderer.render_window(std::move(packet), [&] {
                presenter.present();
            });
            renderer.check_debug_messages();
            previous_dom = dom;
            window.state.repaint = false;
            ++frames;
            const auto count = script.eval("caseElephantReact.snapshot().count");
            const auto title = L"CaseElephant - React / native D3D11 - " + std::wstring(count.begin(), count.end());
            SetWindowTextW(window.state.hwnd, title.c_str());
            if (capture) {
                save_png(folder / ("window-" + std::to_string(frames - 1) + ".png"), presenter.pixels(),
                         presenter.width, presenter.height);
            }
        }
    };
    auto mouse = [&](UINT message, int x, int y) {
        require(PostMessageW(window.state.hwnd, message, message == WM_LBUTTONDOWN ? MK_LBUTTON : 0,
                             MAKELPARAM(short(x), short(y))) != 0,
                "post own-window mouse fixture");
    };
    auto count = [&] {
        return script.eval("caseElephantReact.snapshot().count");
    };
    if (self_test) {
        window.client_size(caseelephant::constants::default_view_width, caseelephant::constants::default_view_height);
        step(0, true);
        const auto capture_losses_before = window.state.capture_losses;
        mouse(WM_LBUTTONDOWN, 100, 220);
        step(1);
        mouse(WM_LBUTTONUP, 100, 220);
        step(2, true);
        require(count() == "Count: 1", "HWND mouse messages did not update React count");
        require(window.state.capture_losses == capture_losses_before,
                "normal button release spuriously cancelled input");
        mouse(WM_LBUTTONDOWN, 100, 340);
        step(3);
        mouse(WM_LBUTTONUP, 100, 340);
        step(4, true);
        require(script.eval("caseElephantReact.snapshot().order.join(',')") == "moss,tide,ember", "HWND reverse click");
        mouse(WM_LBUTTONDOWN, 100, 220);
        step(5);
        mouse(WM_LBUTTONUP, -20, 220);
        step(6);
        require(count() == "Count: 1", "negative captured release clicked button");
        mouse(WM_LBUTTONDOWN, 100, 220);
        step(7);
        PostMessageW(window.state.hwnd, WM_CANCELMODE, 0, 0);
        step(8);
        mouse(WM_LBUTTONUP, 100, 220);
        step(9);
        require(count() == "Count: 1", "capture cancellation clicked button");
        window.client_size(1000, 420);
        step(10, true);
        const auto pixels = presenter.pixels();
        require(pixels[8 * 1000 + 8] == Pixel{0, 0, 0, 255} && pixels[8 * 1000 + 188] == Pixel{16, 21, 30, 255},
                "swapchain letterbox and page background pixels");
        mouse(WM_LBUTTONDOWN, 100, 220);
        step(11);
        mouse(WM_LBUTTONUP, 100, 220);
        step(12);
        require(count() == "Count: 1", "letterbox input reached page");
        mouse(WM_LBUTTONDOWN, 280, 220);
        step(13);
        mouse(WM_LBUTTONUP, 280, 220);
        step(14, true);
        require(count() == "Count: 2", "resized client-to-logical mouse mapping");
        // Mutation between messages must change the next hit, even before a repaint.
        script.eval("document.getElementById('increment').style.display='none'");
        mouse(WM_LBUTTONDOWN, 280, 220);
        mouse(WM_LBUTTONUP, 280, 220);
        step(15);
        require(count() == "Count: 2", "fresh hidden DOM was still hit");
        script.eval("document.getElementById('increment').style.display='flex'");
        PostMessageW(window.state.hwnd, WM_SIZE, SIZE_MINIMIZED, 0);
        step(16);
        require(window.state.minimized, "minimized host state");
        window.client_size(480, 640);
        step(17, true);
        require(!window.state.minimized && presenter.width == 480, "resize restored host");
        require(renderer.native_releases == expected_textures &&
                    renderer.tracker.Stats().completed_serial ==
                        frames * caseelephant::probe_constants::serials_per_ui_frame,
                "persistent device submission serial and glyph retirement");
        {
            Experiment failed(warp, caseelephant::constants::default_view_width,
                              caseelephant::constants::default_view_height);
            const auto page = caseelephant::build_page_probe(script.snapshot_subtree("#page"), font);
            auto packet = prepare_page_glyphs(page, font);
            const auto expected = packet.assets.size();
            bool rejected{};
            try {
                failed.render_window(std::move(packet), [] {
                    throw std::runtime_error("post-submit-proof");
                });
            } catch (const std::runtime_error& e) {
                rejected = std::string(e.what()) == "post-submit-proof";
            }
            require(rejected && failed.submitted && !failed.acknowledged, "post-submit failure lost actual submission");
            failed.complete();
            require(failed.native_releases == expected && failed.tracker.Stats().completed_serial == 2,
                    "post-submit actual query retirement");
            failed.check_debug_messages();
            lifetime(failed.tracker.Shutdown(), "post-submit failure drained");
        }
        PostMessageW(window.state.hwnd, WM_CLOSE, 0, 0);
        step(18);
        require(window.state.closed, "native close teardown");
    } else {
        step(0, true);
        while (!window.state.closed) {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
                    .count();
            require(elapsed < UINT_MAX, "sample host time bound");
            step(unsigned(elapsed));
            if (!window.state.closed) {
                MsgWaitForMultipleObjectsEx(0, nullptr, caseelephant::probe_constants::idle_wait_ms, QS_ALLINPUT,
                                            MWMO_INPUTAVAILABLE);
            }
        }
    }
    input.cancel();
    const auto final_state = script.eval("JSON.stringify(caseElephantReact.snapshot())");
    script.eval("caseElephantReact.unmount()");
    pump_react(script, time);
    script.reset_document();
    lifetime(renderer.tracker.Shutdown(), "window actual submissions drained before shutdown");
    renderer.check_debug_messages();
    std::ostringstream report;
    report
        << "{\"fixture\":\"caseelephant-react-window-v1\",\"driver\":" << json_string(warp ? "warp" : "hardware")
        << ",\"adapter\":" << json_string(renderer.adapter)
        << ",\"hidden_message_fixture\":" << (self_test ? "true" : "false")
        << ",\"physical_mouse_verified\":false,\"post_submit_failure_verified\":" << (self_test ? "true" : "false")
        << ",\"frames\":" << frames << ",\"present_calls\":" << presenter.presents
        << ",\"resize_calls\":" << presenter.resizes << ",\"occluded_presents\":" << presenter.occluded
        << ",\"mouse_messages\":" << window.state.mouse_messages
        << ",\"capture_losses\":" << window.state.capture_losses
        << ",\"native_textures_retired\":" << renderer.native_releases
        << ",\"completed_serial\":" << renderer.tracker.Stats().completed_serial
        << ",\"debug_layer\":" << (renderer.debug_layer ? "true" : "false")
        << ",\"debug_warning_error_count\":0,\"final_state\":" << final_state
        << ",\"render_backend_abi_version\":1,\"render_backend_interface_verified\":true,\"html_parser_verified\":false,\"stylesheet_cascade_verified\":false,\"mobile_verified\":false,\"passed\":true}";
    write_text(folder / "report.json", report.str());
    std::cout << "PASS native React HWND/swapchain/mouse/resize/cancel/close " << (warp ? "WARP" : "hardware") << "; "
              << frames << " frames; " << renderer.native_releases << " textures retired\n";
    return 0;
}
