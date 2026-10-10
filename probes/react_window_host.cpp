#include "react_window_host.hpp"
#include "cheetah_probe.hpp"
#include "probe_modes.hpp"

namespace probes {
    int run_react_window(bool warp, bool self_test, const std::filesystem::path& folder) {
        std::filesystem::create_directories(folder);
        NativeReactWindow window(!self_test);
        Experiment renderer(warp, weft::constants::default_view_width, weft::constants::default_view_height);
        WindowPresenter presenter(renderer, window.state.hwnd);
        weft::ScriptConfig config{weft::constants::script::probe_vm_stack_bytes};
#ifdef WEFT_REACT_GC_STRESS
        config.execution_budget_ms = weft::constants::script::max_execution_budget_ms;
#endif
        weft::ScriptDom script(config);
        weft::FontSet fonts({WEFT_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf"});
        auto layout = [&] {
            return weft::layout_document(script.snapshot_subtree("#page"), "", weft::constants::default_view_width,
                                         weft::constants::default_view_height, fonts);
        };
        weft::install_script_platform(script, weft::ScriptPlatformModule::WebHost);
        script.eval(read_file(std::filesystem::path(WEFT_SOURCE_DIR) / "examples/react-vite/native/app.js"),
                    "react-vite-app.js");
        pump_react(script, 0);
        weft::MouseRouter input(script, [&] {
            return layout().page.geometry;
        });
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
                weft::MouseEvent event;
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
                const auto page = layout();
                presenter.capture = capture;
                auto packet = cheetah_packet(page.page, fonts, {});
                expected_textures += packet.assets.size();
                render_window(renderer, std::move(packet), [&] {
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
            window.client_size(weft::constants::default_view_width, weft::constants::default_view_height);
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
            require(script.eval("caseElephantReact.snapshot().order.join(',')") == "moss,tide,ember",
                    "HWND reverse click");
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
                            frames * weft::probe_constants::serials_per_ui_frame,
                    "persistent device submission serial and glyph retirement");
            {
                Experiment failed(warp, weft::constants::default_view_width, weft::constants::default_view_height);
                const auto page = layout();
                auto packet = cheetah_packet(page.page, fonts, {});
                const auto expected = packet.assets.size();
                bool rejected{};
                try {
                    render_window(failed, std::move(packet), [] {
                        throw std::runtime_error("post-submit-proof");
                    });
                } catch (const std::runtime_error& e) {
                    rejected = std::string(e.what()) == "post-submit-proof";
                }
                require(rejected && failed.submitted && !failed.acknowledged,
                        "post-submit failure lost actual submission");
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
                    MsgWaitForMultipleObjectsEx(0, nullptr, weft::probe_constants::idle_wait_ms, QS_ALLINPUT,
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
        std::cout << "PASS native React HWND/swapchain/mouse/resize/cancel/close " << (warp ? "WARP" : "hardware")
                  << "; " << frames << " frames; " << renderer.native_releases << " textures retired\n";
        return 0;
    }
} // namespace probes
