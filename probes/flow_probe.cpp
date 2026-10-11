#include "flow_probe.hpp"
#include "react_window_host.hpp"
#include "cheetah_probe.hpp"
#include "probe_modes.hpp"

namespace probes {
    int run_flow(bool warp, const std::filesystem::path& folder) {
        std::filesystem::create_directories(folder);
        NativeReactWindow window(false);
        Experiment gpu(warp, weft::constants::default_view_width, weft::constants::default_view_height);
        WindowPresenter presenter(gpu, window.state.hwnd);
        presenter.resize(weft::constants::default_view_width, weft::constants::default_view_height);
        presenter.capture = true;
        weft::FontSet fonts({WEFT_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf",
                             WEFT_SOURCE_DIR "/third_party/noto-cjk/source/NotoSansCJKsc-Regular.otf"});
        const auto css = read_file(WEFT_SOURCE_DIR "/tests/fixtures/flow-layout/profile.css");
        weft::ScriptDom vm;
        weft::install_script_platform(vm, weft::ScriptPlatformModule::WebHost);
        vm.eval(read_file(WEFT_SOURCE_DIR "/tests/fixtures/flow-layout/fixture.js"));
        vm.install_layout_host([&](const auto& dom) {
            return weft::layout_document(flow_find(dom, "gallery"), css, weft::constants::default_view_width,
                                         weft::constants::default_view_height, fonts)
                .elements;
        });
        std::array<weft::DocumentLayout, weft::probe_constants::interaction_frames> pages;
        for (int i = 0; i < weft::probe_constants::interaction_frames; ++i) {
            if (i) {
                require(vm.dispatch_mouse(vm.node_identity("#advance"), {weft::MouseType::Click, 30, 320}) ==
                            weft::DispatchResult::DefaultAllowed,
                        "flow native button dispatch");
            }
            pages[i] = weft::layout_document(vm.snapshot_subtree("#gallery"), css, weft::constants::default_view_width,
                                             weft::constants::default_view_height, fonts);
        }
        const auto old = vm.node_identity("#viewport");
        vm.reset_document();
        vm.collect_garbage();
        require(!vm.is_live(old), "flow values survive epoch reset");
        auto images = load_cheetah_images();
        unsigned retired{};
        unsigned masks{};
        unsigned fallback{};
        std::size_t samples{};
        int error{};
        float edge_error{};
        int mask_error{};
        std::ostringstream report;
        report << "{\"fixture\":\"flow-layout-gpu-v1\",\"driver\":" << json_string(warp ? "warp" : "hardware")
               << ",\"adapter\":" << json_string(gpu.adapter) << ",\"frames\":[";
        std::vector<Pixel> previous;
        auto render = [&](const weft::PositionedPage& page) {
            auto packet = cheetah_packet(page, fonts, images);
            retired += unsigned(packet.list->assets.size());
            render_window(gpu, packet, [&] {
                gpu.immediate->CopyResource(gpu.staging.Get(), gpu.output.texture.Get());
                presenter.present();
            });
            // Inspect the RGBA UI target before the host's opaque-black presentation.
            std::vector<Pixel> pixels(weft::constants::default_view_width * weft::constants::default_view_height);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check_hresult(gpu.immediate->Map(gpu.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                          "flow completed RGBA target");
            for (unsigned y = 0; y < weft::constants::default_view_height; ++y) {
                std::memcpy(pixels.data() + y * weft::constants::default_view_width,
                            static_cast<std::uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                            weft::constants::default_view_width * sizeof(Pixel));
            }
            gpu.immediate->Unmap(gpu.staging.Get(), 0);
            for (int y = 0; y < weft::constants::default_view_height; y += 3) {
                for (int x = 0; x < weft::constants::default_view_width; x += 3) {
                    compare_oracle(cheetah_sample(packet, x, y),
                                   pixels[std::size_t(y) * weft::constants::default_view_width + x], error, edge_error);
                    ++samples;
                }
            }
            return std::pair{std::move(packet), std::move(pixels)};
        };
        for (int i = 0; i < weft::probe_constants::interaction_frames; ++i) {
            for (const auto& p : pages[i].page.paints) {
                masks += !p.rounded_clips.empty();
                for (const auto& g : p.paint.shaped.glyphs) {
                    fallback += g.font_id == weft::probe_constants::cjk_fallback_font_id;
                }
            }
            auto [packet, pixels] = render(pages[i].page);
            require(pixels.front() == Pixel{0, 0, 0, 0}, "rounded root corner transparent");
            require(pixels[10 * weft::constants::default_view_width + 320] == Pixel{20, 34, 45, 255},
                    "rounded root center background");
            if (i) {
                require(pixels != previous, "flow mutation changes GPU pixels");
            }
            previous = pixels;
            save_png(folder / ("frame-" + std::to_string(i) + ".png"), pixels, weft::constants::default_view_width,
                     weft::constants::default_view_height);
            if (i) {
                report << ',';
            }
            report << "{\"layout\":" << page_json(pages[i].page.geometry)
                   << ",\"textures\":" << packet.list->assets.size() << '}';
        }
        // The GPU's analytic elliptical clip coverage against a separate dense area integration.
        weft::PositionedPage shape;
        weft::PositionedPaint solid;
        solid.paint = {weft::PaintKind::Rectangle, "ellipse", {}, 10, 10, 100, 80, 0, {1, 1, 1, 1}, {}};
        solid.clip = {0, 0, weft::constants::default_view_width, weft::constants::default_view_height};
        solid.rounded_clips = {weft::normalize_rounded({{10, 10, 100, 80}, {70, 70, 70, 70}, {56, 56, 56, 56}})};
        shape.paints.push_back(solid);
        const auto ellipse = render(shape).second;
        for (int y = 0; y < 80; ++y) {
            for (int x = 0; x < 100; ++x) {
                unsigned count{};
                constexpr int dense = 64;
                for (int yy = 0; yy < dense; ++yy) {
                    for (int xx = 0; xx < dense; ++xx) {
                        const double dx = (x + (xx + .5) / dense - 50) / 50;
                        const double dy = (y + (yy + .5) / dense - 40) / 40;
                        if (dx * dx + dy * dy <= 1) {
                            ++count;
                        }
                    }
                }
                const auto reference = int(std::lround(count * 255. / (dense * dense)));
                // White over transparent: alpha is the coverage.
                const auto alpha = int(ellipse[std::size_t(y + 10) * weft::constants::default_view_width + x + 10][3]);
                mask_error = std::max(mask_error, std::abs(reference - alpha));
            }
        }
        if (mask_error > weft::probe_constants::ellipse_area_tolerance) {
            std::cerr << "ellipse difference " << mask_error << '\n';
        }
        require(mask_error <= weft::probe_constants::ellipse_area_tolerance,
                "dense independent elliptical area oracle");
        shape.paints[0].image_source = "art/ink-panel-v2.png";
        auto translucent = solid;
        translucent.paint.srgb = {.2f, .8f, .4f, .35f};
        translucent.paint.x = 50;
        translucent.paint.y = 30;
        shape.paints.push_back(translucent);
        auto [packet, pixels] = render(shape);
        save_png(folder / "masked-original-image.png", pixels, weft::constants::default_view_width,
                 weft::constants::default_view_height);
        require(pixels[10 * weft::constants::default_view_width + 10] == Pixel{0, 0, 0, 0},
                "masked PNG outside corner");
        if (error > weft::probe_constants::cpu_oracle_tolerance ||
            edge_error > weft::probe_constants::rounded_clip_edge_linear_tolerance) {
            std::cerr << "oracle difference " << error << ", at rounded clip edges " << edge_error << '\n';
        }
        require(error <= weft::probe_constants::cpu_oracle_tolerance &&
                    edge_error <= weft::probe_constants::rounded_clip_edge_linear_tolerance,
                "flow bilinear linear-premultiplied GPU oracle");
        gpu.check_debug_messages();
        require(retired == gpu.native_releases, "all textures retire on actual completion");
        report
            << "],\"cpu_pixel_samples\":" << samples << ",\"cpu_pixel_max_difference\":" << error
            << ",\"cpu_pixel_rounded_edge_max_linear_difference\":" << edge_error
            << ",\"independent_ellipse_area_max_difference\":" << mask_error << ",\"rounded_paints\":" << masks
            << ",\"fallback_glyphs\":" << fallback << ",\"native_textures_retired\":" << retired
            << ",\"actual_completion_query\":true,\"post_vm_reset_render_verified\":true,\"render_backend_abi_version\":1,\"debug_warning_error_count\":"
            << (gpu.debug_layer ? "0" : "null")
            << ",\"mobile_verified\":false,\"browser_pixel_parity_verified\":false,\"passed\":true}";
        write_text(folder / "report.json", report.str());
        std::cout << "PASS flow D3D11: CPU max " << error << ", ellipse area max " << mask_error << ", " << retired
                  << " retired textures\n";
        return 0;
    }
} // namespace probes
