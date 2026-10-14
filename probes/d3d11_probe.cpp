#include "cheetah_probe.hpp"
#include "probe_common.hpp"
#include "probe_modes.hpp"
#include "react_window_host.hpp"

// Color, SDF and React fixtures, the browser capture importer and the probe mode dispatcher.
namespace probes {
    struct Check {
        const char* id;
        UINT x;
        UINT y;
        Pixel expected;
    };

    const std::array<Check, 10> checks{{{"white-half", 32, 32, {188, 188, 188, 255}},
                                        {"ordered-red-blue", 96, 32, {137, 0, 188, 255}},
                                        {"srgb-gray-input", 160, 32, {128, 128, 128, 255}},
                                        {"coverage-128", 224, 32, {188, 188, 188, 255}},
                                        {"parent-opacity-group", 32, 96, {0, 0, 188, 255}},
                                        {"premultiplied-image-edge", 96, 96, {188, 0, 0, 255}},
                                        {"actual-freetype-glyph", 160, 96, {255, 255, 255, 255}},
                                        {"glyph-exterior", 136, 72, {0, 0, 0, 255}},
                                        {"scissor-interior", 224, 96, {255, 0, 0, 255}},
                                        {"scissor-exterior", 200, 72, {0, 0, 0, 255}}}};

    int capture_browser(const std::filesystem::path& png, const std::filesystem::path& output) {
        const auto pixels = read_png(png);
        std::ostringstream report;
        report
            << "{\"fixture\":\"caseelephant-d3d11-color-v1\",\"width\":256,\"height\":128,\"source\":\"actual Chrome PNG capture\",\"samples\":[";
        bool comma{};
        for (const auto& check : checks) {
            if (comma) {
                report << ',';
            }
            comma = true;
            report << "{\"id\":" << json_string(check.id)
                   << ",\"rgba\":" << rgba(pixels[static_cast<std::size_t>(check.y) * width + check.x]) << '}';
        }
        report << "]}\n";
        std::filesystem::create_directories(output.parent_path());
        std::ofstream file(output, std::ios::binary);
        require(bool(file << report.str()), "write browser color samples");
        std::cout << "Captured " << checks.size() << " actual browser PNG samples\n";
        return 0;
    }

    int run(bool warp, const std::filesystem::path& folder) {
        std::filesystem::create_directories(folder);
        Experiment direct(warp);
        Experiment rtt(warp);
        auto direct_pixels = direct.execute(false);
        auto rtt_pixels = rtt.execute(true);
        std::ostringstream report;
        report
            << "{\"fixture\":\"caseelephant-d3d11-color-v1\",\"driver\":" << json_string(warp ? "warp" : "hardware")
            << ",\"adapter\":" << json_string(direct.adapter)
            << ",\"feature_level\":\"11_0\",\"debug_layer\":" << (direct.debug_layer ? "true" : "false")
            << ",\"width\":256,\"height\":128,\"color_contract\":\"linear-srgb-premultiplied\",\"tolerance_rgba8\":2,\"checks\":[";
        bool comma{};
        for (const auto& check : checks) {
            const auto p = direct_pixels[static_cast<std::size_t>(check.y) * width + check.x];
            const auto q = rtt_pixels[static_cast<std::size_t>(check.y) * width + check.x];
            for (std::size_t c = 0; c < 4; ++c) {
                direct.test(std::abs(int(p[c]) - int(check.expected[c])) <=
                                (c == 3 ? 0 : weft::probe_constants::rgba8_parity_tolerance),
                            check.id);
                rtt.test(std::abs(int(q[c]) - int(check.expected[c])) <=
                             (c == 3 ? 0 : weft::probe_constants::rgba8_parity_tolerance),
                         check.id);
            }
            if (comma) {
                report << ',';
            }
            comma = true;
            report << "{\"id\":" << json_string(check.id) << ",\"expected\":" << rgba(check.expected)
                   << ",\"direct\":" << rgba(p) << ",\"rtt\":" << rgba(q) << '}';
        }
        int max_difference{};
        for (std::size_t i = 0; i < direct_pixels.size(); ++i) {
            for (std::size_t c = 0; c < 4; ++c) {
                max_difference = std::max(max_difference, std::abs(int(direct_pixels[i][c]) - int(rtt_pixels[i][c])));
            }
        }
        direct.test(max_difference <= weft::probe_constants::rgba8_parity_tolerance, "full-image direct/RTT parity");
        direct.check_debug_messages();
        rtt.check_debug_messages();
        report
            << "],\"direct_rtt_max_difference\":" << max_difference
            << ",\"assertions\":" << direct.assertions + rtt.assertions
            << ",\"native_textures_retired\":" << direct.native_releases + rtt.native_releases
            << ",\"transparent_linear_direct\":" << linear_rgba(direct.transparent_sample)
            << ",\"transparent_linear_rtt\":" << linear_rgba(rtt.transparent_sample)
            << ",\"debug_warning_error_count\":" << (direct.debug_layer ? "0" : "null")
            << ",\"actual_completion_query\":true,\"ui_serial\":42,\"host_completed_serial\":43,\"dom_pipeline_verified\":false,\"mobile_verified\":false,\"passed\":true}\n";
        save_png(folder / "direct.png", direct_pixels);
        save_png(folder / "rtt.png", rtt_pixels);
        std::ofstream file(folder / "report.json", std::ios::binary);
        require(bool(file << report.str()), "write GPU report");
        std::cout << "PASS D3D11 " << (warp ? "WARP" : "hardware") << ": " << direct.adapter << ", "
                  << direct.assertions + rtt.assertions
                  << " assertions; real query completion; direct/RTT max difference " << max_difference
                  << "; debug layer " << (direct.debug_layer ? "on" : "unavailable") << '\n';
        return 0;
    }

    int run_sdf(bool warp, const std::filesystem::path& folder) {
        std::filesystem::create_directories(folder);
        const auto scene = sdf_scene();
        Experiment direct(warp);
        Experiment rtt(warp);
        const auto p = direct.execute(false, scene.packet);
        const auto q = rtt.execute(true, scene.packet);
        float max_edge_error{};
        std::size_t edge_samples{};
        std::size_t partial_samples{};
        std::size_t latin_pixels{};
        int max_difference{};
        for (std::size_t i = 0; i < p.size(); ++i) {
            direct.test(p[i][3] == weft::constants::color_channel_max && q[i][3] == weft::constants::color_channel_max,
                        "SDF opaque background alpha");
            for (std::size_t c = 0; c < 4; ++c) {
                max_difference = std::max(max_difference, std::abs(int(p[i][c]) - int(q[i][c])));
            }
        }
        // Independent area-coverage oracle for axis-aligned straight square edges.
        // Exclude two-edge corner neighborhoods: a single-channel field rounds corners.
        for (const auto& r : scene.squares) {
            float case_error{};
            std::size_t partial{};
            for (int y = static_cast<int>(r.y) - weft::probe_constants::sdf_edge_scan_margin;
                 y < r.y + r.h + weft::probe_constants::sdf_edge_scan_margin; ++y) {
                for (int x = static_cast<int>(r.x) - weft::probe_constants::sdf_edge_scan_margin;
                     x < r.x + r.w + weft::probe_constants::sdf_edge_scan_margin; ++x) {
                    const bool near_x = std::abs(x + 0.5f - r.x) < weft::probe_constants::sdf_corner_exclusion ||
                                        std::abs(x + 0.5f - r.x - r.w) < weft::probe_constants::sdf_corner_exclusion;
                    const bool near_y = std::abs(y + 0.5f - r.y) < weft::probe_constants::sdf_corner_exclusion ||
                                        std::abs(y + 0.5f - r.y - r.h) < weft::probe_constants::sdf_corner_exclusion;
                    if (near_x && near_y) {
                        continue;
                    }
                    const float expected =
                        overlap(static_cast<float>(x), r.x, r.w) * overlap(static_cast<float>(y), r.y, r.h);
                    const auto& pixel = p[static_cast<std::size_t>(y) * width + x];
                    direct.test(pixel[0] == pixel[1] && pixel[1] == pixel[2], "SDF square neutral fill");
                    const float error =
                        std::abs(decode(pixel[0] / weft::probe_constants::color_channel_max_float) - expected);
                    case_error = std::max(case_error, error);
                    ++edge_samples;
                    if (expected > 0 && expected < 1) {
                        ++partial;
                        if (pixel[0] > 0 && pixel[0] < weft::constants::color_channel_max) {
                            ++partial_samples;
                        }
                    }
                }
            }
            max_edge_error = std::max(max_edge_error, case_error);
            direct.test(partial > 40, "subpixel square edges sampled");
            if (case_error > weft::probe_constants::sdf_edge_tolerance) {
                std::cerr << "scale=" << r.w / weft::constants::text::sdf_default_base_ppem
                          << " edge error=" << case_error << '\n';
            }
            direct.test(case_error <= weft::probe_constants::sdf_edge_tolerance,
                        "SDF straight-edge area coverage (0.12 linear tolerance)");
        }
        direct.test(partial_samples > 200, "derivative AA produced partial coverage at all scales");
        float stroke_error{};
        for (int x = 208; x < 237; ++x) {
            const auto& pixel = p[24 * width + x];
            const float fill = overlap(static_cast<float>(x), scene.stroked.x, 20);
            const float outer = overlap(static_cast<float>(x), scene.stroked.x - 1, 22);
            stroke_error = std::max(
                {stroke_error, std::abs(decode(pixel[0] / weft::probe_constants::color_channel_max_float) - fill),
                 std::abs(decode(pixel[2] / weft::probe_constants::color_channel_max_float) - outer)});
            direct.test(pixel[0] == pixel[1], "blue stroke leaves equal red/green fill");
        }
        direct.test(stroke_error <= weft::probe_constants::sdf_edge_tolerance, "SDF stroke source-distance coverage");
        for (UINT y = 78; y < weft::probe_constants::color_fixture_height; ++y) {
            for (UINT x = 0; x < 210; ++x) {
                if (p[y * width + x][0] > 200) {
                    ++latin_pixels;
                }
            }
        }
        direct.test(latin_pixels > 600, "actual Roboto SDF visible at two sizes");
        for (UINT y = 0; y < height; ++y) {
            for (UINT x = 0; x < width; ++x) {
                bool covered{};
                for (const auto& d : scene.packet.draws) {
                    if (d.asset >= 0 && x + 0.5f >= d.rect.x && y + 0.5f >= d.rect.y &&
                        x + 0.5f < d.rect.x + d.rect.w && y + 0.5f < d.rect.y + d.rect.h) {
                        covered = true;
                    }
                }
                if (!covered) {
                    direct.test(p[y * width + x] == Pixel{0, 0, 0, 255}, "no glyph pixels outside quads");
                }
            }
        }
        direct.test(max_difference <= weft::probe_constants::rgba8_parity_tolerance,
                    "SDF full-image direct/RTT parity");
        direct.check_debug_messages();
        rtt.check_debug_messages();
        save_png(folder / "direct.png", p);
        save_png(folder / "rtt.png", q);
        // Distance diagnostic: encoded byte image, not a coverage/glyph screenshot.
        std::vector<Pixel> distance_image;
        distance_image.reserve(width * height);
        for (const auto value : scene.packet.assets[0]->bytes) {
            distance_image.push_back({value, value, value, 255});
        }
        save_png(folder / "distance-atlas.png", distance_image);
        std::ostringstream report;
        report
            << std::setprecision(weft::probe_constants::report_significant_digits)
            << "{\"fixture\":\"caseelephant-sdf-v1\",\"driver\":" << json_string(warp ? "warp" : "hardware")
            << ",\"adapter\":" << json_string(direct.adapter)
            << ",\"debug_layer\":" << (direct.debug_layer ? "true" : "false")
            << ",\"width\":256,\"height\":128,\"generator\":\"FreeType-2.13.3-bsdf\",\"base_ppem\":40,\"spread_source_texels\":8"
            << ",\"zero_byte\":128,\"encoding\":\"R8_UNORM-linear-distance\",\"atlas_bytes\":32768,\"atlas_mip_levels\":1,\"guard_texels\":1"
            << ",\"generated_glyphs\":" << scene.generated_glyphs
            << ",\"glyph_distance_bytes\":" << scene.distance_bytes
            << ",\"square_scales\":[0.5,1,1.5],\"latin_ppem\":[24,48],\"stroke_source_texels\":2,\"edge_samples\":"
            << edge_samples << ",\"partial_coverage_samples\":" << partial_samples
            << ",\"straight_edge_max_linear_error\":" << max_edge_error
            << ",\"straight_edge_tolerance\":0.12,\"corner_oracle_verified\":false,\"stroke_max_linear_error\":"
            << stroke_error << ",\"latin_visible_pixels\":" << latin_pixels
            << ",\"direct_rtt_max_difference\":" << max_difference
            << ",\"assertions\":" << direct.assertions + rtt.assertions
            << ",\"native_textures_retired\":" << direct.native_releases + rtt.native_releases
            << ",\"actual_completion_query\":true,\"ui_serial\":42,\"host_completed_serial\":43,\"debug_warning_error_count\":"
            << (direct.debug_layer ? "0" : "null")
            << ",\"dom_pipeline_verified\":false,\"mobile_verified\":false,\"msdf_verified\":false,\"passed\":true}\n";
        std::ofstream file(folder / "report.json", std::ios::binary);
        require(bool(file << report.str()), "write SDF report");
        std::cout << "PASS SDF D3D11 " << (warp ? "WARP" : "hardware") << ": " << direct.adapter << "; "
                  << direct.assertions + rtt.assertions << " assertions; straight edge max error " << max_edge_error
                  << "; stroke " << stroke_error << "; direct/RTT " << max_difference << '\n';
        return 0;
    }

    int run_react(bool warp, const std::filesystem::path& folder) {
        std::filesystem::create_directories(folder);
        weft::ScriptConfig config{weft::constants::script::probe_vm_stack_bytes};
#ifdef WEFT_REACT_GC_STRESS
        config.execution_budget_ms = weft::constants::script::max_execution_budget_ms;
#endif
        weft::ScriptDom script(config);
        weft::install_script_platform(script, weft::ScriptPlatformModule::WebHost);
        script.eval(read_file(std::filesystem::path(WEFT_SOURCE_DIR) / "examples/react-vite/native/app.js"),
                    "react-vite-app.js");
        pump_react(script, 0);
        require(script.eval("caseElephantReact.version") == "19.3.0", "fixed real React version");
        require(script.eval("caseElephantReact.errors.length") == "0", "React initial error");
        const auto item = script.node_identity("#item-ember");
        const auto button = script.node_identity("#increment");
        weft::FontSet fonts({WEFT_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf"});
        std::array<weft::DocumentLayout, weft::probe_constants::interaction_frames> pages;
        std::array<std::string, weft::probe_constants::interaction_frames> states;
        auto layout = [&] {
            return weft::layout_document(script.snapshot_subtree("#page"), "", weft::constants::default_view_width,
                                         weft::constants::default_view_height, fonts);
        };
        pages[0] = layout();
        states[0] = script.eval("JSON.stringify(caseElephantReact.snapshot())");
        script.eval("document.getElementById('increment').dispatchEvent(new Event('click',{bubbles:true}));");
        pump_react(script, weft::probe_constants::fixture_frame_ms);
        require(script.eval("document.getElementById('count').textContent") == "Count: 1",
                "React useState click update");
        require(script.node_identity("#increment") == button, "React state update replaced button identity");
        pages[1] = layout();
        states[1] = script.eval("JSON.stringify(caseElephantReact.snapshot())");
        script.eval("document.getElementById('reverse').dispatchEvent(new Event('click',{bubbles:true}));");
        pump_react(script, 2 * weft::probe_constants::fixture_frame_ms);
        require(script.eval("caseElephantReact.snapshot().order.join(',')") == "moss,tide,ember",
                "React keyed reorder");
        require(script.node_identity("#item-ember") == item, "keyed update did not retain native node identity");
        require(script.eval("caseElephantReact.errors.length") == "0" && script.stats().listener_exceptions == 0,
                "React interaction error");
        pages[2] = layout();
        states[2] = script.eval("JSON.stringify(caseElephantReact.snapshot())");
        write_text(folder / "native-dom.json", script.dump_document());
        script.eval("caseElephantReact.unmount();");
        pump_react(script, 3 * weft::probe_constants::fixture_frame_ms);
        script.collect_garbage();
        require(script.eval("document.getElementById('root').childNodes.length") == "0", "React root unmount");
        script.reset_document(); // GPU and page value snapshots cannot depend on the VM.
        require(!script.is_live(item), "page epoch reset left old identity alive");
        std::ostringstream report;
        report << "{\"fixture\":\"caseelephant-react-vite-v1\",\"react\":\"19.3.0\",\"vite\":\"8.3.3\",\"driver\":"
               << json_string(warp ? "warp" : "hardware") << ",\"script_budget_ms\":" << config.execution_budget_ms
               << ",\"frames\":[";
        std::vector<Pixel> initial_pixels;
        std::vector<Pixel> count_pixels;
        std::size_t retired{};
        std::size_t assertions{};
        bool debug = true;
        for (std::size_t i = 0; i < pages.size(); ++i) {
            auto packet = cheetah_packet(pages[i].page, fonts, {});
            const auto textures = packet.list->assets.size();
            const auto draws = packet.list->items.size();
            Experiment direct(warp, weft::constants::default_view_width, weft::constants::default_view_height);
            Experiment rtt(warp, weft::constants::default_view_width, weft::constants::default_view_height);
            auto p = direct.execute(false, packet);
            auto q = rtt.execute(true, std::move(packet));
            int difference{};
            for (std::size_t pixel = 0; pixel < p.size(); ++pixel) {
                for (std::size_t c = 0; c < 4; ++c) {
                    difference = std::max(difference, std::abs(int(p[pixel][c]) - int(q[pixel][c])));
                }
            }
            direct.test(difference <= weft::probe_constants::rgba8_parity_tolerance,
                        "React full-image direct/RTT parity");
            direct.test(p[8 * weft::constants::default_view_width + 8] == Pixel{16, 21, 30, 255},
                        "React inline background native pixels");
            const Pixel first_color = i == 2 ? Pixel{146, 200, 121, 255} : Pixel{244, 162, 97, 255};
            direct.test(p[166 * weft::constants::default_view_width + 330] == first_color,
                        "React keyed bars reach GPU paint order");
            std::size_t text_pixels{};
            for (UINT y = 48; y < 84; ++y) {
                for (UINT x = 24; x < 220; ++x) {
                    if (p[y * weft::constants::default_view_width + x][0] > 120) {
                        ++text_pixels;
                    }
                }
            }
            direct.test(text_pixels > 400, "actual React title glyph pixels");
            if (i == 0) {
                initial_pixels = p;
            }
            if (i == 1) {
                std::size_t changed{};
                bool outside_unchanged = true;
                for (UINT y = 0; y < weft::constants::default_view_height; ++y) {
                    for (UINT x = 0; x < weft::constants::default_view_width; ++x) {
                        if (p[y * weft::constants::default_view_width + x] !=
                            initial_pixels[y * weft::constants::default_view_width + x]) {
                            if (x >= 40 && x < 208 && y >= 158 && y < 198) {
                                ++changed;
                            } else {
                                outside_unchanged = false;
                            }
                        }
                    }
                }
                direct.test(changed > 30 && outside_unchanged, "useState changes only counter text region");
                count_pixels = p;
            }
            if (i == 2) {
                std::size_t changed{};
                for (UINT y = 158; y < 258; ++y) {
                    for (UINT x = 256; x < 600; ++x) {
                        if (p[y * weft::constants::default_view_width + x] !=
                            count_pixels[y * weft::constants::default_view_width + x]) {
                            ++changed;
                        }
                    }
                }
                direct.test(changed > 500, "keyed reorder changes actual list pixels");
            }
            direct.check_debug_messages();
            rtt.check_debug_messages();
            debug = debug && direct.debug_layer && rtt.debug_layer;
            retired += direct.native_releases + rtt.native_releases;
            assertions += direct.assertions + rtt.assertions;
            save_png(folder / ("frame-" + std::to_string(i) + ".png"), p, weft::constants::default_view_width,
                     weft::constants::default_view_height);
            save_png(folder / ("frame-" + std::to_string(i) + "-rtt.png"), q, weft::constants::default_view_width,
                     weft::constants::default_view_height);
            if (i) {
                report << ',';
            }
            report << "{\"state\":" << states[i] << ",\"layout\":" << page_json(pages[i].page.geometry)
                   << ",\"textures\":" << textures << ",\"draws\":" << draws
                   << ",\"direct_rtt_max_difference\":" << difference << '}';
        }
        report
            << "],\"assertions\":" << assertions << ",\"native_textures_retired\":" << retired
            << ",\"debug_layer\":" << (debug ? "true" : "false")
            << ",\"debug_warning_error_count\":" << (debug ? "0" : "null")
            << ",\"actual_completion_query\":true,\"keyed_identity_preserved\":true,\"native_event_dispatch_verified\":true,\"snapshots_survive_vm_reset\":true,\"root_unmount_verified\":true"
            << ",\"html_parser_verified\":false,\"stylesheet_cascade_verified\":false,\"native_window_input_verified\":false,\"mobile_verified\":false,\"passed\":true}";
        write_text(folder / "report.json", report.str());
        std::cout << "PASS React/Vite native DOM -> inline CSS -> Yoga -> HarfBuzz/FreeType -> D3D11 "
                  << (warp ? "WARP" : "hardware") << "; 3 real pixel frames; " << retired << " textures retired; "
                  << assertions << " assertions\n";
        return 0;
    }

} // namespace probes

int main(int argc, char** argv) {
    using namespace probes;
    // --no-vsync anywhere turns vertical sync off for the windows this process opens, --no-validation the API
    // validation of its windows (window_present_settings); the modes see the rest.
    std::vector<char*> arguments(argv, argv + argc);
    if (std::erase_if(arguments, [](const char* argument) {
            return std::string_view(argument) == "--no-vsync";
        })) {
        window_present_settings().vsync = false;
    }
    if (std::erase_if(arguments, [](const char* argument) {
            return std::string_view(argument) == "--no-validation";
        })) {
        window_present_settings().validation = false;
    }
    argc = int(arguments.size());
    argv = arguments.data();
    try {
        const bool owner_async = argc == 3 && (std::string(argv[1]) == "--owner-async-test-hardware" ||
                                               std::string(argv[1]) == "--owner-async-test-warp");
        const bool render_stages = argc == 3 && (std::string(argv[1]) == "--render-stages-test-hardware" ||
                                                 std::string(argv[1]) == "--render-stages-test-warp");
        const bool complex_css_test = argc == 3 && (std::string(argv[1]) == "--complex-css-test-hardware" ||
                                                    std::string(argv[1]) == "--complex-css-test-warp");
        const bool scene_native_test = argc == 3 && (std::string(argv[1]) == "--scene-native-test-hardware" ||
                                                     std::string(argv[1]) == "--scene-native-test-warp");
        const bool scene_gpu_test = argc == 3 && (std::string(argv[1]) == "--scene-gpu-test-hardware" ||
                                                  std::string(argv[1]) == "--scene-gpu-test-warp");
        const bool gallery_profile = argc == 4 && (std::string(argv[1]) == "--gallery-owner-profile" ||
                                                   std::string(argv[1]) == "--gallery-owner-sample");
        const bool gallery_input_benchmark = argc == 4 && std::string(argv[1]) == "--gallery-input-benchmark-hardware";
        const bool gallery_benchmark = argc == 4 && std::string(argv[1]) == "--gallery-benchmark-hardware";
        const bool gallery_live_benchmark = argc == 4 && std::string(argv[1]) == "--gallery-live-benchmark-hardware";
        const bool particle_benchmark = argc == 3 && (std::string(argv[1]) == "--particles-benchmark-hardware" ||
                                                      std::string(argv[1]) == "--particles-benchmark-warp");
        const bool particle_page_benchmark =
            argc == 3 && (std::string(argv[1]) == "--particle-page-benchmark-hardware" ||
                          std::string(argv[1]) == "--particle-page-benchmark-warp");
        const bool glass_page_benchmark = argc == 3 && (std::string(argv[1]) == "--glass-page-benchmark-hardware" ||
                                                        std::string(argv[1]) == "--glass-page-benchmark-warp");
        const bool interaction_benchmark =
            argc == 3 && (std::string(argv[1]) == "--scene-interaction-benchmark-hardware" ||
                          std::string(argv[1]) == "--scene-interaction-benchmark-warp" ||
                          std::string(argv[1]) == "--scene-interaction-benchmark-sampled");
        const bool surface_retirement = argc == 3 && (std::string(argv[1]) == "--scene-surface-retirement-hardware" ||
                                                      std::string(argv[1]) == "--scene-surface-retirement-warp");
        const bool gallery_scroll = argc == 3 && (std::string(argv[1]) == "--gallery-scroll-test-hardware" ||
                                                  std::string(argv[1]) == "--gallery-scroll-test-warp");
        const bool gallery_cluster = argc == 3 && (std::string(argv[1]) == "--gallery-cluster-gpu-hardware" ||
                                                   std::string(argv[1]) == "--gallery-cluster-gpu-warp");
        const bool gallery_trace = argc == 3 && (std::string(argv[1]) == "--gallery-window-profile-hardware" ||
                                                 std::string(argv[1]) == "--gallery-window-profile-warp");
        const bool gallery_preview = argc == 3 && (std::string(argv[1]) == "--gallery-window-hardware" ||
                                                   std::string(argv[1]) == "--gallery-window-warp" ||
                                                   std::string(argv[1]) == "--gallery-window-test-hardware" ||
                                                   std::string(argv[1]) == "--gallery-window-test-warp");
        const bool external_texture = argc == 3 && (std::string(argv[1]) == "--external-texture-warp" ||
                                                    std::string(argv[1]) == "--external-texture-hardware");
        const bool gallery_canvas_window =
            argc == 3 && (std::string(argv[1]) == "--gallery-canvas-window-hardware" ||
                          std::string(argv[1]) == "--gallery-canvas-window-warp" ||
                          std::string(argv[1]) == "--gallery-canvas-window-test-warp" ||
                          std::string(argv[1]) == "--gallery-canvas-window-test-hardware");
        const bool gallery_canvas = argc == 3 && (std::string(argv[1]) == "--gallery-canvas-warp" ||
                                                  std::string(argv[1]) == "--gallery-canvas-hardware");
        const bool pipeline_probe =
            argc == 3 && (std::string(argv[1]) == "--pipeline-warp" || std::string(argv[1]) == "--pipeline-hardware");
        const bool cache_probe =
            argc == 3 && (std::string(argv[1]) == "--cache-warp" || std::string(argv[1]) == "--cache-hardware");
        const bool browser = argc == 4 && std::string(argv[1]) == "--browser-png";
        const bool sdf =
            argc == 3 && (std::string(argv[1]) == "--sdf-warp" || std::string(argv[1]) == "--sdf-hardware");
        const bool react =
            argc == 3 && (std::string(argv[1]) == "--react-warp" || std::string(argv[1]) == "--react-hardware");
        const bool backend =
            argc == 3 && (std::string(argv[1]) == "--backend-warp" || std::string(argv[1]) == "--backend-hardware");
        const bool cheetah =
            argc == 3 && (std::string(argv[1]) == "--cheetah-warp" || std::string(argv[1]) == "--cheetah-hardware" ||
                          std::string(argv[1]) == "--cheetah-window-hardware");
        const bool automatic = argc == 3 && (std::string(argv[1]) == "--cheetah-auto-warp" ||
                                             std::string(argv[1]) == "--cheetah-auto-hardware");
        const bool cheetah_flow = argc == 3 && (std::string(argv[1]) == "--cheetah-flow-warp" ||
                                                std::string(argv[1]) == "--cheetah-flow-hardware");
        const bool flow_window =
            argc == 3 && (std::string(argv[1]) == "--cheetah-flow-window-test-4k-motion-warp" ||
                          std::string(argv[1]) == "--cheetah-flow-window-test-4k-motion-hardware" ||
                          std::string(argv[1]) == "--cheetah-flow-window-test-warp" ||
                          std::string(argv[1]) == "--cheetah-flow-window-test-hardware" ||
                          std::string(argv[1]) == "--cheetah-flow-window-warp" ||
                          std::string(argv[1]) == "--cheetah-flow-window-hardware" ||
                          std::string(argv[1]) == "--cheetah-flow-window-test-4k-warp" ||
                          std::string(argv[1]) == "--cheetah-flow-window-test-4k-hardware" ||
                          std::string(argv[1]) == "--cheetah-flow-window-perf-hardware" ||
                          std::string(argv[1]) == "--cheetah-flow-window-perf-sync-hardware");
        const bool flow =
            argc == 3 && (std::string(argv[1]) == "--flow-warp" || std::string(argv[1]) == "--flow-hardware");
        const bool theme = argc == 3 && (std::string(argv[1]) == "--cheetah-style-warp" ||
                                         std::string(argv[1]) == "--cheetah-style-hardware");
        const bool alert = argc == 3 && (std::string(argv[1]) == "--cheetah-alert-warp" ||
                                         std::string(argv[1]) == "--cheetah-alert-hardware");
        const bool window = argc == 3 && (std::string(argv[1]) == "--react-window-hardware" ||
                                          std::string(argv[1]) == "--react-window-warp" ||
                                          std::string(argv[1]) == "--react-window-test-hardware" ||
                                          std::string(argv[1]) == "--react-window-test-warp");
#ifdef WEFT_PROBE_VULKAN
        const bool vulkan_flow_window = argc == 3 && (std::string(argv[1]) == "--cheetah-flow-window-vulkan" ||
                                                      std::string(argv[1]) == "--cheetah-flow-window-test-4k-vulkan" ||
                                                      std::string(argv[1]) == "--cheetah-flow-window-perf-vulkan");
        // --scene-vulkan-test <output-directory> [--adapter <name>]
        const bool vulkan_scene = (argc == 3 || (argc == 5 && std::string(argv[3]) == "--adapter")) &&
                                  std::string(argv[1]) == "--scene-vulkan-test";
        const bool vulkan_scene_window = argc == 3 && (std::string(argv[1]) == "--scene-vulkan-window" ||
                                                       std::string(argv[1]) == "--scene-vulkan-window-test");
#else
        const bool vulkan_flow_window = false;
        const bool vulkan_scene = false;
        const bool vulkan_scene_window = false;
#endif
        require(gallery_input_benchmark || gallery_live_benchmark || gallery_trace || gallery_profile ||
                    surface_retirement || particle_benchmark || particle_page_benchmark || glass_page_benchmark ||
                    interaction_benchmark || gallery_scroll || gallery_cluster || owner_async || scene_native_test ||
                    render_stages || complex_css_test || scene_gpu_test || gallery_benchmark || gallery_preview ||
                    external_texture || gallery_canvas_window || gallery_canvas || pipeline_probe || cache_probe ||
                    flow_window || vulkan_flow_window || vulkan_scene || vulkan_scene_window || cheetah_flow || flow ||
                    automatic || browser || sdf || react || window || backend || cheetah || alert || theme ||
                    (argc == 3 && (std::string(argv[1]) == "--warp" || std::string(argv[1]) == "--hardware")),
                "Usage: weft_d3d11_probe --gallery-window-[test-]hardware|warp <output-directory> [--no-vsync] (or an "
                "existing probe mode)");
        check_hresult(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");
        try {
#ifdef WEFT_PROBE_VULKAN
            if (vulkan_scene) {
                const auto result = run_scene_vulkan_test(argv[2], argc == 5 ? argv[4] : "");
                CoUninitialize();
                return result;
            }
            if (vulkan_scene_window) {
                const auto result = run_scene_vulkan_window(std::string(argv[1]).ends_with("-test"), argv[2]);
                CoUninitialize();
                return result;
            }
            if (vulkan_flow_window) {
                const auto result =
                    run_cheetah_flow_window_vulkan(std::string(argv[1]).find("-test-") != std::string::npos ||
                                                       std::string(argv[1]).find("-perf-") != std::string::npos,
                                                   argv[2], std::string(argv[1]).find("-perf-") != std::string::npos);
                CoUninitialize();
                return result;
            }
#endif
            const auto result =
                gallery_input_benchmark  ? run_gallery_preview(false, argv[2], false, std::string("input-") + argv[3])
                : gallery_live_benchmark ? run_gallery_preview(false, argv[2], false, std::string("live-") + argv[3])
                : gallery_trace
                    ? run_gallery_preview(std::string(argv[1]).ends_with("warp"), argv[2], false, {}, true, false, true)
                : gallery_profile    ? run_gallery_interaction_profile(argv[2], argv[3],
                                                                       std::string(argv[1]) == "--gallery-owner-sample")
                : surface_retirement ? run_scene_surface_retirement(std::string(argv[1]).ends_with("warp"), argv[2])
                : particle_benchmark ? run_particle_benchmark(std::string(argv[1]).ends_with("warp"), argv[2])
                : particle_page_benchmark ? run_particle_page_benchmark(std::string(argv[1]).ends_with("warp"), argv[2])
                : glass_page_benchmark    ? run_glass_page_benchmark(std::string(argv[1]).ends_with("warp"), argv[2])
                : interaction_benchmark
                    ? run_scene_interaction_benchmark(std::string(argv[1]).ends_with("warp"), argv[2],
                                                      std::string(argv[1]).ends_with("sampled"))
                : gallery_scroll
                    ? run_gallery_preview(std::string(argv[1]).ends_with("warp"), argv[2], true, {}, true, true)
                : gallery_cluster   ? run_gallery_cluster_gpu(std::string(argv[1]).ends_with("warp"), argv[2])
                : owner_async       ? run_owner_async(std::string(argv[1]).ends_with("warp"), argv[2])
                : scene_native_test ? run_scene_gpu_test(std::string(argv[1]).ends_with("warp"), argv[2], true)
                : render_stages     ? run_render_stages(std::string(argv[1]).ends_with("warp"), argv[2])
                : complex_css_test  ? run_complex_css_test(std::string(argv[1]).ends_with("warp"), argv[2])
                : scene_gpu_test    ? run_scene_gpu_test(std::string(argv[1]).ends_with("warp"), argv[2])
                : gallery_benchmark ? run_gallery_preview(false, argv[2], false, argv[3])
                : gallery_preview   ? run_gallery_preview(std::string(argv[1]).ends_with("warp"), argv[2],
                                                          std::string(argv[1]).find("-test-") != std::string::npos)
                : external_texture  ? run_external_texture(std::string(argv[1]).ends_with("warp"), argv[2])
                : gallery_canvas_window
                    ? run_gallery_canvas_window(std::string(argv[1]).ends_with("warp"), argv[2],
                                                std::string(argv[1]).find("-test-") != std::string::npos)
                : gallery_canvas ? run_gallery_canvas(std::string(argv[1]).ends_with("warp"), argv[2])
                : pipeline_probe ? run_two_frame_probe(std::string(argv[1]).ends_with("warp"), argv[2])
                : cache_probe    ? run_render_cache(std::string(argv[1]).ends_with("warp"), argv[2])
                : flow_window    ? run_cheetah_flow_window(std::string(argv[1]).ends_with("warp"),
                                                           std::string(argv[1]).find("-test-") != std::string::npos ||
                                                               std::string(argv[1]).find("-perf-") != std::string::npos,
                                                           argv[2],
                                                           std::string(argv[1]).find("-test-") == std::string::npos ||
                                                               std::string(argv[1]).find("-4k-") != std::string::npos,
                                                           std::string(argv[1]).find("-perf-") != std::string::npos,
                                                           std::string(argv[1]).find("-sync-") != std::string::npos,
                                                           std::string(argv[1]).find("-motion-") != std::string::npos)
                : cheetah_flow
                    ? run_cheetah_alert(std::string(argv[1]) == "--cheetah-flow-warp", argv[2], true, true, true)
                : flow      ? run_flow(std::string(argv[1]) == "--flow-warp", argv[2])
                : automatic ? run_cheetah_alert(std::string(argv[1]) == "--cheetah-auto-warp", argv[2], true, true)
                : theme     ? run_cheetah_alert(std::string(argv[1]) == "--cheetah-style-warp", argv[2], true)
                : alert     ? run_cheetah_alert(std::string(argv[1]) == "--cheetah-alert-warp", argv[2])
                : cheetah   ? run_cheetah(std::string(argv[1]) == "--cheetah-warp",
                                          std::string(argv[1]) == "--cheetah-window-hardware", argv[2])
                : backend   ? run_backend(std::string(argv[1]) == "--backend-warp", argv[2])
                : window    ? run_react_window(std::string(argv[1]).ends_with("warp"),
                                               std::string(argv[1]).find("-test-") != std::string::npos, argv[2])
                : browser   ? capture_browser(argv[2], argv[3])
                : react     ? run_react(std::string(argv[1]) == "--react-warp", argv[2])
                : sdf       ? run_sdf(std::string(argv[1]) == "--sdf-warp", argv[2])
                            : run(std::string(argv[1]) == "--warp", argv[2]);
            CoUninitialize();
            return result;
        } catch (...) {
            CoUninitialize();
            throw;
        }
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
