#include "cheetah_probe.hpp"
#include "react_window_host.hpp"
#include "probe_modes.hpp"

namespace probes {
    int run_cheetah(bool warp, bool visible, const std::filesystem::path& folder) {
        std::filesystem::create_directories(folder);
        NativeReactWindow window(visible);
        SetWindowTextW(window.state.hwnd, L"CaseElephant - CaseCheetah original NineSlice / D3D11");
        Experiment gpu(warp, weft::constants::default_view_width, weft::constants::default_view_height);
        WindowPresenter presenter(gpu, window.state.hwnd);
        presenter.resize(weft::constants::default_view_width, weft::constants::default_view_height);
        presenter.capture = true;
        const auto images = load_cheetah_images();
        weft::FontSet fonts({WEFT_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf"});
        weft::ScriptConfig config{weft::constants::script::probe_vm_stack_bytes};
#ifdef WEFT_REACT_GC_STRESS
        config.execution_budget_ms = weft::constants::script::max_execution_budget_ms;
#endif
        weft::ScriptDom script(config);
        weft::install_script_platform(script, weft::ScriptPlatformModule::WebHost);
        script.eval(read_file(WEFT_SOURCE_DIR "/examples/cheetah-nine-slice/native/app.js"));
        pump_react(script, 0);
        require(script.eval("caseCheetahFixture.version") == "19.3.0", "pinned fixture React runtime");
        const auto css = read_file(WEFT_SOURCE_DIR "/examples/cheetah-nine-slice/src/profile.css");
        const auto card = script.node_identity("#card");
        const auto image = script.node_identity(".nine-slice-art");
        std::array<weft::PositionedPage, weft::probe_constants::interaction_frames> pages;
        std::array<std::string, weft::probe_constants::interaction_frames> states;
        for (int i = 0; i < weft::probe_constants::interaction_frames; ++i) {
            if (i) {
                weft::MouseEvent e{weft::MouseType::Click, 250, 368};
                require(script.dispatch_mouse(script.node_identity("#resize"), e) != weft::DispatchResult::StaleTarget,
                        "host weak-identity native MouseEvent");
                pump_react(script, i * weft::probe_constants::fixture_frame_ms);
            }
            pages[i] =
                weft::layout_document(script.snapshot_subtree("#gallery"), css, weft::constants::default_view_width,
                                      weft::constants::default_view_height, fonts)
                    .page;
            states[i] = script.eval("JSON.stringify(caseCheetahFixture.snapshot())");
            require(script.node_identity("#card") == card && script.node_identity(".nine-slice-art") == image,
                    "original component wrappers retain native identities");
        }
        require(script.eval("caseCheetahFixture.errors.length") == "0" && script.stats().listener_exceptions == 0,
                "game component React error");
        write_text(folder / "native-dom.json", script.dump_document());
        if (!visible) {
            script.eval("caseCheetahFixture.unmount()");
            pump_react(script, 64);
            script.reset_document();
            script.collect_garbage();
        }
        std::ostringstream report;
        report << "{\"fixture\":\"casecheetah-nine-slice-v1\",\"driver\":" << json_string(warp ? "warp" : "hardware")
               << ",\"adapter\":" << json_string(gpu.adapter) << ",\"frames\":[";
        int max_error{};
        std::size_t samples{};
        std::vector<Pixel> initial;
        unsigned expected_releases{};
        auto render = [&](const auto& page) {
            auto packet = cheetah_packet(page, fonts, images);
            expected_releases += unsigned(packet.assets.size());
            render_window(gpu, packet, [&] {
                presenter.present();
            });
            return std::pair{std::move(packet), presenter.pixels()};
        };
        for (int i = 0; i < weft::probe_constants::interaction_frames; ++i) {
            auto [packet, pixels] = render(pages[i]);
            unsigned image_draws{};
            for (const auto& d : packet.draws) {
                if (d.asset >= 0 && packet.assets[d.asset]->encoding == Encoding::SrgbStraight) {
                    ++image_draws;
                }
            }
            require(image_draws == 13, "original 9 card + 2 left + 2 right pieces reach GPU");
            for (int y = 3; y < weft::constants::default_view_height; y += 11) {
                for (int x = 2; x < weft::constants::default_view_width; x += 11) {
                    const auto expected = cheetah_sample(packet, x, y);
                    const auto actual = pixels[std::size_t(y) * weft::constants::default_view_width + x];
                    for (int c = 0; c < 4; ++c) {
                        max_error = std::max(max_error, std::abs(int(actual[c]) - int(expected[c])));
                    }
                    ++samples;
                }
            }
            if (i == 0) {
                initial = pixels;
            } else if (i == 1) {
                require(pixels != initial, "React resize changes real GPU pixels");
            } else {
                require(pixels == initial, "React second resize restores full-image original exactly");
            }
            save_png(folder / ("frame-" + std::to_string(i) + ".png"), pixels, weft::constants::default_view_width,
                     weft::constants::default_view_height);
            if (i) {
                report << ',';
            }
            report << "{\"state\":" << states[i] << ",\"layout\":" << page_json(pages[i].geometry)
                   << ",\"image_draws\":" << image_draws << ",\"textures\":" << packet.assets.size() << '}';
        }
        require(max_error <= weft::probe_constants::cpu_oracle_tolerance, "game PNG/crop/linear-alpha CPU oracle");
        gpu.check_debug_messages();
        require(expected_releases == gpu.native_releases, "all image/glyph resources retire after actual completion");
        report
            << "],\"original_component_verified\":true,\"original_css_rule_count\":5,\"original_png_count\":3,\"actual_completion_query\":true,\"native_textures_retired\":"
            << gpu.native_releases << ",\"cpu_pixel_samples\":" << samples
            << ",\"cpu_pixel_max_difference\":" << max_error
            << ",\"debug_layer\":" << (gpu.debug_layer ? "true" : "false")
            << ",\"debug_warning_error_count\":" << (gpu.debug_layer ? "0" : "null")
            << ",\"render_backend_abi_version\":1,\"mouse_geometry_hit_verified\":false,\"full_game_verified\":false,\"chinese_font_verified\":false,\"browser_pixel_parity_verified\":false,\"mobile_verified\":false,\"passed\":true}";
        write_text(folder / "report.json", report.str());
        std::cout << "PASS CaseCheetah original NineSlice: 3 PNGs, 13 cropped pieces, 3 frames; CPU pixel max "
                  << max_error << "; " << gpu.native_releases << " textures retired\n";
        if (visible) {
            // Own window input: this isolated scaffold has one fixed known button.
            // General positioned-context hit testing is intentionally not claimed.
            const auto started = std::chrono::steady_clock::now();
            std::string previous = script.dump_document();
            bool pressed{};
            while (!window.state.closed) {
                window.messages();
                if (window.state.closed) {
                    break;
                }
                const auto time = unsigned(std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::steady_clock::now() - started)
                                               .count()) +
                                  100;
                script.eval("__ceHost.pump(" + std::to_string(time) + ")");
                while (!window.state.input.empty()) {
                    auto v = window.state.input.front();
                    window.state.input.pop_front();
                    const auto p = v.viewport.logical(v.x, v.y);
                    const bool inside = p[0] >= 228 && p[0] < 392 && p[1] >= 354 && p[1] < 386;
                    if (v.action == weft::MouseAction::Down) {
                        pressed = inside;
                    } else if (v.action == weft::MouseAction::Cancel) {
                        pressed = false;
                    } else if (v.action == weft::MouseAction::Up) {
                        if (pressed && inside) {
                            script.dispatch_mouse(script.node_identity("#resize"),
                                                  {weft::MouseType::Click, p[0], p[1]});
                            pump_react(script, time);
                        }
                        pressed = false;
                    }
                }
                const auto dom = script.dump_document();
                if (!window.state.minimized && (dom != previous || window.state.repaint)) {
                    if (window.state.resize) {
                        presenter.resize(window.state.width, window.state.height);
                        window.state.resize = false;
                    }
                    auto page = weft::layout_document(script.snapshot_subtree("#gallery"), css,
                                                      weft::constants::default_view_width,
                                                      weft::constants::default_view_height, fonts)
                                    .page;
                    render(page);
                    previous = dom;
                    window.state.repaint = false;
                }
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(static_cast<int>(weft::probe_constants::visible_poll_ms)));
            }
        }
        return 0;
    }
} // namespace probes
