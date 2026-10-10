#include "gallery_canvas_probe.hpp"
#include "react_window_host.hpp"
#include "cheetah_probe.hpp"
#include "cheetah_flow_window_probe.hpp"
#include "probe_modes.hpp"

namespace probes {
    int run_gallery_canvas(bool warp, const std::filesystem::path& folder) {
        std::filesystem::create_directories(folder);
        std::ostringstream report;
        report
            << "{\"format\":\"gallery-canvas-native-v1\",\"case_count\":2,\"frames_per_case\":64,\"seed\":3458213392,\"backing\":[800,600],\"viewport\":[800,600],\"css_profile\":\"explicit positioned Canvas adapter\",\"cases\":[";
        for (unsigned index = 0; index < weft::probe_constants::gallery::canvas_case_count; ++index) {
            GalleryCanvasSession session(index);
            Experiment gpu(warp, weft::probe_constants::gallery::canvas_backing_width,
                           weft::probe_constants::gallery::canvas_backing_height);
            gpu.cache_assets = true;
            TwoFramePipeline pipeline(gpu);
            FramePacket retained;
            std::vector<Pixel> retained_pixels;
            unsigned error{};
            unsigned samples{};
            unsigned changed{};
            std::uint64_t fills{};
            std::ostringstream comparisons;
            for (unsigned frame = 0; frame < weft::probe_constants::gallery::canvas_frames_per_case; ++frame) {
                const double time = frame * weft::probe_constants::gallery::canvas_frame_step_ms +
                                    weft::probe_constants::gallery::initial_clock_ms;
                if (frame == 32) {
                    session.size(true);
                }
                if (frame == 48) {
                    session.size(false);
                }
                if (frame == 2) {
                    session.input(index, 260, 180, time);
                }
                if (frame == 34) {
                    session.input(index, 130, 90, time);
                }
                session.pump(time);
                const auto raf = session.vm.animation_frame(time);
                session.pump(time);
                require(raf.callbacks == 1 && raf.pending == 1 && raf.exceptions == 0, "original Canvas rAF failed");
                auto packet = session.packet();
                fills = session.vm.canvas_snapshot(session.identity()).fills;
                pipeline.submit(packet, [] {
                });
                if (frame == 11 || frame == 39 || frame == 63) {
                    pipeline.drain();
                    const auto pixels = gallery_output(gpu);
                    for (int y = 1; y < weft::probe_constants::gallery::canvas_backing_height;
                         y += weft::probe_constants::gallery::canvas_oracle_stride_y) {
                        for (int x = 1; x < weft::probe_constants::gallery::canvas_backing_width;
                             x += weft::probe_constants::gallery::canvas_oracle_stride_x) {
                            const auto expected = cheetah_sample(packet, x, y);
                            const auto actual =
                                pixels[std::size_t(y) * weft::probe_constants::gallery::canvas_backing_width + x];
                            for (unsigned c = 0; c < 4; ++c) {
                                error = std::max(error, unsigned(std::abs(int(expected[c]) - actual[c])));
                            }
                            ++samples;
                        }
                    }
                    const auto name = "webgl-0" + std::to_string(index) + "-" + std::to_string(frame);
                    save_png(folder / (name + ".png"), pixels, weft::probe_constants::gallery::canvas_backing_width,
                             weft::probe_constants::gallery::canvas_backing_height);
                    const auto& a = *packet.assets[0];
                    std::vector<Pixel> bitmap(std::size_t(a.width) * a.height);
                    std::memcpy(bitmap.data(), a.bytes.data(), a.bytes.size());
                    const auto reference =
                        gallery_reference(std::filesystem::path(WEFT_SOURCE_DIR) /
                                          "tests/fixtures/canvas-gallery-reference" / (name + "-bitmap.png"));
                    const auto difference = gallery_difference(bitmap, reference);
                    require(difference.within_profile(),
                            "original Canvas bitmap outside declared Chromium difference limits");
                    // Reject visually empty output even when most pixels are dark.
                    auto blank = bitmap;
                    for (auto& p : blank) {
                        p = {10, 8, 6, 255};
                    }
                    require(!gallery_difference(blank, reference).within_profile(),
                            "Canvas comparison accepts missing animation/input");
                    if (frame != 11) {
                        comparisons << ',';
                    }
                    comparisons << "{\"frame\":" << frame << ",\"max\":" << difference.maximum << ",\"channel_mean\":["
                                << difference.mean[0] << ',' << difference.mean[1] << ',' << difference.mean[2] << ','
                                << difference.mean[3] << "],\"bright_mean\":" << difference.bright_mean
                                << ",\"bright_iou\":" << difference.bright_iou << '}';
                    save_png(folder / (name + "-bitmap.png"), bitmap, a.width, a.height);
                    std::ofstream raw(folder / (name + ".rgba"), std::ios::binary);
                    raw.write(reinterpret_cast<const char*>(a.bytes.data()), std::streamsize(a.bytes.size()));
                    require(bool(raw), "Canvas raw output");
                    if (frame == 11) {
                        retained = packet;
                        retained_pixels = pixels;
                    }
                    if (frame == 63) {
                        for (std::size_t i = 0; i < pixels.size(); ++i) {
                            changed += pixels[i] != retained_pixels[i];
                        }
                    }
                }
            }
            pipeline.drain();
            require(error <= weft::probe_constants::gallery::max_cpu_oracle_difference && changed > 500 && fills > 3000,
                    "Canvas real GPU animation/pixel oracle");
            require(session.vm.eval("__ceGallery.state().errors.length") == "0" &&
                        session.vm.stats().listener_exceptions == 0,
                    "original Canvas execution/input exception");
            const auto target = session.identity();
            session.vm.eval("__ceGallery.unmount()");
            session.pump(weft::probe_constants::gallery::canvas_unmount_time_ms);
            require(session.vm.animation_frame(weft::probe_constants::gallery::canvas_unmount_time_ms).callbacks == 0,
                    "unmount did not cancel original animation");
            session.vm.reset_document();
            session.vm.collect_garbage();
            require(session.vm.stats().canvas_contexts == 0 && session.vm.stats().canvas_backing_bytes == 0,
                    "Canvas reset leaked backing");
            require(session.vm.dispatch_mouse(target, {}) == weft::DispatchResult::StaleTarget,
                    "reset input epoch accepted");
            pipeline.submit(retained, [] {
            });
            pipeline.drain();
            require(gallery_output(gpu) == retained_pixels, "Canvas old FramePacket changed after VM reset");
            gpu.evict_assets(0, true);
            gpu.check_debug_messages();
            require(gpu.native_creations == gpu.native_releases && pipeline.submissions == pipeline.completions,
                    "Canvas resources released before/missing actual completion");
            if (index) {
                report << ',';
            }
            report
                << "{\"id\":\"webgl-0" << index << "\",\"adapter\":" << json_string(gpu.adapter)
                << ",\"raster\":\"Direct2D SOFTWARE WIC\",\"raf_callbacks\":64,\"native_input_events\":2,\"fill_commands\":"
                << fills << ",\"changed_pixels\":" << changed << ",\"cpu_pixel_samples\":" << samples
                << ",\"cpu_pixel_max_difference\":" << error << ",\"browser_bitmap_differences\":[" << comparisons.str()
                << "],\"exact_browser_pixel_parity\":false,\"peak_in_flight\":" << pipeline.peak_in_flight
                << ",\"actual_completed_submissions\":" << pipeline.completions
                << ",\"retired_textures\":" << gpu.native_releases
                << ",\"debug_warning_error_count\":" << (gpu.debug_layer ? "0" : "null") << ",\"passed\":true}";
        }
        report
            << "],\"original_component_bodies_unchanged\":true,\"snapshots_survive_vm_reset\":true,\"mobile_verified\":false,\"webgl_verified\":false,\"performance_verified\":false}";
        write_text(folder / "report.json", report.str());
        std::cout
            << "PASS 2 original Canvas gallery cases: 64 frames each, input/resize/pixels/2-frame completion/reset\n";
        return 0;
    }

    // Visible 4K host. The original components still choose 800x600 backings;
    // Tab switches cases, Escape closes. This host is deliberately one Canvas.
    int run_gallery_canvas_window(bool warp, const std::filesystem::path& folder, bool self_test) {
        std::filesystem::create_directories(folder);
        NativeReactWindow window(!self_test, true);
        window.state.logical_width = weft::probe_constants::gallery::canvas_backing_width;
        window.state.logical_height = weft::probe_constants::gallery::canvas_backing_height;
        Experiment gpu(warp, weft::constants::uhd_width, weft::constants::uhd_height);
        gpu.cache_assets = true;
        WindowPresenter presenter(gpu, window.state.hwnd);
        TwoFramePipeline pipeline(gpu);
        if (self_test) {
            window.client_size(weft::constants::uhd_width, weft::constants::uhd_height);
            presenter.capture = true;
        }
        weft::FontSet fonts({WEFT_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf",
                             WEFT_SOURCE_DIR "/third_party/noto-cjk/source/NotoSansCJKsc-Regular.otf"});
        unsigned index{};
        unsigned frames{};
        bool pressed{};
        auto session = std::make_unique<GalleryCanvasSession>(index);
        const auto started = std::chrono::steady_clock::now();
        auto seconds = [&] {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        };
        WindowFrameRate fps;
        fps.reset(seconds());
        presenter.overlay(frame_rate_overlay(fonts, "FPS: warming up"));
        std::string window_title;
        const auto raster = weft::Viewport::fit(weft::constants::uhd_width, weft::constants::uhd_height,
                                                weft::probe_constants::gallery::canvas_backing_width,
                                                weft::probe_constants::gallery::canvas_backing_height);
        while (!window.state.closed) {
            MSG key{};
            while (PeekMessageW(&key, window.state.hwnd, WM_KEYDOWN, WM_KEYDOWN, PM_REMOVE)) {
                if (key.wParam == VK_ESCAPE) {
                    window.state.close_requested = true;
                }
                if (key.wParam == VK_F7 && !(key.lParam & weft::probe_constants::gallery::key_repeat_flag)) {
                    presenter.vsync = !presenter.vsync;
                    fps.reset(seconds());
                }
                if (key.wParam == VK_TAB && !(key.lParam & weft::probe_constants::gallery::key_repeat_flag)) {
                    pipeline.drain();
                    session->vm.eval("__ceGallery.unmount()");
                    index = 1 - index;
                    session = std::make_unique<GalleryCanvasSession>(index);
                    pressed = false;
                }
            }
            window.messages();
            if (window.state.closed) {
                break;
            }
            if (window.state.minimized) {
                pipeline.poll();
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(weft::probe_constants::gallery::minimized_poll_ms));
                continue;
            }
            if (window.state.resize) {
                pipeline.drain();
                presenter.resize(window.state.width, window.state.height);
                window.state.resize = false;
            }
            const double time = self_test ? frames * weft::probe_constants::gallery::canvas_frame_step_ms +
                                                weft::probe_constants::gallery::host_clock_offset_ms
                                          : seconds() * weft::constants::milliseconds_per_second +
                                                weft::probe_constants::gallery::initial_clock_ms;
            session->pump(time);
            while (!window.state.input.empty()) {
                auto value = window.state.input.front();
                window.state.input.pop_front();
                if (value.wheel) {
                    continue;
                }
                const auto point = value.viewport.logical(value.x, value.y);
                const bool inside = point[0] >= 0 && point[0] < weft::probe_constants::gallery::canvas_backing_width &&
                                    point[1] >= 0 && point[1] < weft::probe_constants::gallery::canvas_backing_height;
                if (value.action == weft::MouseAction::Cancel) {
                    pressed = false;
                    continue;
                }
                if (value.action == weft::MouseAction::Down) {
                    pressed = inside;
                }
                if (value.action == weft::MouseAction::Move && inside) {
                    session->input(0, point[0], point[1], time);
                }
                if (value.action == weft::MouseAction::Up) {
                    if (pressed && inside) {
                        session->input(1, point[0], point[1], time);
                    }
                    pressed = false;
                }
            }
            const auto raf = session->vm.animation_frame(time);
            session->pump(time);
            require(raf.callbacks == 1 && !raf.exceptions && session->vm.stats().listener_exceptions == 0,
                    "Canvas window animation/input failed");
            auto packet = session->packet();
            for (auto& d : packet.draws) {
                d.rect = {raster.x + d.rect.x * raster.scale, raster.y + d.rect.y * raster.scale,
                          d.rect.w * raster.scale, d.rect.h * raster.scale};
            }
            packet.draws.insert(packet.draws.begin(),
                                Draw{{0, 0, weft::constants::uhd_width, weft::constants::uhd_height}, {0, 0, 0, 1}});
            pipeline.submit(
                std::move(packet),
                [&] {
                    presenter.present();
                },
                presenter.overlay_lease());
            ++frames;
            if (presenter.last_visible && fps.presented(seconds())) {
                std::ostringstream rate;
                rate << "FPS: " << std::fixed << std::setprecision(1) << fps.fps;
                presenter.overlay(frame_rate_overlay(fonts, rate.str()));
            }
            auto title = std::string("CaseElephant / Original Canvas ") + (index ? "Fireworks" : "Particles") +
                         " / Tab: switch / FPS " + std::to_string(fps.fps) + " / VSync " +
                         (presenter.vsync ? "on" : "off") + " (F7)";
            // WM_SETTEXT redraws the caption: only when the text changes (about once a second).
            if (title != window_title) {
                SetWindowTextW(window.state.hwnd, std::wstring(title.begin(), title.end()).c_str());
                window_title = std::move(title);
            }
            pipeline.poll();
            if (self_test && frames == 4) {
                pipeline.drain();
                session->vm.eval("__ceGallery.unmount()");
                index = 1;
                session = std::make_unique<GalleryCanvasSession>(index);
                const auto point = MAKELPARAM(1416, 648);
                require(PostMessageW(window.state.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point) &&
                            PostMessageW(window.state.hwnd, WM_LBUTTONUP, 0, point),
                        "Canvas queued HWND click");
            }
            if (self_test && frames == 8) {
                pipeline.drain();
                const auto pixels = gallery_output(gpu);
                const auto hud = presenter.pixels();
                save_png(folder / "window-4k.png", pixels, weft::constants::uhd_width, weft::constants::uhd_height);
                save_png(folder / "window-4k-hud.png", hud, weft::constants::uhd_width, weft::constants::uhd_height);
                unsigned hud_changes{};
                for (unsigned y = 16; y < 128; ++y) {
                    for (unsigned x = 3300; x < 3824; ++x) {
                        hud_changes += pixels[std::size_t(y) * weft::constants::uhd_width + x] !=
                                       hud[std::size_t(y) * weft::constants::uhd_width + x];
                    }
                }
                require(hud_changes > 1000 && session->vm.canvas_snapshot(session->identity()).fills > 100 &&
                            session->vm.stats().listener_calls >= 3,
                        "Canvas HWND click or actual FPS overlay absent");
                window.state.close_requested = true;
            }
        }
        session->vm.eval("__ceGallery.unmount()");
        session->vm.reset_document();
        pipeline.drain();
        gpu.evict_assets(0, true);
        gpu.check_debug_messages();
        require(gpu.native_creations == gpu.native_releases, "Canvas window completion leak");
        write_text(folder / "window-report.json",
                   "{\"frames\":" + std::to_string(frames) +
                       ",\"surface\":[3840,2160],\"canvas_backing\":[800,600],\"actual_completed_submissions\":" +
                       std::to_string(pipeline.completions) +
                       ",\"mobile_verified\":false,\"performance_verified\":false}");
        return 0;
    }
} // namespace probes
