// Original React component, original Zustand store and UE protocol modules.
#include "probe_constants.hpp"

caseelephant::ProbeNodeIdentity alert_button(const caseelephant::ProbeDomSnapshot& root, int link = -1) {
    std::vector<caseelephant::ProbeNodeIdentity> found;
    auto walk = [&](auto&& self, const auto& n) -> void {
        if (n.type == caseelephant::constants::element_node_type && n.local_name == "button" &&
            (link < 0 ? n.attributes.contains("aria-expanded")
                      : n.attributes.contains("class") && n.attributes.at("class") == "link")) {
            found.push_back(n.identity);
        }
        for (const auto& child : n.children) {
            self(self, child);
        }
    };
    walk(walk, root);
    const auto index = link < 0 ? 0 : link;
    require(std::size_t(index) < found.size(), "original AlertBar button missing");
    return found[index];
}

// Style evaluation may start at html while formatting starts at #gallery.
// Ancestors pass computed values, never become synthetic layout boxes.
std::pair<caseelephant::ProbeDomSnapshot, caseelephant::ProbeComputedStyle>
alert_style_subtree(const caseelephant::ProbeDomSnapshot& dom, const caseelephant::ProbeComputedStyle& style) {
    if (dom.attributes.contains("id") && dom.attributes.at("id") == "gallery") {
        return {dom, style};
    }
    std::size_t i{};
    for (const auto& child : dom.children) {
        if (child.type == caseelephant::constants::element_node_type) {
            try {
                return alert_style_subtree(child, style.children[i]);
            } catch (const std::out_of_range&) {
            }
            ++i;
        }
    }
    throw std::out_of_range("gallery style subtree");
}

std::string alert_style_colors(const caseelephant::ProbeComputedStyle& style) {
    std::ostringstream out;
    out << '[';
    bool first = true;
    auto walk = [&](auto&& self, const auto& n) -> void {
        if (!first) {
            out << ',';
        }
        first = false;
        out << "{\"tag\":" << json_string(n.local_name);
        for (const auto* key : {"color", "background-color"}) {
            const auto value =
                n.values.contains(key)
                    ? n.values.at(key)
                    : (std::string(key) == "color" ? caseelephant::constants::default_color : "transparent");
            const auto c = caseelephant::parse_probe_css_color(value);
            out << ',' << json_string(key) << ":[" << c[0] << ',' << c[1] << ',' << c[2] << ',' << c[3] << ']';
        }
        out << '}';
        for (const auto& c : n.children) {
            self(self, c);
        }
    };
    walk(walk, style);
    out << ']';
    return out.str();
}

int run_cheetah_alert(bool warp, const std::filesystem::path& folder, bool original_style = false,
                      bool automatic = false, bool flow = false) {
    std::filesystem::create_directories(folder);
    NativeReactWindow window(false);
    Experiment gpu(warp, caseelephant::constants::default_view_width, caseelephant::constants::default_view_height);
    WindowPresenter presenter(gpu, window.state.hwnd);
    presenter.resize(caseelephant::constants::default_view_width, caseelephant::constants::default_view_height);
    presenter.capture = true;
    const auto images = load_cheetah_images();
    caseelephant::TextProbeFontSet fonts({CE_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf",
                                          CE_SOURCE_DIR "/third_party/noto-cjk/source/NotoSansCJKsc-Regular.otf"});
    caseelephant::ScriptProbeConfig config{caseelephant::constants::script::probe_vm_stack_bytes};
#ifdef CE_REACT_GC_STRESS
    config.execution_budget_ms = caseelephant::constants::script::max_execution_budget_ms;
#endif
    caseelephant::ScriptDomProbe script(config);
    script.eval(read_file(CE_SOURCE_DIR "/probes/web_host_bootstrap.js"));
    script.eval(read_file(CE_SOURCE_DIR "/examples/cheetah-alert/native/app.js"));
    pump_react(script, 0);
    const auto css = read_file(flow             ? CE_SOURCE_DIR "/examples/cheetah-flow/profile.css"
                               : automatic      ? CE_SOURCE_DIR "/examples/cheetah-auto/profile.css"
                               : original_style ? CE_SOURCE_DIR "/examples/cheetah-style/profile.css"
                                                : CE_SOURCE_DIR "/examples/cheetah-alert/src/profile.css");
    if (flow) {
        script.eval(read_file(CE_SOURCE_DIR "/examples/cheetah-flow/scroll.js"));
        script.install_layout_host([&](const auto& document) {
            auto html = document.children.at(0);
            auto computed = caseelephant::compute_style_probe(html, css, caseelephant::flow_page_style_profile());
            const auto [dom, style] = alert_style_subtree(html, computed);
            return caseelephant::build_flow_page_probe(dom, style, caseelephant::constants::default_view_width,
                                                       caseelephant::constants::default_view_height, fonts)
                .elements;
        });
    }
    std::array<caseelephant::ProbePositionedPage, caseelephant::probe_constants::alert_frames> pages;
    std::array<std::string, caseelephant::probe_constants::alert_frames> states;
    std::array<std::string, caseelephant::probe_constants::alert_frames> colors;
    std::array<double, caseelephant::probe_constants::alert_frames> scrolls{};
    const auto header = alert_button(script.snapshot_subtree("#gallery"));
    auto click = [&](const auto& identity, unsigned time) {
        require(script.dispatch_mouse(identity, {caseelephant::ProbeMouseType::Click, 60, 104}) !=
                    caseelephant::ProbeDispatchResult::StaleTarget,
                "native weak target dispatch");
        pump_react(script, time);
    };
    caseelephant::ProbeNodeIdentity removed;
    for (int i = 0; i < caseelephant::probe_constants::alert_frames; ++i) {
        if (i == 1 || i == 3 || i == 6) {
            click(header, i * caseelephant::probe_constants::fixture_frame_ms);
        } else if (i == 2 || i == 4) {
            removed = alert_button(script.snapshot_subtree("#gallery"), i == 2 ? 0 : 1);
            click(removed, i * caseelephant::probe_constants::fixture_frame_ms);
            // The detached node is no longer a valid UI target even if JS kept it alive.
            require(script.dispatch_mouse(removed, {caseelephant::ProbeMouseType::Click, 60, 144}) ==
                        caseelephant::ProbeDispatchResult::StaleTarget,
                    "unmounted list target must be rejected");
        } else if (i == 5) {
            click(script.node_identity("#many"), i * caseelephant::probe_constants::fixture_frame_ms);
        } else if (i == 7) {
            click(script.node_identity("#reply"), i * caseelephant::probe_constants::fixture_frame_ms);
        }
        if (flow && i == 6) {
            click(script.node_identity("#flow-scroll"), i * caseelephant::probe_constants::fixture_frame_ms + 1);
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
            const auto computed =
                caseelephant::compute_style_probe(dom, css,
                                                  flow        ? caseelephant::flow_page_style_profile()
                                                  : automatic ? caseelephant::auto_page_style_profile()
                                                              : caseelephant::positioned_style_profile());
            const auto [gallery, style] = alert_style_subtree(dom, computed);
            pages[i] =
                flow ? caseelephant::build_flow_page_probe(gallery, style, caseelephant::constants::default_view_width,
                                                           caseelephant::constants::default_view_height, fonts)
                           .page
                : automatic
                    ? caseelephant::build_auto_page_probe(gallery, style, caseelephant::constants::default_view_width,
                                                          caseelephant::constants::default_view_height, fonts)
                    : caseelephant::build_positioned_page_probe(gallery, style,
                                                                caseelephant::constants::default_view_width,
                                                                caseelephant::constants::default_view_height, fonts);
            colors[i] = alert_style_colors(style);
        } else {
            pages[i] = caseelephant::build_positioned_page_probe(script.snapshot_subtree("#gallery"), css,
                                                                 caseelephant::constants::default_view_width,
                                                                 caseelephant::constants::default_view_height, fonts);
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
           << ",\"driver\":" << json_string(warp ? "warp" : "hardware") << ",\"adapter\":" << json_string(gpu.adapter)
           << ",\"frames\":[";
    std::size_t samples{};
    std::size_t fallback_glyphs{};
    unsigned expected_releases{};
    int max_error{};
    std::vector<Pixel> first;
    for (int i = 0; i < caseelephant::probe_constants::alert_frames; ++i) {
        auto packet = cheetah_packet(pages[i], fonts, images);
        expected_releases += unsigned(packet.assets.size());
        for (const auto& p : pages[i].paints) {
            for (const auto& g : p.paint.shaped.glyphs) {
                require(g.glyph_id != 0, "no tofu glyph reaches GPU");
                fallback_glyphs += g.font_id == caseelephant::probe_constants::cjk_fallback_font_id;
            }
        }
        gpu.render_window(packet, [&] {
            presenter.present();
        });
        const auto pixels = presenter.pixels();
        for (int y = 1; y < caseelephant::constants::default_view_height; y += 9) {
            for (int x = 1; x < caseelephant::constants::default_view_width; x += 9) {
                const auto expected = cheetah_sample(packet, x, y);
                const auto actual = pixels[std::size_t(y) * caseelephant::constants::default_view_width + x];
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
                     y < std::min(static_cast<int>(caseelephant::constants::default_view_height),
                                  int(std::ceil(d.rect.y + d.rect.h)));
                     ++y) {
                    for (int x = std::max(0, int(std::floor(d.rect.x)));
                         x < std::min(static_cast<int>(caseelephant::constants::default_view_width),
                                      int(std::ceil(d.rect.x + d.rect.w)));
                         ++x) {
                        const auto expected = cheetah_sample(packet, x, y);
                        const auto actual = pixels[std::size_t(y) * caseelephant::constants::default_view_width + x];
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
        save_png(folder / ("frame-" + std::to_string(i) + ".png"), pixels, caseelephant::constants::default_view_width,
                 caseelephant::constants::default_view_height);
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
    caseelephant::ProbePositionedPage collision;
    caseelephant::ProbePositionedPaint background;
    background.paint.kind = caseelephant::ProbePaintKind::Rectangle;
    background.paint.width = caseelephant::constants::default_view_width;
    background.paint.height = caseelephant::constants::default_view_height;
    background.paint.srgb = {0, 0, 0, 1};
    background.clip = {0, 0, caseelephant::constants::default_view_width, caseelephant::constants::default_view_height};
    collision.paints.push_back(background);
    caseelephant::ProbePositionedPaint text;
    text.paint.kind = caseelephant::ProbePaintKind::Text;
    text.paint.x = 10;
    text.paint.y = 30;
    text.paint.font_size = 24;
    text.paint.srgb = {1, 1, 1, 1};
    text.clip = {0, 0, caseelephant::constants::default_view_width, caseelephant::constants::default_view_height};
    auto glyph = fonts.primary().shape("g", 24).glyphs.front();
    glyph.advance_x = 30;
    text.paint.shaped.glyphs.push_back(glyph);
    glyph.font_id = caseelephant::probe_constants::cjk_fallback_font_id;
    text.paint.shaped.glyphs.push_back(glyph);
    collision.paints.push_back(text);
    auto collision_packet = cheetah_packet(collision, fonts, images);
    require(collision_packet.assets.size() == 2 &&
                collision_packet.assets[0]->bytes != collision_packet.assets[1]->bytes,
            "glyph cache must include font resource identity");
    expected_releases += 2;
    gpu.render_window(collision_packet, [&] {
        presenter.present();
    });
    const auto collision_pixels = presenter.pixels();
    for (int y = 0; y < 40; ++y) {
        for (int x = 0; x < 80; ++x) {
            const auto expected = cheetah_sample(collision_packet, x, y);
            const auto actual = collision_pixels[std::size_t(y) * caseelephant::constants::default_view_width + x];
            for (int c = 0; c < 4; ++c) {
                max_error = std::max(max_error, std::abs(int(actual[c]) - int(expected[c])));
            }
            ++samples;
        }
    }
    require(fallback_glyphs > 100 && max_error <= caseelephant::probe_constants::cpu_oracle_tolerance,
            "fallback coverage/image independent CPU oracle");
    gpu.check_debug_messages();
    require(expected_releases == gpu.native_releases, "all font/image textures retire at actual GPU completion");
    report << "],\"font_measurements\":[";
    const std::array<std::string, 4> words{"office AV", "仓库", "A仓B", "仓\xcc\x81"};
    for (std::size_t i = 0; i < words.size(); ++i) {
        caseelephant::ProbeDomSnapshot n;
        n.type = caseelephant::constants::element_node_type;
        n.local_name = "div";
        n.namespace_uri = caseelephant::constants::xhtml_namespace_uri;
        n.attributes = {
            {"style",
             "width:220px;height:24px;font-family:ProbeRoboto, ProbeCjk;font-size:16px;line-height:24px;white-space:nowrap"}};
        caseelephant::ProbeDomSnapshot word_node;
        word_node.type = caseelephant::constants::text_node_type;
        word_node.data = words[i];
        n.children.push_back(word_node);
        const auto page = caseelephant::build_positioned_page_probe(n, "", 220, 24, fonts);
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
