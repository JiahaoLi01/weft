#include "cheetah_alert_probe.hpp"
#include "cheetah_flow_window_probe.hpp"
#include "cheetah_probe.hpp"
#include "probe_modes.hpp"
#include "react_window_host.hpp"
#include "scroll_timing.hpp"
#include "vulkan_window_host.hpp"

namespace probes {
    // The 4K CaseCheetah flow window on Vulkan: the same React app, layout and display lists as
    // run_cheetah_flow_window, drawn by the Vulkan renderer through its C ABI into a Vulkan swapchain
    // (VulkanWindowPresenter). Interactive; a hidden self test of React input, wheel scrolling, the letterbox, resize
    // and the 4K pixel oracle; or the hidden performance run of the D3D11 window's wheel workload.
    int run_cheetah_flow_window_vulkan(bool self_test, const std::filesystem::path& folder, bool performance) {
        std::filesystem::create_directories(folder);
        vk::DeviceOptions options;
        options.presentation = true;
        options.validation = window_present_settings().validation;
        std::string unavailable;
        const auto device = vk::Device::open(options, unavailable);
        if (!device) {
            std::cout << "SKIP Vulkan flow window: " << unavailable << '\n';
            return weft::probe_constants::skipped_exit_code;
        }
        NativeReactWindow window(!self_test, true);
        require(SetWindowTextW(window.state.hwnd, L"CaseElephant - React / native Vulkan") != 0, "Vulkan title");
        VulkanWindowPresenter presenter(*device, window.state.hwnd, weft::constants::uhd_width,
                                        weft::constants::uhd_height, window_present_settings().vsync);
        auto& gpu = presenter.renderer;
        const UINT os_lines = window.state.wheel_lines;
        const UINT os_chars = window.state.wheel_chars;
        if (self_test) {
            window.state.wheel_lines = weft::probe_constants::default_wheel_scroll_lines;
            window.state.wheel_chars = weft::probe_constants::default_wheel_scroll_chars;
        } // Local fixture policy, never changes OS settings.
        const auto images = load_cheetah_images();
        CheetahPacketCache packet_cache;
        weft::FontSet fonts({WEFT_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf",
                             WEFT_SOURCE_DIR "/third_party/noto-cjk/source/NotoSansCJKsc-Regular.otf"});
        weft::ScriptConfig config{weft::constants::script::probe_vm_stack_bytes};
        weft::ScriptDom script(config);
        weft::install_script_platform(script, weft::ScriptPlatformModule::WebHost);
        script.eval(read_file(WEFT_SOURCE_DIR "/examples/cheetah-alert/native/app.js"));
        pump_react(script, 0);
        const bool motion = !self_test;
        if (motion) {
            script.eval(read_file(WEFT_SOURCE_DIR "/examples/cheetah-flow/motion.js"));
        }
        const auto css = read_file(WEFT_SOURCE_DIR "/examples/cheetah-flow/profile.css") +
                         (motion ? read_file(WEFT_SOURCE_DIR "/examples/cheetah-flow/motion.css") : "");
        std::optional<weft::DomSnapshot> layout_input;
        std::optional<weft::DocumentLayout> layout_result;
        std::optional<weft::DomSnapshot> style_input;
        std::optional<weft::ComputedStyle> style_result;
        auto layout_dom = [&](const auto& document) {
            // Full owning input includes epoch/generation, attributes, text, order and scroll offsets.
            const auto& html = document.children.at(0);
            if (layout_input && *layout_input == html) {
                return *layout_result;
            }
            if (!style_input || !same_style_input(*style_input, html)) {
                style_result = weft::compute_style(html, css, weft::document_style_profile());
                style_input = html;
            }
            const auto [dom, style] = alert_style_subtree(html, *style_result);
            auto result = weft::layout_document(dom, style, weft::constants::default_view_width,
                                                weft::constants::default_view_height, fonts);
            layout_input = html;
            layout_result = result;
            return result;
        };
        script.install_layout_host([&](const auto& document) {
            return layout_dom(document).elements;
        });
        auto page = [&] {
            weft::DomSnapshot document;
            document.children.push_back(script.snapshot_subtree("html"));
            return layout_dom(document);
        };
        weft::MouseRouter input(script, [&] {
            return page().page.geometry;
        });
        script.eval(
            "globalThis.windowWheels=[];document.getElementById('gallery').addEventListener('wheel',e=>windowWheels.push([e.deltaX,e.deltaY,e.clientX,e.clientY,e.screenX,e.screenY,e.shiftKey,e.ctrlKey,e.deltaMode,e.isTrusted]));");
        unsigned time{};
        unsigned frames{};
        unsigned compositions{};
        std::size_t samples{};
        int max_error{};
        float edge_error{};
        std::size_t high_ppem_glyphs{};
        std::string previous_key;
        const auto clock_start = std::chrono::steady_clock::now();
        auto seconds = [&] {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - clock_start).count();
        };
        WindowFrameRate rate;
        rate.reset(seconds());
        bool paused{};
        ScrollTimings timings;
        bool measure{};
        double startup_ms{};
        double startup_resize_ms{};
        double measurement_ms{};
        double measurement_drain_ms{};
        std::size_t measured_dom_elements{};
        std::vector<std::uint64_t> timing_serials;
        presenter.completed_timing = [&](std::uint64_t serial, double ms) {
            const auto i = std::find(timing_serials.begin(), timing_serials.end(), serial);
            if (i != timing_serials.end()) {
                timings
                    .rows[std::size_t(i - timing_serials.begin())][weft::probe_constants::scroll_timing_gpu_ui_column] =
                    ms;
            }
        };
        auto show_rate = [&](bool inactive = false) {
            std::ostringstream value;
            value << "FPS: ";
            if (inactive) {
                value << "paused";
            } else if (rate.ready) {
                value << std::fixed << std::setprecision(1) << rate.fps;
            } else {
                value << "--";
            }
            const auto title = value.str() + " | CaseElephant Vulkan | " + device->name + " | RT 3840x2160 | Client " +
                               std::to_string(window.state.width) + "x" + std::to_string(window.state.height);
            require(SetWindowTextW(window.state.hwnd, std::wstring(title.begin(), title.end()).c_str()) != 0,
                    "FPS title");
        };
        show_rate();
        auto step = [&](double next, bool capture = false) {
            const auto tick_started = seconds();
            require(std::isfinite(next) && next >= time && next < UINT_MAX, "owner frame clock bound");
            time = unsigned(next);
            window.messages();
            if (window.state.closed) {
                return;
            }
            presenter.poll();
            script.eval("__ceHost.pump(" + std::to_string(time) + ")");
            while (!window.state.input.empty()) {
                const auto value = window.state.input.front();
                window.state.input.pop_front();
                const auto point = value.viewport.logical(value.x, value.y);
                if (value.wheel) {
                    weft::WheelEvent event{value.delta_x,
                                           value.delta_y,
                                           point[0],
                                           point[1],
                                           double(value.screen_x),
                                           double(value.screen_y),
                                           double(time),
                                           value.ctrl,
                                           value.shift,
                                           value.alt,
                                           value.meta};
                    input.wheel(event);
                } else {
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
                    input.route(value.action, event);
                }
                // React commits before the next queued input takes its fresh snapshot.
                script.eval("__ceHost.pump(" + std::to_string(time) + ")");
            }
            require(script.stats().listener_exceptions == 0 && script.eval("caseCheetahAlert.errors.length") == "0",
                    "flow window runtime errors");
            if (motion) {
                script.eval("__ceMotion.tick(" + std::to_string(next) + "," +
                            (!window.state.minimized ? "true" : "false") + ")");
            }
            if (window.state.minimized) {
                if (!paused) {
                    rate.reset(seconds());
                    show_rate(true);
                    paused = true;
                }
                return;
            }
            if (paused) {
                rate.reset(seconds());
                show_rate();
                paused = false;
            }
            const auto input_finished = seconds();
            auto current = page();
            std::ostringstream key;
            key << std::setprecision(weft::probe_constants::double_round_trip_digits) << script.dump_document();
            for (const auto& g : current.elements) {
                key << '/' << g.scroll_left << ',' << g.scroll_top;
            }
            const auto layout_finished = seconds();
            if (window.state.resize) {
                presenter.resize(window.state.width, window.state.height);
                window.state.resize = false;
                window.state.repaint = true;
            }
            // Scrolling can change pixels without changing DOM serialization.
            if (key.str() != previous_key || window.state.repaint || capture) {
                auto raster = raster_flow_page(current.page, gpu.width, gpu.height);
                for (const auto& p : raster.paints) {
                    if (p.paint.kind == weft::PaintKind::Text &&
                        p.paint.font_size > weft::probe_constants::high_ppem_font_size) {
                        high_ppem_glyphs += p.paint.shaped.glyphs.size();
                    }
                }
                const auto packet = cheetah_packet(raster, fonts, images, &packet_cache);
                presenter.capture = capture;
                const auto packet_finished = seconds();
                presenter.submit(packet.list);
                if (measure) {
                    timing_serials.push_back(presenter.serial);
                    timings.rows.push_back(
                        {(input_finished - tick_started) * weft::constants::milliseconds_per_second,
                         (layout_finished - input_finished) * weft::constants::milliseconds_per_second,
                         (packet_finished - layout_finished) * weft::constants::milliseconds_per_second,
                         presenter.prepare_ms, presenter.encode_ms, presenter.present_ms, presenter.wait_ms, 0,
                         (seconds() - tick_started) * weft::constants::milliseconds_per_second});
                }
                previous_key = key.str();
                window.state.repaint = false;
                ++frames;
                if (capture) {
                    presenter.drain();
                    const auto pixels = presenter.pixels();
                    save_png(folder / ("window-" + std::to_string(frames - 1) + ".png"), pixels, presenter.width,
                             presenter.height);
                    if (presenter.width == weft::constants::uhd_width &&
                        presenter.height == weft::constants::uhd_height) {
                        // The display list's CPU samples at the render resolution, which the copy keeps at scale 1.
                        for (int y = 11; y < weft::constants::uhd_height; y += 97) {
                            for (int x = 11; x < weft::constants::uhd_width; x += 97) {
                                compare_oracle(cheetah_sample(packet, x, y),
                                               pixels[std::size_t(y) * weft::constants::uhd_width + x], max_error,
                                               edge_error);
                                ++samples;
                            }
                        }
                        require(max_error <= weft::probe_constants::cpu_oracle_tolerance &&
                                    edge_error <= weft::probe_constants::rounded_clip_edge_linear_tolerance,
                                "Vulkan 4K pixel oracle");
                        require(pixels[1000 * weft::constants::uhd_width + 8] == Pixel{0, 0, 0, 255},
                                "Vulkan 4K internal letterbox");
                        require(pixels[1000 * weft::constants::uhd_width + 300] == Pixel{89, 104, 100, 255},
                                "Vulkan 4K page background");
                    }
                }
            } else {
                presenter.capture = false;
                const auto compose_started = seconds();
                presenter.compose();
                ++compositions;
                if (measure) {
                    timing_serials.push_back(presenter.serial);
                    timings.rows.push_back(
                        {(input_finished - tick_started) * weft::constants::milliseconds_per_second,
                         (layout_finished - input_finished) * weft::constants::milliseconds_per_second, 0, 0, 0,
                         std::max(0., (seconds() - compose_started) * weft::constants::milliseconds_per_second -
                                          presenter.wait_ms),
                         presenter.wait_ms, 0, (seconds() - tick_started) * weft::constants::milliseconds_per_second});
                }
            }
            if (!presenter.last_visible) {
                rate.reset(seconds());
            } else if (rate.presented(seconds())) {
                show_rate();
            }
        };
        auto mouse = [&](UINT message, int x, int y) {
            require(PostMessageW(window.state.hwnd, message, message == WM_LBUTTONDOWN ? MK_LBUTTON : 0,
                                 MAKELPARAM(short(x), short(y))) != 0,
                    "post own flow HWND mouse");
        };
        auto click = [&](int x, int y) {
            mouse(WM_LBUTTONDOWN, x, y);
            step(++time);
            mouse(WM_LBUTTONUP, x, y);
            step(++time, self_test && !performance);
        };
        auto wheel = [&](UINT message, int delta, int x, int y) {
            POINT screen{x, y};
            require(ClientToScreen(window.state.hwnd, &screen) != 0, "test wheel screen coordinates");
            require(screen.x >= SHRT_MIN && screen.x <= SHRT_MAX && screen.y >= SHRT_MIN && screen.y <= SHRT_MAX,
                    "signed wheel coordinate range");
            require(PostMessageW(window.state.hwnd, message, MAKEWPARAM(0, short(delta)),
                                 MAKELPARAM(short(screen.x), short(screen.y))) != 0,
                    "post own flow HWND wheel");
            step(++time);
        };
        auto top = [&] {
            return std::stod(script.eval("document.querySelector('.alert-list').scrollTop"));
        };
        auto verify_scroll = [&](double expected, const char* reason) {
            require(std::abs(top() - expected) < weft::probe_constants::scroll_tolerance_px, reason);
        };
        auto pixel = [&](float x, float y) {
            const auto v = window.state.viewport();
            return std::array<int, 2>{int(std::lround(v.x + x * v.scale)), int(std::lround(v.y + y * v.scale))};
        };
        if (performance) {
            window.client_size(weft::constants::uhd_width, weft::constants::uhd_height);
            const auto cold = seconds();
            step(0);
            presenter.drain();
            startup_ms = (seconds() - cold) * weft::constants::milliseconds_per_second;
            startup_resize_ms = presenter.resize_ms;
            auto p = pixel(100, 399);
            click(p[0], p[1]);
            p = pixel(100, 104);
            click(p[0], p[1]);
            p = pixel(160, 175);
            for (int i = 0; i < weft::probe_constants::perf_warmup_frames; ++i) {
                wheel(WM_MOUSEWHEEL, i % 2 ? 60 : -60, p[0], p[1]);
            }
            measured_dom_elements = page().elements.size();
            presenter.drain();
            const auto measured = seconds();
            measure = true;
            for (int i = 0; i < weft::probe_constants::perf_sample_frames; ++i) {
                wheel(WM_MOUSEWHEEL, i % 2 ? 60 : -60, p[0], p[1]);
            }
            measure = false;
            const auto draining = seconds();
            presenter.drain();
            measurement_drain_ms = (seconds() - draining) * weft::constants::milliseconds_per_second;
            measurement_ms = (seconds() - measured) * weft::constants::milliseconds_per_second;
            require(timings.rows.size() == weft::probe_constants::perf_sample_frames,
                    "every benchmark wheel must repaint");
            verify_scroll(0, "benchmark net scroll offset");
            PostMessageW(window.state.hwnd, WM_CLOSE, 0, 0);
            step(++time);
        } else if (self_test) {
            window.client_size(weft::constants::uhd_width, weft::constants::uhd_height);
            step(0, true);
            auto logical_click = [&](float x, float y) {
                const auto p = pixel(x, y);
                click(p[0], p[1]);
            };
            logical_click(100, 399);
            logical_click(100, 104);
            require(script.eval("caseCheetahAlert.snapshot().many") == "true" &&
                        script.eval("caseCheetahAlert.snapshot().expanded") == "true",
                    "Vulkan 4K input reaches React");
            auto point = pixel(160, 175);
            wheel(WM_MOUSEWHEEL, -60, point[0], point[1]);
            verify_scroll(36, "Vulkan 4K wheel mapping");
            step(++time, true);
            const auto before = frames;
            step(++time);
            require(frames == before && compositions > 0, "idle frames present again without rebuilding the UI");
            const auto wheels = script.eval("windowWheels.length");
            wheel(WM_MOUSEWHEEL, -60, 8, 1000);
            verify_scroll(36, "Vulkan 4K internal letterbox wheel");
            require(script.eval("windowWheels.length") == wheels, "Vulkan 4K internal letterbox dispatched");
            window.client_size(1000, 420);
            step(++time, true);
            require(presenter.width == 1000 && presenter.height == 420, "Vulkan swapchain resize");
            const auto letterbox = presenter.pixels();
            require(letterbox[8 * 1000 + 8] == Pixel{0, 0, 0, 255}, "Vulkan resized letterbox");
            point = pixel(160, 175);
            wheel(WM_MOUSEWHEEL, -60, point[0], point[1]);
            verify_scroll(72, "Vulkan resized wheel mapping");
            require(high_ppem_glyphs > 0 && gpu.cache.hits > 0, "native-resolution glyph cache");
            PostMessageW(window.state.hwnd, WM_CLOSE, 0, 0);
            step(++time);
            require(window.state.closed, "Vulkan window close");
        } else {
            const auto started = std::chrono::steady_clock::now();
            step(0);
            while (!window.state.closed) {
                const auto elapsed =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                require(elapsed < UINT_MAX, "host clock bound");
                step(elapsed);
                if (!window.state.closed) {
                    const DWORD wait =
                        !window.state.minimized && presenter.last_visible ? 0 : weft::probe_constants::idle_wait_ms;
                    MsgWaitForMultipleObjectsEx(0, nullptr, wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                }
            }
        }
        presenter.drain();
        input.cancel();
        const auto final_state = script.eval("JSON.stringify(caseCheetahAlert.snapshot())");
        if (performance) {
            PROCESS_MEMORY_COUNTERS_EX memory{};
            memory.cb = sizeof(memory);
            require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                                         sizeof(memory)) != 0,
                    "performance process memory");
            std::ostringstream csv;
            csv << "input,layout,packet,prepare,encode,present,wait,gpu_ui,total\n";
            for (const auto& row : timings.rows) {
                for (std::size_t i = 0; i < row.size(); ++i) {
                    if (i) {
                        csv << ',';
                    }
                    csv << std::setprecision(weft::probe_constants::report_significant_digits) << row[i];
                }
                csv << '\n';
            }
            write_text(folder / "samples.csv", csv.str());
            std::ostringstream profile;
            profile << "{\"fixture\":\"4k-hwnd-scroll-perf-vulkan-v1\",\"api\":\"vulkan\",\"adapter\":"
                    << json_string(device->name) << ",\"present_mode\":" << int(presenter.swapchain.mode)
                    << ",\"width\":3840,\"height\":2160,\"warmup_frames\":" << weft::probe_constants::perf_warmup_frames
                    << ",\"sample_frames\":" << weft::probe_constants::perf_sample_frames
                    << ",\"dom_elements\":" << measured_dom_elements << ",\"cold_first_frame_ms\":" << startup_ms
                    << ",\"cold_first_frame_swapchain_ms\":" << startup_resize_ms
                    << ",\"peak_working_set_bytes\":" << memory.PeakWorkingSetSize
                    << ",\"private_usage_bytes\":" << memory.PrivateUsage
                    << ",\"hidden_window\":true,\"readback_in_measurement\":false"
                    << ",\"gpu_metric\":\"timestamp interval of the UI pass, excluding the copy and presentation\""
                    << ",\"total_metric\":\"owner tick through submission; completion asynchronous\""
                    << ",\"completion_wait_metric\":\"bounded queue capacity wait only\""
                    << ",\"pipeline_depth\":" << weft::probe_constants::frames_in_flight
                    << ",\"peak_in_flight\":" << presenter.peak_in_flight
                    << ",\"measurement_wall_ms\":" << measurement_ms
                    << ",\"measurement_tail_drain_ms\":" << measurement_drain_ms << ",\"completed_frames_per_second\":"
                    << weft::probe_constants::perf_sample_frames * weft::constants::milliseconds_per_second /
                           measurement_ms
                    << ",\"capacity_waits\":" << presenter.capacity_waits << ",\"timings\":" << timings.json()
                    << ",\"native_texture_creations\":" << gpu.native_creations << ",\"native_uploads\":" << gpu.uploads
                    << ",\"gpu_cache_hits\":" << gpu.cache.hits << ",\"gpu_cache_peak_bytes\":" << gpu.cache.peak_bytes
                    << ",\"atlas_placements\":" << gpu.atlas_placements << ",\"atlas_pages\":" << gpu.atlas.pages()
                    << ",\"draws\":" << gpu.draws << ",\"instances\":" << gpu.instances << '}';
            write_text(folder / "profile.json", profile.str());
        }
        write_text(folder / "native-dom.json", script.dump_document());
        if (motion) {
            script.eval("__ceMotion.dispose()");
        }
        script.eval("caseCheetahAlert.unmount()");
        pump_react(script, time);
        script.reset_document();
        script.collect_garbage();
        // Every cached and per-frame texture retires once the frames that used it completed.
        gpu.drain();
        require(gpu.native_creations == gpu.native_releases && gpu.frames_created == gpu.frames_released,
                "Vulkan textures and frame resources retired after completion");
        if (device->validation_messages) {
            std::cerr << device->validation_messages
                      << " validation messages; first: " << device->first_validation_message << '\n';
        }
        require(device->validation_messages == 0, "Vulkan validation clean");
        std::ostringstream report;
        report << "{\"fixture\":\"casecheetah-flow-window-vulkan-v1\",\"api\":\"vulkan\",\"adapter\":"
               << json_string(device->name) << ",\"validation_layer\":" << (device->validation ? "true" : "false")
               << ",\"synchronization_validation\":" << (device->synchronization ? "true" : "false")
               << ",\"validation_messages\":" << device->validation_messages
               << ",\"hidden_message_fixture\":" << (self_test ? "true" : "false") << ",\"frames\":" << frames
               << ",\"compositions\":" << compositions << ",\"final_state\":" << final_state
               << ",\"present_calls\":" << presenter.presents << ",\"resize_calls\":" << presenter.resizes
               << ",\"wheel_messages\":" << window.state.wheel_messages << ",\"os_wheel_lines\":" << os_lines
               << ",\"os_wheel_chars\":" << os_chars << ",\"native_textures_retired\":" << gpu.native_releases
               << ",\"cpu_pixel_samples\":" << samples << ",\"cpu_pixel_max_difference\":" << max_error
               << ",\"cpu_pixel_rounded_edge_max_linear_difference\":" << edge_error
               << ",\"render_width\":" << gpu.width << ",\"render_height\":" << gpu.height
               << ",\"high_ppem_glyphs\":" << high_ppem_glyphs << ",\"native_uploads\":" << gpu.uploads
               << ",\"gpu_cache_hits\":" << gpu.cache.hits << ",\"atlas_placements\":" << gpu.atlas_placements
               << ",\"pipeline_depth\":" << weft::probe_constants::frames_in_flight
               << ",\"peak_in_flight\":" << presenter.peak_in_flight << ",\"submissions\":" << presenter.submissions
               << ",\"completions\":" << presenter.completions
               << ",\"physical_mouse_verified\":false,\"mobile_verified\":false,\"passed\":true}";
        write_text(folder / "report.json", report.str());
        std::cout << "PASS Vulkan flow window on " << device->name << "; " << frames << " frames, " << compositions
                  << " compositions; " << gpu.native_releases << " textures retired; CPU max " << max_error << '\n';
        return 0;
    }
} // namespace probes
