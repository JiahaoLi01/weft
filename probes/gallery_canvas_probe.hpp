#include "gallery_constants.hpp"

// Original gallery components, native Canvas raster, positioned values and the
// existing renderer ABI. Only the owner below resolves Canvas identities.
struct GalleryCanvasSession {
    caseelephant::ScriptDomProbe vm{{caseelephant::probe_constants::gallery::vm_stack_bytes,
                                     caseelephant::probe_constants::gallery::host_vm_execution_budget_ms}};
    caseelephant::TextProbeFont font{CE_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf"};
    std::string css = read_file(CE_SOURCE_DIR "/examples/crpg-gallery/canvas-profile.css");

    explicit GalleryCanvasSession(unsigned index) {
        vm.install_canvas_host(caseelephant::make_d2d_canvas_factory_probe());
        vm.eval(read_file(CE_SOURCE_DIR "/probes/web_host_bootstrap.js"));
        vm.eval(read_file(CE_SOURCE_DIR "/probes/animation_frame_bootstrap.js"));
        vm.eval(read_file(CE_SOURCE_DIR "/examples/crpg-gallery/native/platform.js"));
        vm.eval(read_file(CE_SOURCE_DIR "/examples/crpg-gallery/native/WEBGL_TESTS.js"));
        vm.install_layout_host([this](const caseelephant::ProbeDomSnapshot& document) {
            const auto page = caseelephant::build_positioned_page_probe(
                flow_find(document, "root"), css, caseelephant::probe_constants::gallery::canvas_backing_width,
                caseelephant::probe_constants::gallery::canvas_backing_height, font);
            return page.elements;
        });
        // Same deterministic host seed in the real Chromium reference. The
        // original component bodies and drawing calls remain unchanged.
        vm.eval(
            "(()=>{let seed=0xce202610;Math.random=()=>((seed=(Math.imul(seed,1664525)+1013904223)>>>0)/4294967296)})()");
        vm.eval("__ceGallery.mount('WEBGL_TESTS'," + std::to_string(index) + ")");
        pump(caseelephant::probe_constants::gallery::initial_clock_ms);
        require(
            vm.eval("document.querySelector('canvas').width===800 && document.querySelector('canvas').height===600") ==
                "true",
            "original Canvas effect did not mount");
    }

    void pump(double time) {
        for (unsigned turn = 0; turn < caseelephant::probe_constants::gallery::task_pump_turn_limit; ++turn) {
            if (vm.eval("__ceHost.pump(" + format_host_time(time) + ")") == "0") {
                return;
            }
        }
        throw std::runtime_error("CE_GALLERY_TASK_LIMIT: ready turns");
    }

    caseelephant::ProbeNodeIdentity identity() {
        return vm.node_identity("canvas");
    }

    void input(unsigned index, double x, double y, double time) {
        caseelephant::ProbeMouseEvent e;
        e.type = index ? caseelephant::ProbeMouseType::Click : caseelephant::ProbeMouseType::Move;
        e.client_x = x;
        e.client_y = y;
        e.time_ms = time;
        require(vm.dispatch_mouse(identity(), e) == caseelephant::ProbeDispatchResult::DefaultAllowed,
                "native Canvas input rejected");
    }

    void size(bool reduced) {
        vm.eval(
            reduced
                ? "document.getElementById('root').style.width='400px';document.getElementById('root').style.height='300px'"
                : "document.getElementById('root').style.width='800px';document.getElementById('root').style.height='600px'");
    }

    FramePacket packet() {
        auto page = caseelephant::build_positioned_page_probe(
            vm.snapshot_subtree("#root"), css, caseelephant::probe_constants::gallery::canvas_backing_width,
            caseelephant::probe_constants::gallery::canvas_backing_height, font);
        FramePacket out;
        for (const auto& p : page.paints) {
            require(p.paint.kind == caseelephant::ProbePaintKind::Rectangle && p.image_source.empty() &&
                        p.rounded_clips.empty(),
                    "Canvas sample left explicit positioned profile");
            Draw d{{p.paint.x, p.paint.y, p.paint.width, p.paint.height}, p.paint.srgb};
            d.uv = p.uv;
            if (p.canvas_identity) {
                const auto s = vm.canvas_snapshot(*p.canvas_identity);
                if (!s.width || !s.height) {
                    continue;
                }
                auto a = std::make_shared<Asset>(Asset{s.width, s.height, Encoding::SrgbStraight, *s.rgba});
                d.asset = int(out.assets.size());
                out.assets.push_back(std::move(a));
            }
            out.draws.push_back(std::move(d));
        }
        require(out.assets.size() == 1, "Canvas paint must produce one immutable image");
        return out;
    }
};

std::vector<Pixel> gallery_output(Experiment& gpu) {
    gpu.immediate->CopyResource(gpu.staging.Get(), gpu.output.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check_hresult(gpu.immediate->Map(gpu.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Canvas completed readback");
    std::vector<Pixel> out(std::size_t(gpu.width) * gpu.height);
    for (UINT y = 0; y < gpu.height; ++y) {
        std::memcpy(out.data() + std::size_t(y) * gpu.width,
                    static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(y) * mapped.RowPitch, gpu.width * 4);
    }
    gpu.immediate->Unmap(gpu.staging.Get(), 0);
    return out;
}

std::vector<Pixel> gallery_reference(const std::filesystem::path& path) {
    const auto image = caseelephant::testing::read_png(path);
    require(image.width == caseelephant::probe_constants::gallery::canvas_backing_width &&
                image.height == caseelephant::probe_constants::gallery::canvas_backing_height,
            "Canvas reference 800x600");
    std::vector<Pixel> out(std::size_t(image.width) * image.height);
    std::memcpy(out.data(), image.rgba.data(), image.rgba.size());
    return out;
}

struct CanvasDifference {
    unsigned maximum{};
    std::array<double, 4> mean{};
    double bright_mean{};
    double bright_iou{};
    std::size_t bright_union{};

    bool within_profile() const {
        return maximum <= caseelephant::probe_constants::gallery::canvas_max_channel_difference &&
               mean[0] <= caseelephant::probe_constants::gallery::canvas_max_mean_difference_r &&
               mean[1] <= caseelephant::probe_constants::gallery::canvas_max_mean_difference_g &&
               mean[2] <= caseelephant::probe_constants::gallery::canvas_max_mean_difference_b &&
               mean[3] <= caseelephant::probe_constants::gallery::canvas_max_mean_difference_a &&
               bright_mean <= caseelephant::probe_constants::gallery::canvas_max_bright_mean_difference &&
               bright_union >= caseelephant::probe_constants::gallery::canvas_min_bright_pixels &&
               bright_iou >= caseelephant::probe_constants::gallery::canvas_min_bright_iou;
    }
};

CanvasDifference gallery_difference(const std::vector<Pixel>& actual, const std::vector<Pixel>& expected) {
    require(actual.size() == expected.size(), "Canvas comparison dimensions");
    CanvasDifference out;
    std::size_t intersection{};
    double bright_error{};
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const auto a = actual[i];
        const auto b = expected[i];
        const bool ah =
            std::max({a[0], a[1], a[2]}) > caseelephant::probe_constants::gallery::canvas_bright_channel_threshold;
        const bool bh =
            std::max({b[0], b[1], b[2]}) > caseelephant::probe_constants::gallery::canvas_bright_channel_threshold;
        intersection += ah && bh;
        out.bright_union += ah || bh;
        for (unsigned c = 0; c < 4; ++c) {
            const auto d = unsigned(std::abs(int(a[c]) - b[c]));
            out.maximum = std::max(out.maximum, d);
            out.mean[c] += d;
            if (ah || bh) {
                bright_error += d;
            }
        }
    }
    for (auto& v : out.mean) {
        v /= actual.size();
    }
    out.bright_mean = out.bright_union ? bright_error / (out.bright_union * 4) : 0;
    out.bright_iou = out.bright_union ? double(intersection) / out.bright_union : 0;
    return out;
}

int run_gallery_canvas(bool warp, const std::filesystem::path& folder) {
    std::filesystem::create_directories(folder);
    std::ostringstream report;
    report
        << "{\"format\":\"gallery-canvas-native-v1\",\"case_count\":2,\"frames_per_case\":64,\"seed\":3458213392,\"backing\":[800,600],\"viewport\":[800,600],\"css_profile\":\"explicit positioned Canvas adapter\",\"cases\":[";
    for (unsigned index = 0; index < caseelephant::probe_constants::gallery::canvas_case_count; ++index) {
        GalleryCanvasSession session(index);
        Experiment gpu(warp, caseelephant::probe_constants::gallery::canvas_backing_width,
                       caseelephant::probe_constants::gallery::canvas_backing_height);
        gpu.cache_assets = true;
        TwoFramePipeline pipeline(gpu);
        FramePacket retained;
        std::vector<Pixel> retained_pixels;
        unsigned error{};
        unsigned samples{};
        unsigned changed{};
        std::uint64_t fills{};
        std::ostringstream comparisons;
        for (unsigned frame = 0; frame < caseelephant::probe_constants::gallery::canvas_frames_per_case; ++frame) {
            const double time = frame * caseelephant::probe_constants::gallery::canvas_frame_step_ms +
                                caseelephant::probe_constants::gallery::initial_clock_ms;
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
                for (int y = 1; y < caseelephant::probe_constants::gallery::canvas_backing_height;
                     y += caseelephant::probe_constants::gallery::canvas_oracle_stride_y) {
                    for (int x = 1; x < caseelephant::probe_constants::gallery::canvas_backing_width;
                         x += caseelephant::probe_constants::gallery::canvas_oracle_stride_x) {
                        const auto expected = cheetah_sample(packet, x, y);
                        const auto actual =
                            pixels[std::size_t(y) * caseelephant::probe_constants::gallery::canvas_backing_width + x];
                        for (unsigned c = 0; c < 4; ++c) {
                            error = std::max(error, unsigned(std::abs(int(expected[c]) - actual[c])));
                        }
                        ++samples;
                    }
                }
                const auto name = "webgl-0" + std::to_string(index) + "-" + std::to_string(frame);
                save_png(folder / (name + ".png"), pixels, caseelephant::probe_constants::gallery::canvas_backing_width,
                         caseelephant::probe_constants::gallery::canvas_backing_height);
                const auto& a = *packet.assets[0];
                std::vector<Pixel> bitmap(std::size_t(a.width) * a.height);
                std::memcpy(bitmap.data(), a.bytes.data(), a.bytes.size());
                const auto reference =
                    gallery_reference(std::filesystem::path(CE_SOURCE_DIR) / "tests/fixtures/canvas-gallery-reference" /
                                      (name + "-bitmap.png"));
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
        require(error <= caseelephant::probe_constants::gallery::max_cpu_oracle_difference && changed > 500 &&
                    fills > 3000,
                "Canvas real GPU animation/pixel oracle");
        require(session.vm.eval("__ceGallery.state().errors.length") == "0" &&
                    session.vm.stats().listener_exceptions == 0,
                "original Canvas execution/input exception");
        const auto target = session.identity();
        session.vm.eval("__ceGallery.unmount()");
        session.pump(caseelephant::probe_constants::gallery::canvas_unmount_time_ms);
        require(session.vm.animation_frame(caseelephant::probe_constants::gallery::canvas_unmount_time_ms).callbacks ==
                    0,
                "unmount did not cancel original animation");
        session.vm.reset_document();
        session.vm.collect_garbage();
        require(session.vm.stats().canvas_contexts == 0 && session.vm.stats().canvas_backing_bytes == 0,
                "Canvas reset leaked backing");
        require(session.vm.dispatch_mouse(target, {}) == caseelephant::ProbeDispatchResult::StaleTarget,
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
    std::cout << "PASS 2 original Canvas gallery cases: 64 frames each, input/resize/pixels/2-frame completion/reset\n";
    return 0;
}

// Visible 4K host. The original components still choose 800x600 backings;
// Tab switches cases, Escape closes. This host is deliberately one Canvas.
int run_gallery_canvas_window(bool warp, const std::filesystem::path& folder, bool self_test) {
    std::filesystem::create_directories(folder);
    NativeReactWindow window(!self_test, true);
    window.state.logical_width = caseelephant::probe_constants::gallery::canvas_backing_width;
    window.state.logical_height = caseelephant::probe_constants::gallery::canvas_backing_height;
    Experiment gpu(warp, caseelephant::constants::uhd_width, caseelephant::constants::uhd_height);
    gpu.cache_assets = true;
    WindowPresenter presenter(gpu, window.state.hwnd);
    TwoFramePipeline pipeline(gpu);
    if (self_test) {
        window.client_size(caseelephant::constants::uhd_width, caseelephant::constants::uhd_height);
        presenter.capture = true;
    }
    caseelephant::TextProbeFontSet fonts({CE_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf",
                                          CE_SOURCE_DIR "/third_party/noto-cjk/source/NotoSansCJKsc-Regular.otf"});
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
    const auto raster =
        caseelephant::ProbeViewport::fit(caseelephant::constants::uhd_width, caseelephant::constants::uhd_height,
                                         caseelephant::probe_constants::gallery::canvas_backing_width,
                                         caseelephant::probe_constants::gallery::canvas_backing_height);
    while (!window.state.closed) {
        MSG key{};
        while (PeekMessageW(&key, window.state.hwnd, WM_KEYDOWN, WM_KEYDOWN, PM_REMOVE)) {
            if (key.wParam == VK_ESCAPE) {
                window.state.close_requested = true;
            }
            if (key.wParam == VK_TAB && !(key.lParam & caseelephant::probe_constants::gallery::key_repeat_flag)) {
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
                std::chrono::milliseconds(caseelephant::probe_constants::gallery::minimized_poll_ms));
            continue;
        }
        if (window.state.resize) {
            pipeline.drain();
            presenter.resize(window.state.width, window.state.height);
            window.state.resize = false;
        }
        const double time = self_test ? frames * caseelephant::probe_constants::gallery::canvas_frame_step_ms +
                                            caseelephant::probe_constants::gallery::host_clock_offset_ms
                                      : seconds() * caseelephant::constants::milliseconds_per_second +
                                            caseelephant::probe_constants::gallery::initial_clock_ms;
        session->pump(time);
        while (!window.state.input.empty()) {
            auto value = window.state.input.front();
            window.state.input.pop_front();
            if (value.wheel) {
                continue;
            }
            const auto point = value.viewport.logical(value.x, value.y);
            const bool inside =
                point[0] >= 0 && point[0] < caseelephant::probe_constants::gallery::canvas_backing_width &&
                point[1] >= 0 && point[1] < caseelephant::probe_constants::gallery::canvas_backing_height;
            if (value.action == caseelephant::ProbeMouseAction::Cancel) {
                pressed = false;
                continue;
            }
            if (value.action == caseelephant::ProbeMouseAction::Down) {
                pressed = inside;
            }
            if (value.action == caseelephant::ProbeMouseAction::Move && inside) {
                session->input(0, point[0], point[1], time);
            }
            if (value.action == caseelephant::ProbeMouseAction::Up) {
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
            d.rect = {raster.x + d.rect.x * raster.scale, raster.y + d.rect.y * raster.scale, d.rect.w * raster.scale,
                      d.rect.h * raster.scale};
        }
        packet.draws.insert(
            packet.draws.begin(),
            Draw{{0, 0, caseelephant::constants::uhd_width, caseelephant::constants::uhd_height}, {0, 0, 0, 1}});
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
        const auto title = std::string("CaseElephant / Original Canvas ") + (index ? "Fireworks" : "Particles") +
                           " / Tab: switch / FPS " + std::to_string(fps.fps);
        SetWindowTextW(window.state.hwnd, std::wstring(title.begin(), title.end()).c_str());
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
            save_png(folder / "window-4k.png", pixels, caseelephant::constants::uhd_width,
                     caseelephant::constants::uhd_height);
            save_png(folder / "window-4k-hud.png", hud, caseelephant::constants::uhd_width,
                     caseelephant::constants::uhd_height);
            unsigned hud_changes{};
            for (unsigned y = 16; y < 128; ++y) {
                for (unsigned x = 3300; x < 3824; ++x) {
                    hud_changes += pixels[std::size_t(y) * caseelephant::constants::uhd_width + x] !=
                                   hud[std::size_t(y) * caseelephant::constants::uhd_width + x];
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
                   std::to_string(pipeline.completions) + ",\"mobile_verified\":false,\"performance_verified\":false}");
    return 0;
}
