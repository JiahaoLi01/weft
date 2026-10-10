#include "cheetah_alert_probe.hpp"
#include "react_window_host.hpp"
#include "cheetah_probe.hpp"
#include "probe_modes.hpp"

namespace probes {
    int run_cheetah_alert(bool warp, const std::filesystem::path& folder, bool original_style, bool automatic,
                          bool flow) {
        std::filesystem::create_directories(folder);
        NativeReactWindow window(false);
        Experiment gpu(warp, weft::constants::default_view_width, weft::constants::default_view_height);
        WindowPresenter presenter(gpu, window.state.hwnd);
        presenter.resize(weft::constants::default_view_width, weft::constants::default_view_height);
        presenter.capture = true;
        const auto images = load_cheetah_images();
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
        const auto css = read_file(flow             ? WEFT_SOURCE_DIR "/examples/cheetah-flow/profile.css"
                                   : automatic      ? WEFT_SOURCE_DIR "/examples/cheetah-auto/profile.css"
                                   : original_style ? WEFT_SOURCE_DIR "/examples/cheetah-style/profile.css"
                                                    : WEFT_SOURCE_DIR "/examples/cheetah-alert/src/profile.css");
        if (flow) {
            script.eval(read_file(WEFT_SOURCE_DIR "/examples/cheetah-flow/scroll.js"));
            script.install_layout_host([&](const auto& document) {
                auto html = document.children.at(0);
                auto computed = weft::compute_style(html, css, weft::document_style_profile());
                const auto [dom, style] = alert_style_subtree(html, computed);
                return weft::layout_document(dom, style, weft::constants::default_view_width,
                                             weft::constants::default_view_height, fonts)
                    .elements;
            });
        }
        std::array<weft::PositionedPage, weft::probe_constants::alert_frames> pages;
        std::array<std::string, weft::probe_constants::alert_frames> states;
        std::array<std::string, weft::probe_constants::alert_frames> colors;
        std::array<double, weft::probe_constants::alert_frames> scrolls{};
        const auto header = alert_button(script.snapshot_subtree("#gallery"));
        auto click = [&](const auto& identity, unsigned time) {
            require(script.dispatch_mouse(identity, {weft::MouseType::Click, 60, 104}) !=
                        weft::DispatchResult::StaleTarget,
                    "native weak target dispatch");
            pump_react(script, time);
        };
        weft::NodeIdentity removed;
        for (int i = 0; i < weft::probe_constants::alert_frames; ++i) {
            if (i == 1 || i == 3 || i == 6) {
                click(header, i * weft::probe_constants::fixture_frame_ms);
            } else if (i == 2 || i == 4) {
                removed = alert_button(script.snapshot_subtree("#gallery"), i == 2 ? 0 : 1);
                click(removed, i * weft::probe_constants::fixture_frame_ms);
                // The detached node is no longer a valid UI target even if JS kept it alive.
                require(script.dispatch_mouse(removed, {weft::MouseType::Click, 60, 144}) ==
                            weft::DispatchResult::StaleTarget,
                        "unmounted list target must be rejected");
            } else if (i == 5) {
                click(script.node_identity("#many"), i * weft::probe_constants::fixture_frame_ms);
            } else if (i == 7) {
                click(script.node_identity("#reply"), i * weft::probe_constants::fixture_frame_ms);
            }
            if (flow && i == 6) {
                click(script.node_identity("#flow-scroll"), i * weft::probe_constants::fixture_frame_ms + 1);
            }
            if (flow && script.eval("!!document.querySelector('.alert-list')") == "true") {
                scrolls[i] = std::stod(script.eval("document.querySelector('.alert-list').scrollTop"));
            }
            if (flow && i == 6) {
                require(scrolls[i] == 70, "captured React list scroll");
            }
            if (flow && i == 7) {
                require(scrolls[i] == 70, "unrelated original store reply preserves list scroll");
            }
            if (original_style) {
                const auto dom = script.snapshot_subtree("html");
                const auto computed = weft::compute_style(dom, css, weft::document_style_profile());
                const auto [gallery, style] = alert_style_subtree(dom, computed);
                pages[i] = weft::layout_document(gallery, style, weft::constants::default_view_width,
                                                 weft::constants::default_view_height, fonts)
                               .page;
                colors[i] = alert_style_colors(style);
            } else {
                pages[i] =
                    weft::layout_document(script.snapshot_subtree("#gallery"), css, weft::constants::default_view_width,
                                          weft::constants::default_view_height, fonts)
                        .page;
            }
            states[i] = script.eval("JSON.stringify(caseCheetahAlert.snapshot())");
            require(alert_button(script.snapshot_subtree("#gallery")) == header,
                    "original AlertBar header retains identity");
        }
        require(script.eval("caseCheetahAlert.version") == "19.3.0" &&
                    script.eval("caseCheetahAlert.errors.length") == "0" && script.stats().listener_exceptions == 0,
                "React/store runtime errors");
        require(
            script.eval("JSON.stringify(caseCheetahAlert.snapshot().events)") ==
                "[{\"name\":\"focus\",\"data\":{\"kind\":\"facility\",\"id\":\"warehouse-london\",\"view\":\"\"}},{\"name\":\"focus\",\"data\":{\"kind\":\"company\",\"id\":\"company-tide\",\"view\":\"\"}}]",
            "original UE bridge payloads");
        require(script.eval("document.querySelector('.alert-list').childNodes.length") == "13",
                "original 12 item cap plus overflow notice");
        require(script.eval("caseCheetahAlert.snapshot().dossier") == "open" &&
                    script.eval("caseCheetahAlert.snapshot().dock") == "business",
                "actual original store transitions");
        require(script.eval("caseCheetahAlert.snapshot().reply") == "回执已进入原始 store / fixture-q1",
                "incoming UE dispatch to original Zustand store");
        write_text(folder / "native-dom.json", script.dump_document());
        script.eval("caseCheetahAlert.unmount()");
        pump_react(script, 144);
        script.reset_document();
        script.collect_garbage();
        std::ostringstream report;
        report << "{\"fixture\":"
               << json_string(flow             ? "casecheetah-flow-v1"
                              : automatic      ? "casecheetah-auto-v1"
                              : original_style ? "casecheetah-style-v1"
                                               : "casecheetah-alert-v1")
               << ",\"driver\":" << json_string(warp ? "warp" : "hardware")
               << ",\"adapter\":" << json_string(gpu.adapter) << ",\"frames\":[";
        std::size_t samples{};
        std::size_t fallback_glyphs{};
        unsigned expected_releases{};
        int max_error{};
        std::vector<Pixel> first;
        for (int i = 0; i < weft::probe_constants::alert_frames; ++i) {
            auto packet = cheetah_packet(pages[i], fonts, images);
            expected_releases += unsigned(packet.assets.size());
            for (const auto& p : pages[i].paints) {
                for (const auto& g : p.paint.shaped.glyphs) {
                    require(g.glyph_id != 0, "no tofu glyph reaches GPU");
                    fallback_glyphs += g.font_id == weft::probe_constants::cjk_fallback_font_id;
                }
            }
            render_window(gpu, packet, [&] {
                presenter.present();
            });
            const auto pixels = presenter.pixels();
            for (int y = 1; y < weft::constants::default_view_height; y += 9) {
                for (int x = 1; x < weft::constants::default_view_width; x += 9) {
                    const auto expected = cheetah_sample(packet, x, y);
                    const auto actual = pixels[std::size_t(y) * weft::constants::default_view_width + x];
                    for (int c = 0; c < 4; ++c) {
                        max_error = std::max(max_error, std::abs(int(actual[c]) - int(expected[c])));
                    }
                    ++samples;
                }
            }
            // Separate text-only oracle visits every covered glyph quad. Sampling a
            // coarse image grid alone could miss an empty/misidentified CJK glyph.
            for (const auto& d : packet.draws) {
                if (d.asset >= 0 && packet.assets[d.asset]->encoding == Encoding::Coverage) {
                    for (int y = std::max(0, int(std::floor(d.rect.y)));
                         y < std::min(static_cast<int>(weft::constants::default_view_height),
                                      int(std::ceil(d.rect.y + d.rect.h)));
                         ++y) {
                        for (int x = std::max(0, int(std::floor(d.rect.x)));
                             x < std::min(static_cast<int>(weft::constants::default_view_width),
                                          int(std::ceil(d.rect.x + d.rect.w)));
                             ++x) {
                            const auto expected = cheetah_sample(packet, x, y);
                            const auto actual = pixels[std::size_t(y) * weft::constants::default_view_width + x];
                            for (int c = 0; c < 4; ++c) {
                                max_error = std::max(max_error, std::abs(int(actual[c]) - int(expected[c])));
                            }
                            ++samples;
                        }
                    }
                }
            }
            if (i == 0) {
                first = pixels;
            } else {
                require(pixels != first, "state/Chinese text update changes actual pixels");
            }
            save_png(folder / ("frame-" + std::to_string(i) + ".png"), pixels, weft::constants::default_view_width,
                     weft::constants::default_view_height);
            if (i) {
                report << ',';
            }
            report << "{\"state\":" << states[i] << ",\"layout\":" << page_json(pages[i].geometry)
                   << ",\"textures\":" << packet.assets.size();
            if (original_style) {
                report << ",\"style_colors\":" << colors[i];
            }
            if (flow) {
                report << ",\"scroll_top\":" << scrolls[i];
            }
            report << '}';
        }
        // Both font resources can use the same glyph number for different outlines.
        // Exercise a collision deliberately; normal Han/Latin text may not hit one.
        weft::PositionedPage collision;
        weft::PositionedPaint background;
        background.paint.kind = weft::PaintKind::Rectangle;
        background.paint.width = weft::constants::default_view_width;
        background.paint.height = weft::constants::default_view_height;
        background.paint.srgb = {0, 0, 0, 1};
        background.clip = {0, 0, weft::constants::default_view_width, weft::constants::default_view_height};
        collision.paints.push_back(background);
        weft::PositionedPaint text;
        text.paint.kind = weft::PaintKind::Text;
        text.paint.x = 10;
        text.paint.y = 30;
        text.paint.font_size = 24;
        text.paint.srgb = {1, 1, 1, 1};
        text.clip = {0, 0, weft::constants::default_view_width, weft::constants::default_view_height};
        auto glyph = fonts.primary().shape("g", 24).glyphs.front();
        glyph.advance_x = 30;
        text.paint.shaped.glyphs.push_back(glyph);
        glyph.font_id = weft::probe_constants::cjk_fallback_font_id;
        text.paint.shaped.glyphs.push_back(glyph);
        collision.paints.push_back(text);
        auto collision_packet = cheetah_packet(collision, fonts, images);
        require(collision_packet.assets.size() == 2 &&
                    collision_packet.assets[0]->bytes != collision_packet.assets[1]->bytes,
                "glyph cache must include font resource identity");
        expected_releases += 2;
        render_window(gpu, collision_packet, [&] {
            presenter.present();
        });
        const auto collision_pixels = presenter.pixels();
        for (int y = 0; y < 40; ++y) {
            for (int x = 0; x < 80; ++x) {
                const auto expected = cheetah_sample(collision_packet, x, y);
                const auto actual = collision_pixels[std::size_t(y) * weft::constants::default_view_width + x];
                for (int c = 0; c < 4; ++c) {
                    max_error = std::max(max_error, std::abs(int(actual[c]) - int(expected[c])));
                }
                ++samples;
            }
        }
        require(fallback_glyphs > 100 && max_error <= weft::probe_constants::cpu_oracle_tolerance,
                "fallback coverage/image independent CPU oracle");
        gpu.check_debug_messages();
        require(expected_releases == gpu.native_releases, "all font/image textures retire at actual GPU completion");
        report << "],\"font_measurements\":[";
        const std::array<std::string, 4> words{"office AV", "仓库", "A仓B", "仓\xcc\x81"};
        for (std::size_t i = 0; i < words.size(); ++i) {
            weft::DomSnapshot n;
            n.type = weft::constants::element_node_type;
            n.local_name = "div";
            n.namespace_uri = weft::constants::xhtml_namespace_uri;
            n.attributes = {
                {"style",
                 "width:220px;height:24px;font-family:ProbeRoboto, ProbeCjk;font-size:16px;line-height:24px;white-space:nowrap"}};
            weft::DomSnapshot word_node;
            word_node.type = weft::constants::text_node_type;
            word_node.data = words[i];
            n.children.push_back(word_node);
            const auto page = weft::layout_document(n, "", 220, 24, fonts).page;
            const auto& p = page.paints.front().paint;
            if (i) {
                report << ',';
            }
            report << "{\"text\":" << json_string(words[i]) << ",\"advance\":" << p.shaped.advance
                   << ",\"baseline\":" << p.y << '}';
        }
        report
            << "],\"flow_layout_verified\":" << (flow ? "true" : "false")
            << ",\"original_theme_css_verified\":" << (original_style ? "true" : "false")
            << ",\"original_expanded_rule_verified\":" << ((original_style && !automatic) ? "true" : "false")
            << ",\"automatic_layout_verified\":" << (automatic ? "true" : "false")
            << ",\"original_component_verified\":true,\"original_store_and_bridge_verified\":true,\"authored_preview_css\":true,\"chinese_font_verified\":true,\"font_id_cache_collision_verified\":true,\"post_vm_reset_render_verified\":true,\"stale_list_target_verified\":true,\"actual_completion_query\":true,\"native_textures_retired\":"
            << gpu.native_releases << ",\"fallback_glyph_count\":" << fallback_glyphs
            << ",\"cpu_pixel_samples\":" << samples << ",\"cpu_pixel_max_difference\":" << max_error
            << ",\"debug_layer\":" << (gpu.debug_layer ? "true" : "false")
            << ",\"debug_warning_error_count\":" << (gpu.debug_layer ? "0" : "null")
            << ",\"render_backend_abi_version\":1,\"real_unreal_commands_executed\":false,\"browser_pixel_parity_verified\":false,\"full_game_verified\":false,\"mobile_verified\":false,\"passed\":true}";
        write_text(folder / "report.json", report.str());
        std::cout << "PASS original AlertBar/Zustand/UE bridge: 8 frames; " << fallback_glyphs
                  << " fallback glyphs; CPU max " << max_error << "; " << gpu.native_releases << " retired textures\n";
        return 0;
    }
} // namespace probes
