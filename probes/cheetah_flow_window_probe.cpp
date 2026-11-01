#include "cheetah_flow_window_probe.hpp"
#include "react_window_host.hpp"
#include "cheetah_probe.hpp"
#include "cheetah_alert_probe.hpp"
#include "scroll_timing.hpp"
#include "probe_modes.hpp"

namespace probes {
    int run_cheetah_flow_window(bool warp, bool self_test, const std::filesystem::path& folder, bool high_resolution,
                                bool performance, bool sequential, bool motion_test) {
        std::filesystem::create_directories(folder);
        NativeReactWindow window(!self_test, high_resolution);
        Experiment gpu(warp, high_resolution ? weft::constants::uhd_width : weft::constants::default_view_width,
                       high_resolution ? weft::constants::uhd_height : weft::constants::default_view_height);
        WindowPresenter presenter(gpu, window.state.hwnd);
        gpu.cache_assets = high_resolution;
        if (performance || motion_test) {
            gpu.enable_timing();
        }
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
#ifdef WEFT_REACT_GC_STRESS
        config.execution_budget_ms = weft::constants::script::max_execution_budget_ms;
#endif
        weft::ScriptDom script(config);
        weft::install_script_platform(script, weft::ScriptPlatformModule::WebHost);
        script.eval(read_file(WEFT_SOURCE_DIR "/examples/cheetah-alert/native/app.js"));
        pump_react(script, 0);
        const bool motion = motion_test || (high_resolution && !self_test);
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
            // Full owning input includes epoch/generation, attributes, text,
            // order and scroll offsets. Never reuse geometry on a dirty snapshot.
            const auto& html = document.children.at(0);
            if (high_resolution && layout_input && *layout_input == html) {
                return *layout_result;
            }
            if (!high_resolution || !style_input || !same_style_input(*style_input, html)) {
                style_result = weft::compute_style(html, css, weft::document_style_profile());
                style_input = html;
            }
            const auto& computed = *style_result;
            const auto [dom, style] = alert_style_subtree(html, computed);
            auto result = weft::layout_document(dom, style, weft::constants::default_view_width,
                                                weft::constants::default_view_height, fonts);
            if (high_resolution) {
                layout_input = html;
                layout_result = result;
            }
            return result;
        };
        script.install_layout_host([&](const auto& document) {
            return layout_dom(document).elements;
        });
        // layout_dom expects a Document. Wrap the html snapshot as an owning value.
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
        std::size_t expected_textures{};
        std::size_t samples{};
        int max_error{};
        float edge_error{};
        std::string previous_key;
        std::ostringstream captures;
        bool first = true;
        const auto clock_start = std::chrono::steady_clock::now();
        auto seconds = [&] {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - clock_start).count();
        };
        WindowFrameRate rate;
        rate.reset(seconds());
        bool paused{};
        unsigned compositions{};
        std::size_t high_ppem_glyphs{};
        ScrollTimings timings;
        bool measure{};
        double startup_ms{};
        double measurement_ms{};
        double measurement_drain_ms{};
        std::size_t measured_dom_elements{};
        std::vector<std::uint64_t> timing_serials;
        std::optional<TwoFramePipeline> pipeline;
        if (high_resolution && !sequential) {
            pipeline.emplace(gpu);
        }
        if (pipeline) {
            pipeline->completed_timing = [&](std::uint64_t serial, double ms) {
                const auto i = std::find(timing_serials.begin(), timing_serials.end(), serial);
                if (i != timing_serials.end()) {
                    timings.rows[std::size_t(i - timing_serials.begin())]
                                [weft::probe_constants::scroll_timing_gpu_ui_column] = ms;
                }
            };
        }
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
            presenter.overlay(frame_rate_overlay(fonts, value.str()));
            const auto title = value.str() + " | CaseElephant | RT 3840x2160 | Client " +
                               std::to_string(window.state.width) + "x" + std::to_string(window.state.height);
            require(SetWindowTextW(window.state.hwnd, std::wstring(title.begin(), title.end()).c_str()) != 0,
                    "FPS title");
        };
        if (high_resolution) {
            show_rate();
        }
        auto step = [&](double next, bool capture = false) {
            const auto tick_started = seconds();
            require(std::isfinite(next) && next >= time && next < UINT_MAX, "owner frame clock bound");
            time = unsigned(next);
            window.messages();
            if (window.state.closed) {
                return;
            }
            if (pipeline) {
                pipeline->poll();
            }
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
                script.eval(
                    "__ceMotion.tick(" + std::to_string(next) + "," +
                    (!window.state.minimized && (self_test || presenter.last_visible || !frames) ? "true" : "false") +
                    ")");
            }
            if (window.state.minimized) {
                if (high_resolution && !paused) {
                    rate.reset(seconds());
                    show_rate(true);
                    paused = true;
                }
                return;
            }
            if (high_resolution && paused) {
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
            // Scrolling can change pixels without changing DOM serialization.
            if (key.str() != previous_key || window.state.repaint || capture) {
                if (window.state.resize) {
                    if (pipeline) {
                        pipeline->drain();
                    }
                    presenter.resize(window.state.width, window.state.height);
                    window.state.resize = false;
                }
                auto raster = high_resolution ? raster_flow_page(current.page, gpu.width, gpu.height) : current.page;
                if (high_resolution) {
                    for (const auto& p : raster.paints) {
                        if (p.paint.kind == weft::PaintKind::Text &&
                            p.paint.font_size > weft::probe_constants::high_ppem_font_size) {
                            high_ppem_glyphs += p.paint.shaped.glyphs.size();
                        }
                    }
                }
                auto packet = cheetah_packet(raster, fonts, images, high_resolution ? &packet_cache : nullptr);
                expected_textures += packet.list->assets.size();
                presenter.capture = capture;
                const auto packet_finished = seconds();
                if (pipeline) {
                    pipeline->submit(
                        packet,
                        [&] {
                            presenter.present();
                        },
                        presenter.overlay_lease());
                } else {
                    render_window(gpu, packet, [&] {
                        presenter.present();
                    });
                }
                gpu.check_debug_messages();
                if (measure) {
                    timing_serials.push_back(gpu.window_serial);
                    timings.rows.push_back(
                        {(input_finished - tick_started) * weft::constants::milliseconds_per_second,
                         (layout_finished - input_finished) * weft::constants::milliseconds_per_second,
                         (packet_finished - layout_finished) * weft::constants::milliseconds_per_second, gpu.prepare_ms,
                         gpu.encode_ms, gpu.present_ms, gpu.wait_ms, pipeline ? 0 : gpu.gpu_ui_ms,
                         (seconds() - tick_started) * weft::constants::milliseconds_per_second});
                }
                previous_key = key.str();
                window.state.repaint = false;
                ++frames;
                if (capture) {
                    if (pipeline) {
                        pipeline->drain();
                    }
                    const auto pixels = presenter.pixels();
                    save_png(folder / ("window-" + std::to_string(frames - 1) + ".png"), pixels, presenter.width,
                             presenter.height);
                    if (!high_resolution && presenter.width == weft::constants::default_view_width &&
                        presenter.height == weft::constants::default_view_height) {
                        for (int y = 1; y < weft::constants::default_view_height; y += 7) {
                            for (int x = 1; x < weft::constants::default_view_width; x += 7) {
                                compare_oracle(cheetah_sample(packet, x, y),
                                               pixels[std::size_t(y) * weft::constants::default_view_width + x],
                                               max_error, edge_error);
                                ++samples;
                            }
                        }
                        require(max_error <= weft::probe_constants::cpu_oracle_tolerance &&
                                    edge_error <= weft::probe_constants::rounded_clip_edge_linear_tolerance,
                                "flow window CPU pixel oracle");
                    }
                    if (high_resolution && presenter.width == weft::constants::uhd_width &&
                        presenter.height == weft::constants::uhd_height) {
                        // Independent packet/sample oracle at actual raster resolution.
                        // Avoid the separate native diagnostic HUD in this comparison.
                        for (int y = 11; y < weft::constants::uhd_height; y += 97) {
                            for (int x = 11; x < weft::probe_constants::fps_hud_x; x += 97) {
                                compare_oracle(cheetah_sample(packet, x, y),
                                               pixels[std::size_t(y) * weft::constants::uhd_width + x], max_error,
                                               edge_error);
                                ++samples;
                            }
                        }
                        require(max_error <= weft::probe_constants::cpu_oracle_tolerance &&
                                    edge_error <= weft::probe_constants::rounded_clip_edge_linear_tolerance,
                                "4K native pixel oracle");
                        require(pixels[1000 * weft::constants::uhd_width + 8] == Pixel{0, 0, 0, 255},
                                "4K internal letterbox");
                        require(pixels[1000 * weft::constants::uhd_width + 300] == Pixel{89, 104, 100, 255},
                                "4K page background");
                    }
                    if (!first) {
                        captures << ',';
                    }
                    first = false;
                    captures
                        << "{\"frame\":" << frames - 1 << ",\"width\":" << presenter.width
                        << ",\"height\":" << presenter.height
                        << ",\"state\":" << script.eval("JSON.stringify(caseCheetahAlert.snapshot())")
                        << ",\"scroll_top\":"
                        << script.eval(
                               "document.querySelector('.alert-list')?document.querySelector('.alert-list').scrollTop:0");
                    if (motion) {
                        captures << ",\"motion\":" << script.eval("JSON.stringify(__ceMotion.snapshot())");
                    }
                    captures << '}';
                }
            } else if (high_resolution) {
                presenter.capture = false;
                const auto compose_started = seconds();
                if (pipeline) {
                    pipeline->compose(
                        [&] {
                            presenter.present();
                        },
                        presenter.overlay_lease());
                } else {
                    compose_window(gpu, [&] {
                        presenter.present();
                    });
                }
                ++compositions;
                if (measure) {
                    timing_serials.push_back(gpu.window_serial);
                    timings.rows.push_back(
                        {(input_finished - tick_started) * weft::constants::milliseconds_per_second,
                         (layout_finished - input_finished) * weft::constants::milliseconds_per_second, 0, 0, 0,
                         std::max(0., (seconds() - compose_started) * weft::constants::milliseconds_per_second -
                                          gpu.wait_ms),
                         gpu.wait_ms, 0, (seconds() - tick_started) * weft::constants::milliseconds_per_second});
                }
            }
            if (high_resolution) {
                if (!presenter.last_visible) {
                    rate.reset(seconds());
                } else if (rate.presented(seconds())) {
                    show_rate();
                }
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
            step(++time, !performance && !motion_test);
        };
        auto wheel = [&](UINT message, int delta, int x, int y, UINT keys = 0) {
            POINT screen{x, y};
            require(ClientToScreen(window.state.hwnd, &screen) != 0, "test wheel screen coordinates");
            require(screen.x >= SHRT_MIN && screen.x <= SHRT_MAX && screen.y >= SHRT_MIN && screen.y <= SHRT_MAX,
                    "signed wheel coordinate range");
            require(PostMessageW(window.state.hwnd, message, MAKEWPARAM(keys, short(delta)),
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
        if (motion_test) {
            window.client_size(weft::constants::uhd_width, weft::constants::uhd_height);
            const auto cold = seconds();
            step(0, true);
            startup_ms = (seconds() - cold) * weft::constants::milliseconds_per_second;
            auto live_x = [&] {
                return std::stod(script.eval("__ceMotion.snapshot().live_x"));
            };
            auto reference_x = [&] {
                return std::stod(script.eval("__ceMotion.snapshot().reference_x"));
            };
            auto expect_x = [&](double expected) {
                require(std::abs(live_x() - expected) < weft::probe_constants::scroll_tolerance_px,
                        "motion must follow elapsed time, not tick count");
                // Layout takes the inset in whole LayoutUnits, truncated, as Chrome does.
                require(script.eval("Math.abs(document.getElementById('ce-motion-live').getBoundingClientRect().x-" +
                                    std::to_string(104 + expected) + ")<" +
                                    std::to_string(1.0 / weft::constants::layout_units_per_px +
                                                   weft::probe_constants::scroll_tolerance_px)) == "true",
                        "animated inline style must reach fresh native layout");
            };
            step(10);
            expect_x(3);
            require(reference_x() == 0, "30 Hz reference must hold between samples");
            step(20);
            expect_x(6);
            require(reference_x() == 0, "extra tick must not advance held reference");
            step(40);
            expect_x(12);
            require(std::abs(reference_x() - 12) < weft::probe_constants::scroll_tolerance_px,
                    "30 Hz reference sample");
            step(44, true);
            expect_x(13.2);
            require(std::abs(reference_x() - 12) < weft::probe_constants::scroll_tolerance_px, "live vs held motion");
            auto coords = [&](float x, float y) {
                const auto vp = window.state.viewport();
                return std::array<int, 2>{int(std::lround(vp.x + x * vp.scale)), int(std::lround(vp.y + y * vp.scale))};
            };
            auto colored = [&](double x, float y, Pixel expected) {
                const auto p = coords(float(104 + x + 9), y);
                require(presenter.pixels()[std::size_t(p[1]) * weft::constants::uhd_width + p[0]] == expected,
                        "animated DOM rectangle native pixels");
            };
            colored(live_x(), 317, {230, 201, 139, 255});
            colored(reference_x(), 343, {154, 199, 176, 255});
            step(44.25);
            expect_x(13.275); // High FPS is not quantized to integral milliseconds.
            step(1600, true);
            expect_x(472); // Skipped ticks and the reversal are continuous in time.
            colored(live_x(), 317, {230, 201, 139, 255});
            require(
                script.eval(
                    "(()=>{const before=JSON.stringify(__ceMotion.snapshot());let rejected=0;for(const t of [-1,NaN,Infinity,1599]){try{__ceMotion.tick(t)}catch(e){++rejected}}return rejected===4 && JSON.stringify(__ceMotion.snapshot())===before})()") ==
                    "true",
                "motion invalid clocks must fail without mutation");
            auto p = coords(454, 295);
            click(p[0], p[1]);
            require(script.eval("__ceMotion.snapshot().running") == "false", "native pause button");
            const auto held = live_x();
            const auto held_reference = reference_x();
            step(time + 500);
            require(live_x() == held && reference_x() == held_reference, "paused clock must not move either track");
            click(p[0], p[1]);
            require(script.eval("__ceMotion.snapshot().running") == "true" && live_x() == held,
                    "resume must anchor without jump");
            p = coords(554, 295);
            click(p[0], p[1]);
            require(script.eval("__ceMotion.snapshot().speed") == "2", "native speed button");
            const auto fast_before = live_x();
            step(time + 10);
            require(std::abs(live_x() - (fast_before - 6)) < weft::probe_constants::motion_speed_tolerance_px,
                    "2x time-based speed");
            click(p[0], p[1]);
            require(script.eval("__ceMotion.snapshot().speed") == "0.5", "native half speed button");
            const auto slow_before = live_x();
            step(time + 10);
            require(std::abs(live_x() - (slow_before - 1.5)) < weft::probe_constants::motion_speed_tolerance_px,
                    "half time-based speed");
            click(p[0], p[1]);
            click(p[0], p[1]);
            require(script.eval("__ceMotion.snapshot().speed") == "2", "native speed cycle");
            PostMessageW(window.state.hwnd, WM_SIZE, SIZE_MINIMIZED, 0);
            step(++time);
            const auto hidden = live_x();
            step(time + 1000);
            require(live_x() == hidden, "minimized motion must freeze");
            window.client_size(weft::constants::uhd_width, weft::constants::uhd_height);
            PostMessageW(window.state.hwnd, WM_SIZE, SIZE_RESTORED,
                         MAKELPARAM(weft::constants::uhd_width, weft::constants::uhd_height));
            step(++time, true);
            require(!window.state.minimized && live_x() == hidden, "restore must not include hidden time");
            // The original React/store remains interactive while the demo is mounted.
            p = coords(100, 399);
            click(p[0], p[1]);
            p = coords(100, 104);
            click(p[0], p[1]);
            require(
                script.eval(
                    "caseCheetahAlert.snapshot().many==='true' && caseCheetahAlert.snapshot().expanded==='true' && !!document.getElementById('ce-motion')") ==
                    "true",
                "original React update coexists with motion");
            for (int i = 0; i < weft::probe_constants::perf_warmup_frames; ++i) {
                step(time + weft::probe_constants::motion_tick_ms);
            }
            measured_dom_elements = page().elements.size();
            if (pipeline) {
                pipeline->drain();
            }
            const auto measured = seconds();
            const auto presents_before = presenter.presents;
            measure = true;
            for (int i = 0; i < weft::probe_constants::perf_sample_frames; ++i) {
                step(time + weft::probe_constants::motion_tick_ms);
            }
            measure = false;
            const auto draining = seconds();
            if (pipeline) {
                pipeline->drain();
            }
            measurement_drain_ms = (seconds() - draining) * weft::constants::milliseconds_per_second;
            measurement_ms = (seconds() - measured) * weft::constants::milliseconds_per_second;
            require(timings.rows.size() == weft::probe_constants::perf_sample_frames &&
                        presenter.presents - presents_before == weft::probe_constants::perf_sample_frames,
                    "every motion tick must present current DOM or equal retained pixels");
            window.client_size(1000, 420);
            step(++time, true);
            require(presenter.width == 1000 && presenter.height == 420, "motion window resize");
            p = coords(454, 295);
            click(p[0], p[1]);
            require(script.eval("__ceMotion.snapshot().running") == "false", "resized native motion hit");
            click(p[0], p[1]);
            window.client_size(weft::constants::uhd_width, weft::constants::uhd_height);
            step(++time, true);
            script.eval("__ceMotion.dispose();__ceMotion.dispose()");
            const auto disposed_dom = script.dump_document();
            script.eval("__ceMotion.tick(" + std::to_string(time + 1000) + ")");
            require(script.dump_document() == disposed_dom &&
                        script.eval("document.getElementById('ce-motion')===null && __ceMotion.snapshot().disposed") ==
                            "true",
                    "disposed motion must not retain scheduled DOM work");
            PostMessageW(window.state.hwnd, WM_CLOSE, 0, 0);
            step(++time);
            require(window.state.closed, "motion close");
        } else if (performance) {
            window.client_size(weft::constants::uhd_width, weft::constants::uhd_height);
            const auto cold = seconds();
            step(0);
            if (pipeline) {
                pipeline->drain();
            }
            startup_ms = (seconds() - cold) * weft::constants::milliseconds_per_second;
            const auto vp = window.state.viewport();
            auto coords = [&](float x, float y) {
                return std::array<int, 2>{int(std::lround(vp.x + x * vp.scale)), int(std::lround(vp.y + y * vp.scale))};
            };
            auto p = coords(100, 399);
            click(p[0], p[1]);
            p = coords(100, 104);
            click(p[0], p[1]);
            p = coords(160, 175);
            for (int i = 0; i < weft::probe_constants::perf_warmup_frames; ++i) {
                wheel(WM_MOUSEWHEEL, i % 2 ? 60 : -60, p[0], p[1]);
            }
            measured_dom_elements = page().elements.size();
            if (pipeline) {
                pipeline->drain();
            }
            const auto measured = seconds();
            measure = true;
            for (int i = 0; i < weft::probe_constants::perf_sample_frames; ++i) {
                wheel(WM_MOUSEWHEEL, i % 2 ? 60 : -60, p[0], p[1]);
            }
            measure = false;
            const auto draining = seconds();
            if (pipeline) {
                pipeline->drain();
            }
            measurement_drain_ms = (seconds() - draining) * weft::constants::milliseconds_per_second;
            measurement_ms = (seconds() - measured) * weft::constants::milliseconds_per_second;
            require(timings.rows.size() == weft::probe_constants::perf_sample_frames,
                    "every benchmark wheel must repaint");
            verify_scroll(0, "benchmark net scroll offset");
            PostMessageW(window.state.hwnd, WM_CLOSE, 0, 0);
            step(++time);
        } else if (self_test && high_resolution) {
            window.client_size(weft::constants::uhd_width, weft::constants::uhd_height);
            step(0, true);
            auto pixel = [&](float x, float y) {
                const auto v = window.state.viewport();
                return std::array<int, 2>{int(std::lround(v.x + x * v.scale)), int(std::lround(v.y + y * v.scale))};
            };
            auto logical_click = [&](float x, float y) {
                const auto p = pixel(x, y);
                click(p[0], p[1]);
            };
            logical_click(100, 399);
            logical_click(100, 104);
            require(script.eval("caseCheetahAlert.snapshot().many") == "true" &&
                        script.eval("caseCheetahAlert.snapshot().expanded") == "true",
                    "4K composed input reaches React");
            auto point = pixel(160, 175);
            wheel(WM_MOUSEWHEEL, -60, point[0], point[1]);
            verify_scroll(36, "4K wheel mapping");
            step(++time, true);
            const auto before = frames;
            step(++time);
            require(frames == before && compositions > 0, "idle FPS must not rebuild UI textures");
            const auto wheels = script.eval("windowWheels.length");
            wheel(WM_MOUSEWHEEL, -60, 8, 1000);
            verify_scroll(36, "4K internal letterbox wheel");
            require(script.eval("windowWheels.length") == wheels, "4K internal letterbox dispatched");
            window.client_size(1000, 420);
            step(++time, true);
            point = pixel(160, 175);
            wheel(WM_MOUSEWHEEL, -60, point[0], point[1]);
            verify_scroll(72, "4K window resize input");
            // Replace the HUD while the last real presentation is still owned by
            // a slot, then verify retirement follows its actual completion.
            // step() can already have acknowledged its query on a fast machine.
            // Explicitly submit, then do not poll before the ownership assertion.
            if (pipeline) {
                pipeline->compose(
                    [&] {
                        presenter.present();
                    },
                    presenter.overlay_lease());
                ++compositions;
            }
            std::weak_ptr<const void> old_hud = presenter.overlay_lease();
            presenter.overlay(frame_rate_overlay(fonts, "FPS: 60.0"));
            if (pipeline) {
                require(!old_hud.expired(), "in-flight HUD lease released early");
                pipeline->drain();
                require(old_hud.expired(), "completed HUD lease retained");
            }
            require(high_ppem_glyphs > 0 && gpu.cache.hits > 0 && gpu.tracker.Stats().submitted_tokens == 0,
                    "native-resolution glyph cache completion");
            require(gpu.tracker.Stats().completed_serial ==
                        frames * weft::probe_constants::serials_per_ui_frame + compositions,
                    "host-only composition actual serial");
            step(++time);
            PostMessageW(window.state.hwnd, WM_CLOSE, 0, 0);
            step(++time);
            require(window.state.closed, "4K close");
        } else if (self_test) {
            require(SetWindowPos(window.state.hwnd, nullptr, -500, -500, 0, 0,
                                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != 0,
                    "hidden signed-coordinate fixture");
            window.client_size(weft::constants::default_view_width, weft::constants::default_view_height);
            step(0, true);
            click(100, 104);
            require(script.eval("caseCheetahAlert.snapshot().expanded") == "true", "HWND expands original AlertBar");
            click(160, 146);
            require(script.eval("caseCheetahAlert.snapshot().events[0].data.id") == "warehouse-london",
                    "HWND original facility bridge");
            click(100, 399);
            click(100, 104);
            require(script.eval("document.querySelector('.alert-list').childNodes.length") == "13",
                    "original capped list");
            const auto before_dom = script.dump_document();
            const auto before_frames = frames;
            wheel(WM_MOUSEWHEEL, -120, 160, 175, MK_SHIFT | MK_CONTROL);
            verify_scroll(72, "line-policy wheel sign and scroll");
            require(script.dump_document() == before_dom && frames == before_frames + 1,
                    "scroll-only mutation did not repaint");
            require(script.eval("JSON.stringify(windowWheels[0].slice(0,4))") == "[0,72,160,175]" &&
                        script.eval("JSON.stringify(windowWheels[0].slice(6))") == "[true,true,0,true]",
                    "HWND wheel metadata");
            require(script.eval("windowWheels[0][4]<0 && windowWheels[0][5]<0") == "true",
                    "signed negative screen coordinates");
            step(++time, true);
            script.eval(
                "document.querySelector('.alert-list').addEventListener('wheel',e=>e.preventDefault(),{once:true})");
            wheel(WM_MOUSEWHEEL, -120, 160, 175);
            verify_scroll(72, "canceled HWND wheel moved content");
            script.eval("document.querySelector('.alert-list').scrollTop=20");
            wheel(WM_MOUSEWHEEL, -1, 160, 175);
            verify_scroll(20.6, "sub-notch delta lost");
            wheel(WM_MOUSEWHEEL, -60, 160, 175);
            verify_scroll(56.6, "half-notch delta lost");
            step(++time, true);
            wheel(WM_MOUSEHWHEEL, 120, 160, 175);
            require(script.eval("windowWheels[windowWheels.length-1][0]") == "24", "horizontal positive right policy");
            verify_scroll(56.6, "horizontal wheel changed vertical offset");
            window.state.wheel_chars = WHEEL_PAGESCROLL;
            wheel(WM_MOUSEHWHEEL, 60, 160, 175);
            require(script.eval("windowWheels[windowWheels.length-1][0]") == "320", "horizontal page policy");
            window.state.wheel_chars = weft::probe_constants::default_wheel_scroll_chars;
            window.state.wheel_lines = 0;
            wheel(WM_MOUSEWHEEL, -120, 160, 175);
            verify_scroll(56.6, "zero wheel setting moved content");
            window.state.wheel_lines = WHEEL_PAGESCROLL;
            wheel(WM_MOUSEWHEEL, 120, 160, 175);
            verify_scroll(0, "page policy scroll clamp");
            window.state.wheel_lines = weft::probe_constants::default_wheel_scroll_lines;
            wheel(WM_MOUSEWHEEL, -60, 160, 175);
            verify_scroll(36, "restored line policy");
            script.eval("document.querySelector('.alert-list').scrollTop=0");
            POINT old_screen{160, 175};
            require(ClientToScreen(window.state.hwnd, &old_screen) != 0, "queued old viewport coordinates");
            SendMessageW(window.state.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, short(-60)),
                         MAKELPARAM(short(old_screen.x), short(old_screen.y)));
            window.client_size(1000, 420);
            step(++time, true);
            verify_scroll(36, "queued wheel used new resize viewport");
            require(script.eval("windowWheels[windowWheels.length-1][2]") == "160", "message receipt viewport mapping");
            const auto letterbox = presenter.pixels();
            require(letterbox[8 * 1000 + 8] == Pixel{0, 0, 0, 255}, "flow letterbox pixels");
            const auto wheel_count = script.eval("windowWheels.length");
            wheel(WM_MOUSEWHEEL, -60, 100, 175);
            verify_scroll(36, "letterbox wheel reached page");
            require(script.eval("windowWheels.length") == wheel_count, "letterbox wheel dispatched");
            wheel(WM_MOUSEWHEEL, -60, 340, 175);
            verify_scroll(72, "resized wheel coordinate mapping");
            // Pick a known original company row using fresh geometry, then deliver real HWND messages.
            const auto target = alert_button(script.snapshot_subtree("#gallery"), 5);
            const auto current = page();
            auto region = std::find_if(current.elements.begin(), current.elements.end(), [&](const auto& g) {
                return g.identity == target;
            });
            require(region != current.elements.end(), "visible original company row");
            const int logical_y = int(region->rect[1] + region->rect[3] * .5);
            const auto hit = weft::hit_page(current.page.geometry, 160, logical_y);
            require(hit && current.page.geometry.hits[*hit].identity == target,
                    "scrolled row hit before physical routing");
            click(340, logical_y);
            require(script.eval("caseCheetahAlert.snapshot().events.length") == "2" &&
                        script.eval("caseCheetahAlert.snapshot().events[1].data.id") == "company-tide",
                    "scrolled original company bridge");
            click(280, 104);
            require(script.eval("caseCheetahAlert.snapshot().expanded") == "true", "reopen scrolled list");
            script.eval("document.querySelector('.alert-list').scrollTop=72");
            click(280, 399);
            verify_scroll(0, "content shrink clamps current scroll");
            require(script.eval("document.querySelector('.alert-list').childNodes.length") == "2",
                    "shrunk original list");
            mouse(WM_LBUTTONDOWN, 280, 399);
            step(++time);
            PostMessageW(window.state.hwnd, WM_SIZE, SIZE_MINIMIZED, 0);
            step(++time);
            window.client_size(480, 640);
            step(++time, true);
            mouse(WM_LBUTTONUP, 75, 461);
            step(++time);
            require(script.eval("caseCheetahAlert.snapshot().many") == "false", "minimize canceled press");
            const auto vp = weft::Viewport::fit(480, 640);
            click(int(vp.x + 300 * vp.scale), int(vp.y + 399 * vp.scale));
            require(script.eval("caseCheetahAlert.snapshot().reply") == "回执已进入原始 store / fixture-q1",
                    "resized HWND reply to original store");
            require(gpu.native_releases == expected_textures &&
                        gpu.tracker.Stats().completed_serial == frames * weft::probe_constants::serials_per_ui_frame,
                    "flow textures retired by real query");
            PostMessageW(window.state.hwnd, WM_CLOSE, 0, 0);
            step(++time);
            require(window.state.closed, "flow window close");
        } else {
            const auto started = std::chrono::steady_clock::now();
            step(0, true);
            while (!window.state.closed) {
                const auto elapsed =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                require(elapsed < UINT_MAX, "host clock bound");
                step(elapsed);
                if (!window.state.closed) {
                    const DWORD wait = high_resolution && !window.state.minimized && presenter.last_visible
                                           ? 0
                                           : weft::probe_constants::idle_wait_ms;
                    MsgWaitForMultipleObjectsEx(0, nullptr, wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                }
            }
        }
        if (pipeline) {
            pipeline->drain();
        }
        input.cancel();
        const auto final_state = script.eval("JSON.stringify(caseCheetahAlert.snapshot())");
        const auto wheels = script.eval("JSON.stringify(windowWheels)");
        if (performance || motion_test) {
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
            profile
                << "{\"fixture\":" << json_string(motion_test ? "4k-hwnd-motion-perf-v1" : "4k-hwnd-scroll-perf-v2")
                << ",\"driver\":" << json_string(warp ? "warp" : "hardware")
                << ",\"adapter\":" << json_string(gpu.adapter)
                << ",\"width\":3840,\"height\":2160,\"warmup_frames\":8,\"sample_frames\":120,\"dom_elements\":"
                << measured_dom_elements << ",\"cold_first_frame_ms\":" << startup_ms
                << ",\"cold_first_frame_includes_readback\":" << (motion_test ? "true" : "false")
                << ",\"peak_working_set_bytes\":" << memory.PeakWorkingSetSize
                << ",\"private_usage_bytes\":" << memory.PrivateUsage
                << ",\"hidden_window\":true,\"readback_in_measurement\":false,\"gpu_metric\":\"timestamp interval for UI command list, excluding host presentation\""
                << ",\"total_metric\":"
                << json_string(pipeline ? "owner tick through submission; completion asynchronous"
                                        : "owner tick through actual completion")
                << ",\"completion_wait_metric\":"
                << json_string(pipeline ? "bounded queue capacity wait only" : "per-frame actual completion wait")
                << ",\"motion_enabled\":" << (motion ? "true" : "false")
                << ",\"synthetic_motion_tick_ms\":" << (motion_test ? weft::probe_constants::motion_tick_ms : 0)
                << ",\"pipeline_depth\":" << (pipeline ? weft::probe_constants::frames_in_flight : 1)
                << ",\"peak_in_flight\":" << (pipeline ? pipeline->peak_in_flight : 1)
                << ",\"measurement_wall_ms\":" << measurement_ms
                << ",\"measurement_tail_drain_ms\":" << measurement_drain_ms << ",\"completed_frames_per_second\":"
                << weft::probe_constants::perf_sample_frames * weft::constants::milliseconds_per_second / measurement_ms
                << ",\"capacity_waits\":" << (pipeline ? pipeline->capacity_waits : 0)
                << ",\"timings\":" << timings.json() << '}';
            // Counters distinguish cached resources from real per-frame uploads.
            auto profile_text = profile.str();
            profile_text.pop_back();
            profile_text += ",\"native_texture_creations\":" + std::to_string(gpu.native_creations) +
                            ",\"native_uploads\":" + std::to_string(gpu.uploads) +
                            ",\"gpu_cache_hits\":" + std::to_string(gpu.cache.hits) +
                            ",\"gpu_cache_peak_bytes\":" + std::to_string(gpu.cache.peak_bytes) +
                            ",\"cpu_glyph_cache_bytes\":" + std::to_string(packet_cache.glyphs.bytes) +
                            ",\"glyph_cache_hits\":" + std::to_string(packet_cache.glyphs.hits) +
                            ",\"atlas_placements\":" + std::to_string(gpu.atlas_placements) +
                            ",\"atlas_pages\":" + std::to_string(gpu.atlas.pages()) +
                            ",\"display_draws\":" + std::to_string(gpu.display_draws) +
                            ",\"display_instances\":" + std::to_string(gpu.display_instances) + "}";
            write_text(folder / "profile.json", profile_text);
        }
        const auto motion_state = motion ? script.eval("JSON.stringify(__ceMotion.snapshot())") : "null";
        write_text(folder / "native-dom.json", script.dump_document());
        if (motion) {
            script.eval("__ceMotion.dispose()");
        }
        script.eval("caseCheetahAlert.unmount()");
        pump_react(script, time);
        script.reset_document();
        script.collect_garbage();
        gpu.evict_assets(0, true);
        require(gpu.native_creations == gpu.native_releases,
                "all cached/transient textures retired after real completion");
        lifetime(gpu.tracker.Shutdown(), "flow window query shutdown");
        gpu.check_debug_messages();
        std::ostringstream report;
        report
            << "{\"fixture\":\"casecheetah-flow-window-v1\",\"driver\":" << json_string(warp ? "warp" : "hardware")
            << ",\"adapter\":" << json_string(gpu.adapter)
            << ",\"hidden_message_fixture\":" << (self_test ? "true" : "false") << ",\"frames\":" << frames
            << ",\"captures\":[" << captures.str() << "],\"wheel_events\":" << wheels
            << ",\"final_state\":" << final_state << ",\"present_calls\":" << presenter.presents
            << ",\"resize_calls\":" << presenter.resizes << ",\"mouse_messages\":" << window.state.mouse_messages
            << ",\"wheel_messages\":" << window.state.wheel_messages << ",\"os_wheel_lines\":" << os_lines
            << ",\"os_wheel_chars\":" << os_chars
            << ",\"line_pixels\":24,\"char_pixels\":8,\"native_textures_retired\":" << gpu.native_releases
            << ",\"completed_serial\":" << gpu.tracker.Stats().completed_serial << ",\"cpu_pixel_samples\":" << samples
            << ",\"cpu_pixel_max_difference\":" << max_error
            << ",\"cpu_pixel_rounded_edge_max_linear_difference\":" << edge_error << ",\"actual_completion_query\":true"
            << ",\"render_width\":" << gpu.width << ",\"render_height\":" << gpu.height
            << ",\"logical_width\":640,\"logical_height\":420,\"host_only_compositions\":" << compositions
            << ",\"high_ppem_glyphs\":" << high_ppem_glyphs << ",\"fps_samples\":" << rate.samples
            << ",\"last_present_fps\":" << rate.fps
            << ",\"fps_metric\":\"successful visible host Present calls per monotonic second\",\"hud_updates\":"
            << presenter.overlay_updates << ",\"native_texture_creations\":" << gpu.native_creations
            << ",\"native_uploads\":" << gpu.uploads << ",\"gpu_cache_hits\":" << gpu.cache.hits
            << ",\"gpu_cache_peak_bytes\":" << gpu.cache.peak_bytes
            << ",\"glyph_cache_hits\":" << packet_cache.glyphs.hits
            << ",\"motion_enabled\":" << (motion ? "true" : "false") << ",\"motion\":" << motion_state
            << ",\"pipeline_depth\":" << (pipeline ? weft::probe_constants::frames_in_flight : 1)
            << ",\"peak_in_flight\":" << (pipeline ? pipeline->peak_in_flight : 1)
            << ",\"pipeline_submissions\":" << (pipeline ? pipeline->submissions : 0)
            << ",\"pipeline_completions\":" << (pipeline ? pipeline->completions : 0)
            << ",\"pending_host_submissions\":" << gpu.async_in_flight
            << ",\"message_receipt_viewport_verified\":" << (self_test ? "true" : "false")
            << ",\"signed_screen_coordinates_verified\":" << (self_test ? "true" : "false")
            << ",\"debug_layer\":" << (gpu.debug_layer ? "true" : "false")
            << ",\"debug_warning_error_count\":0,\"physical_mouse_verified\":false,\"physical_wheel_verified\":false,\"touch_verified\":false,\"mobile_verified\":false,\"passed\":true}";
        write_text(folder / "report.json", report.str());
        std::cout << "PASS original React flow HWND mouse/wheel/resize/cancel/close " << (warp ? "WARP" : "hardware")
                  << "; " << frames << " frames; " << gpu.native_releases << " textures retired; CPU max " << max_error
                  << '\n';
        return 0;
    }
} // namespace probes
