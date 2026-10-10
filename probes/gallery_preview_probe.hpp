#include "gallery_live_profiler.hpp"
#include "gallery_constants.hpp"

// Inspection diagnostics use a retained host overlay. They do not invalidate
// the page whose input/rendering performance is being inspected.
inline void gallery_append_detail(FramePacket& packet, weft::FontSet& fonts, const std::string& value) {
    weft::PositionedPage page;
    weft::PositionedPaint line;
    line.paint.kind = weft::PaintKind::Text;
    line.paint.x = 584;
    line.paint.y = 2088;
    line.paint.font_size = 28;
    line.paint.srgb = {.788f, .545f, .455f, 1};
    line.paint.shaped = fonts.shape(value.substr(0, value.find('\n')), 28).shaped;
    line.clip = {584, 2056, 3200, 88};
    page.paints.push_back(std::move(line));
    auto footer = cheetah_packet(page, fonts, {});
    const auto offset = int(packet.assets.size());
    packet.assets.insert(packet.assets.end(), footer.assets.begin(), footer.assets.end());
    for (auto draw : footer.draws) {
        if (draw.asset >= 0) {
            draw.asset += offset;
        }
        packet.draws.push_back(std::move(draw));
    }
}

// Owner-thread inspection host. This is an authored DOM/CSS shell around the
// fixed inventory, NOT a claim that original Tailwind/SVG/WebGL paint exists.
struct GalleryPreviewCase {
    std::string id;
    std::string group;
    std::string label;
    std::string execution;
    std::string failure;
    std::string reason;
    unsigned index{};
};

std::string gallery_preview_gap(const std::string& group) {
    if (group == "CSS_TESTS" || group == "D3_TESTS") {
        return "Native original CSS/SVG scene; compiled fixed Tailwind adapter; 121-frame raster/input references";
    }
    if (gallery_cluster_group(group)) {
        return "Native original scene; shared SVG/input/layout/animation profile; browser captures and input trace";
    }
    if (group == "WEBGL_TESTS") {
        return "WebGL context / Three.js GPU path not implemented";
    }
    if (group == "KLINE_TESTS") {
        return "Chart Canvas APIs and original CSS not integrated";
    }
    if (group == "LOTTIE_TESTS") {
        return "Lottie SVG paint / remote asset fetch not integrated";
    }
    return "Original animation styles / geometry / input not integrated";
}

std::string gallery_preview_json(const std::vector<GalleryPreviewCase>& cases) {
    std::ostringstream out;
    out << '[';
    bool comma{};
    for (const auto& c : cases) {
        if (comma) {
            out << ',';
        }
        comma = true;
        out << "{\"id\":" << json_string(c.id) << ",\"group\":" << json_string(c.group)
            << ",\"label\":" << json_string(c.label) << ",\"index\":" << c.index
            << ",\"execution\":" << json_string(c.execution) << ",\"failure\":" << json_string(c.failure)
            << ",\"reason\":" << json_string(c.reason) << ",\"pixels\":"
            << json_string(c.group == "WEBGL_TESTS" && c.index < weft::probe_constants::gallery::canvas_case_count
                               ? "native_canvas_profile"
                           : c.group == "CSS_TESTS" || c.group == "D3_TESTS" || gallery_cluster_group(c.group)
                               ? "native_scene_profile"
                               : "not_passed")
            << '}';
    }
    return out.str() + ']';
}

std::vector<GalleryPreviewCase> gallery_preview_observe() {
    weft::ScriptDom registry(
        {weft::probe_constants::gallery::vm_stack_bytes, weft::probe_constants::gallery::host_vm_execution_budget_ms});
    const auto directory = std::filesystem::path(WEFT_SOURCE_DIR) / "examples/crpg-gallery/native";
    registry.eval(read_file(directory / "case-list.js"));
    require(registry.eval("__ceGalleryCases.length") == "72", "gallery inventory changed; review preview scope");
    std::map<std::string, std::string> scripts;
    std::vector<GalleryPreviewCase> cases;
    for (unsigned i = 0; i < weft::probe_constants::gallery::catalog_case_count; ++i) {
        const auto lookup = "__ceGalleryCases[" + std::to_string(i) + "]";
        GalleryPreviewCase c;
        c.id = registry.eval(lookup + ".id");
        c.group = registry.eval(lookup + ".group");
        c.label = registry.eval(lookup + ".label");
        c.index = unsigned(std::stoul(registry.eval(lookup + ".index")));
        c.reason = gallery_preview_gap(c.group);
        if (c.id == "anime-11") {
            c.reason +=
                "; ORIGINAL ISSUE: throw is stationary in Chrome and native; valid-transform inertia is verified separately";
        }
#ifdef WEFT_REACT_GC_STRESS
        // The 72 large original module loads have a separate normal-build audit,
        // as in gallery_foundation. Allocation GC instrumentation tests the
        // full 72-node shell and two live original Canvas VMs below instead.
        c.execution = "not_run_gc_instrumentation";
        c.failure =
            "Isolated module observation runs in normal Debug/Release; GC window tests shell, 34 CSS/D3 and 2 Canvas lifecycles";
#else
        if (c.group == "WEBGL_TESTS" && c.index < weft::probe_constants::gallery::canvas_case_count) {
            GalleryCanvasSession session(c.index);
            for (unsigned frame = 0; frame < weft::probe_constants::gallery::observation_frames; ++frame) {
                const double time = frame * weft::probe_constants::gallery::observation_step_ms +
                                    weft::probe_constants::gallery::host_clock_offset_ms;
                session.pump(time);
                const auto r = session.vm.animation_frame(time);
                session.pump(time);
                require(r.callbacks == 1 && !r.exceptions, "preview original Canvas observation");
            }
            c.execution = "no_error_observed";
            c.reason = "Native original Canvas2D; 800x600 backing; bounded Canvas profile";
            session.vm.eval("__ceGallery.unmount()");
            session.vm.reset_document();
            session.vm.collect_garbage();
        } else if (gallery_cluster_group(c.group)) {
            const auto it =
                std::find_if(gallery_scene_descriptors.begin(), gallery_scene_descriptors.end(), [&](const auto& d) {
                    return c.group == d.group && c.index == d.index;
                });
            GallerySceneSession session(c.group, c.index, it->width, it->height, "cluster-utilities.css");
            for (unsigned f = 0; f < weft::probe_constants::gallery::observation_frames; ++f) {
                session.step(f * weft::probe_constants::gallery::observation_step_ms +
                             weft::probe_constants::gallery::host_clock_offset_ms);
            }
            c.execution = "no_error_observed";
            session.unmount();
            require(session.vm.stats().live_nodes == weft::probe_constants::gallery::unmounted_live_nodes,
                    "cluster observation leak");
        } else {
            weft::ScriptDom vm({weft::probe_constants::gallery::vm_stack_bytes,
                                weft::probe_constants::gallery::host_vm_execution_budget_ms});
            const auto baseline = vm.stats();
            bool clean{};
            try {
                vm.install_canvas_host(weft::make_d2d_canvas_factory());
                weft::install_script_platform(vm, weft::ScriptPlatformModule::WebHost);
                weft::install_script_platform(vm, weft::ScriptPlatformModule::AnimationFrame);
                vm.eval(read_file(directory / "platform.js"));
                if (!scripts.contains(c.group)) {
                    scripts[c.group] = read_file(directory / (c.group + ".js"));
                }
                vm.eval(scripts.at(c.group), c.group + ".js");
                vm.eval("__ceGallery.mount(" + json_string(c.group) + "," + std::to_string(c.index) + ")");
                for (unsigned frame = 0; frame < weft::probe_constants::gallery::observation_frames; ++frame) {
                    const double time = frame * weft::probe_constants::gallery::observation_step_ms +
                                        weft::probe_constants::gallery::host_clock_offset_ms;
                    const auto stamp = format_host_time(time);
                    auto pump = [&] {
                        for (unsigned turn = 0; turn < weft::probe_constants::gallery::task_pump_turn_limit; ++turn) {
                            if (vm.eval("__ceHost.pump(" + stamp + ")") == "0") {
                                return;
                            }
                        }
                        throw std::runtime_error("WEFT_GALLERY_TASK_LIMIT: preview observation");
                    };
                    pump();
                    const auto r = vm.animation_frame(time);
                    if (r.exceptions) {
                        c.failure += r.exception_messages_json;
                    }
                    pump();
                }
                c.failure += vm.eval("__ceGallery.state().errors.join('\\n')");
                clean = c.failure.empty() && !vm.snapshot_subtree("#root").children.empty();
                if (!clean && c.failure.empty()) {
                    c.failure = "Original case mounted no content";
                }
                vm.eval("__ceGallery.unmount()");
            } catch (const std::exception& e) {
                c.failure += e.what();
                clean = false;
            }
            vm.reset_document();
            vm.collect_garbage();
            const auto final = vm.stats();
            require(final.live_nodes == baseline.live_nodes && final.native_roots == baseline.native_roots &&
                        !final.canvas_contexts,
                    "preview isolated observation leaked owner resources");
            c.execution = clean ? "no_error_observed" : "blocked";
        }
#endif
        cases.push_back(std::move(c));
    }
    return cases;
}

struct GalleryPreviewSamples {
    // Sliding 600-frame warm window, bounded.
    std::deque<double> cpu;
    std::deque<double> gpu;
    std::deque<double> interval;

    static void add(std::deque<double>& values, double v) {
        if (values.size() == weft::probe_constants::gallery::sample_window) {
            values.pop_front();
        }
        values.push_back(v);
    }

    static double percentile(const std::deque<double>& values, double p) {
        if (values.empty()) {
            return 0;
        }
        std::vector<double> v(values.begin(), values.end());
        std::sort(v.begin(), v.end());
        return v[std::size_t(std::ceil(p * v.size())) - 1];
    }

    static std::string json(const std::deque<double>& v) {
        return "{\"samples\":" + std::to_string(v.size()) + ",\"p50\":" + format_host_time(percentile(v, .5)) +
               ",\"p95\":" + format_host_time(percentile(v, .95)) + ",\"p99\":" + format_host_time(percentile(v, .99)) +
               ",\"max\":" + format_host_time(percentile(v, 1)) + "}";
    }
};

FramePacket gallery_preview_hud(weft::FontSet& fonts, const WindowFrameRate& rate, unsigned live,
                                const GalleryPreviewSamples& samples, bool paused, std::string owner_label = {},
                                bool detailed = true) {
    weft::PositionedPage page;
    const std::array<float, 4> clip{3000, 16, 824, detailed ? 180.f : 100.f};
    weft::PositionedPaint bg;
    bg.paint.kind = weft::PaintKind::Rectangle;
    bg.paint.x = clip[0];
    bg.paint.y = clip[1];
    bg.paint.width = clip[2];
    bg.paint.height = clip[3];
    bg.paint.srgb = {.025f, .04f, .035f, .97f};
    bg.clip = clip;
    page.paints.push_back(bg);
    auto text = [&](std::string value, float y, float size) {
        weft::PositionedPaint p;
        p.paint.kind = weft::PaintKind::Text;
        p.paint.x = 3020;
        p.paint.y = y;
        p.paint.font_size = size;
        p.paint.srgb = {.8f, 1, .8f, 1};
        p.paint.shaped = fonts.shape(value, size).shaped;
        p.clip = clip;
        page.paints.push_back(std::move(p));
    };
    std::ostringstream fps;
    fps << "FPS: ";
    if (paused) {
        fps << "paused";
    } else if (rate.ready) {
        fps << std::fixed << std::setprecision(1) << rate.fps;
    } else {
        fps << "warming up";
    }
    text(fps.str() + "  |  LIVE " + std::to_string(live) + " / 66", 58, 32);
    if (!detailed) {
        text("Full speed | F8 / sidebar: detailed profiler", 95, 22);
        return cheetah_packet(page, fonts, {});
    }
    std::ostringstream cpu;
    cpu << "CPU p50 / p95 / p99: " << std::fixed << std::setprecision(2)
        << GalleryPreviewSamples::percentile(samples.cpu, .5) << " / "
        << GalleryPreviewSamples::percentile(samples.cpu, .95) << " / "
        << GalleryPreviewSamples::percentile(samples.cpu, .99) << " ms";
    text(cpu.str(), 99, 24);
    text("3840 x 2160 | D3D11 | CSS / SVG GPU at 2x", 137, 24);
    text(owner_label.empty() ? "66 native cases / 6 diagnostic cases; GPU vector raster" : owner_label, 177, 21);
    return cheetah_packet(page, fonts, {});
}

FramePacket gallery_preview_packet(weft::PositionedPage page, weft::FontSet& fonts,
                                   const std::map<std::string, std::shared_ptr<const Asset>>& images,
                                   CheetahPacketCache* cache) {
    for (auto& item : page.paints) {
        auto& p = item.paint;
        if (p.kind == weft::PaintKind::Rectangle && p.id.starts_with("slot-") && images.contains(p.id)) {
            item.image_source = p.id;
        }
        for (unsigned i = 0; i < weft::probe_constants::gallery::canvas_case_count; ++i) {
            if (p.kind == weft::PaintKind::Rectangle && p.id == (i ? "slot-webgl-01" : "slot-webgl-00") &&
                images.contains(i ? "live-1" : "live-0")) {
                item.image_source = i ? "live-1" : "live-0";
            }
        }
        p.x *= weft::probe_constants::gallery::shell_device_scale;
        p.y *= weft::probe_constants::gallery::shell_device_scale;
        p.width *= weft::probe_constants::gallery::shell_device_scale;
        p.height *= weft::probe_constants::gallery::shell_device_scale;
        p.font_size *= weft::probe_constants::gallery::shell_device_scale;
        p.shaped.advance *= weft::probe_constants::gallery::shell_device_scale;
        for (auto& g : p.shaped.glyphs) {
            g.advance_x *= weft::probe_constants::gallery::shell_device_scale;
            g.advance_y *= weft::probe_constants::gallery::shell_device_scale;
            g.offset_x *= weft::probe_constants::gallery::shell_device_scale;
            g.offset_y *= weft::probe_constants::gallery::shell_device_scale;
        }
        for (auto& v : item.clip) {
            v *= weft::probe_constants::gallery::shell_device_scale;
        }
        for (auto& c : item.rounded_clips) {
            for (auto& v : c.rect) {
                v *= weft::probe_constants::gallery::shell_device_scale;
            }
            for (auto& v : c.radius_x) {
                v *= weft::probe_constants::gallery::shell_device_scale;
            }
            for (auto& v : c.radius_y) {
                v *= weft::probe_constants::gallery::shell_device_scale;
            }
        }
    }
    return cheetah_packet(page, fonts, images, cache);
}

void gallery_preview_packet_equal(const FramePacket& cached, const FramePacket& full) {
    require(cached.draws.size() == full.draws.size(), "gallery cached/full draw count");
    for (std::size_t i = 0; i < full.draws.size(); ++i) {
        const auto& a = cached.draws[i];
        const auto& b = full.draws[i];
        require(a.rect.x == b.rect.x && a.rect.y == b.rect.y && a.rect.w == b.rect.w && a.rect.h == b.rect.h &&
                    a.srgb == b.srgb && a.uv == b.uv,
                "gallery cached/full paint/clip order");
        require((a.asset < 0) == (b.asset < 0), "gallery cached/full missing asset");
        if (a.asset >= 0) {
            const auto& x = *cached.assets[unsigned(a.asset)];
            const auto& y = *full.assets[unsigned(b.asset)];
            require(x.width == y.width && x.height == y.height && x.encoding == y.encoding && x.bytes == y.bytes,
                    "gallery cached/full asset content");
        }
    }
}

int run_gallery_preview(bool warp, const std::filesystem::path& folder, bool self_test, std::string benchmark = {},
                        bool threaded = true, bool scroll_test = false, bool profile = false) {
    const bool input_benchmark = benchmark.starts_with("input-");
    const auto scenario = input_benchmark ? benchmark.substr(6) : benchmark;
    std::filesystem::create_directories(folder);
    const auto startup = std::chrono::steady_clock::now();
    std::cout << "Observing 72 fixed original cases (isolated, 12 opportunities; no full CSS/pixel pass)...\n";
    auto profile_control = std::make_shared<TraceControl>();
    profile_control->enabled = profile || !benchmark.empty();
    GalleryLiveProfiler live_profiler;
    live_profiler.enabled = profile;
    OwnerTrace render_trace(folder / "render-trace.json", weft::probe_constants::gallery::render_trace_tid,
                            profile_control);
    double shell_layout_ms{};
    double shell_input_ms{};
    std::size_t shell_layout_builds{};
    std::size_t layouts_before_profile_click{};
    unsigned captures_before_profile_click{};
    const auto cases = gallery_preview_observe();
    const auto records = gallery_preview_json(cases);
    write_text(
        folder / "preview-cases.json",
        "{\"format\":\"gallery-preview-cases-v1\",\"case_count\":72,\"native_pixel_cases\":66,\"original_full_gallery_rendered\":false,\"conditions\":{\"isolated_opportunities\":12,\"canvas_host\":true,\"css\":\"Fixed Tailwind utilities, 64 original CSS/SVG/animation scenes and two original Canvas cases\",\"input_dispatched\":false},\"cases\":" +
            records + "}");
    NativeReactWindow window(!self_test && benchmark.empty(), true);
    window.state.logical_width = weft::probe_constants::gallery::shell_width;
    window.state.logical_height = weft::probe_constants::gallery::shell_height;
    Experiment gpu(warp, weft::constants::uhd_width, weft::constants::uhd_height);
    gpu.cache_assets = true;
    gpu.cache_byte_limit = weft::probe_constants::gallery::asset_cache_budget_bytes;
    gpu.enable_timing();
    WindowPresenter presenter(gpu, window.state.hwnd);
    TwoFramePipeline pipeline(gpu);
    SceneGpuSurfaces scene_gpu(gpu, pipeline);
    scene_gpu.retain_paint = true;
    scene_gpu.native_paint = true;
    pipeline.set_timing(profile || self_test || !benchmark.empty());
    if (self_test || !benchmark.empty()) {
        window.client_size(weft::constants::uhd_width, weft::constants::uhd_height);
    }
    if (self_test) {
        window.client_size(weft::constants::uhd_width, weft::constants::uhd_height);
        presenter.capture = true;
        if (scroll_test) {
            window.state.wheel_lines = weft::probe_constants::gallery::scroll_test_wheel_lines;
        }
    }
    weft::FontSet fonts({WEFT_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf",
                         WEFT_SOURCE_DIR "/third_party/noto-cjk/source/NotoSansCJKsc-Regular.otf",
                         WEFT_SOURCE_DIR "/third_party/harfbuzz/source/test/api/fonts/Mplus1p-Regular.ttf"});
    CheetahPacketCache cache;
    weft::ScriptDom shell(
        {weft::probe_constants::gallery::vm_stack_bytes, weft::probe_constants::gallery::host_vm_execution_budget_ms});
    weft::install_script_platform(shell, weft::ScriptPlatformModule::WebHost);
    shell.eval(read_file(WEFT_SOURCE_DIR "/examples/crpg-gallery/native/case-list.js"));
    shell.eval(read_file(WEFT_SOURCE_DIR "/examples/crpg-gallery/preview.js"));
    shell.eval("__cePreview.update(" + records + ")");
    const auto css = read_file(WEFT_SOURCE_DIR "/examples/crpg-gallery/preview.css");
    weft::CompiledStyle shell_styles(css, weft::flow_page_style_profile());
    weft::StyleMemo shell_memo;
    std::optional<weft::ComputedStyle> shell_computed;
    std::optional<weft::DomSnapshot> shell_style_key;
    std::optional<weft::DomSnapshot> layout_input;
    std::optional<weft::FlowPage> layout_result;
    std::function<void(const weft::DomSnapshot&)> check_text = [&](const auto& n) {
        if (n.type == 3) {
            auto text = n.data;
            std::replace(text.begin(), text.end(), '\n', ' ');
            if (fonts.shape(text, weft::constants::default_font_size_px).shaped.missing_glyph) {
                throw std::runtime_error("WEFT_GALLERY_FONT: " + text);
            }
        }
        for (const auto& c : n.children) {
            check_text(c);
        }
    };
    check_text(shell.snapshot_subtree("#shell"));
    auto layout = [&](const weft::DomSnapshot& dom) {
        auto key = dom;
        shell_styles.normalize_selector_state(key);
        if (layout_input && *layout_input == key) {
            return *layout_result;
        }
        ++shell_layout_builds;
        const auto begin =
            render_trace.enabled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        auto style_key = shell_styles.input_key(key);
        if (!shell_style_key || *shell_style_key != style_key) {
            if (!shell_computed) {
                shell_computed.emplace();
            }
            shell_styles.update(key, *shell_computed, shell_memo);
            shell_style_key = std::move(style_key);
        }
        auto result = weft::build_flow_page(dom, *shell_computed, weft::probe_constants::gallery::shell_width,
                                            weft::probe_constants::gallery::shell_height, fonts);
        layout_input = std::move(key);
        layout_result = result;
        if (render_trace.enabled()) {
            const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            shell_layout_ms += ms;
            render_trace.emit("shell layout", begin, ms);
        }
        return result;
    };
    shell.install_layout_host([&](const auto& document) {
        return layout(flow_find(document, "shell")).elements;
    });
    // Geometry writes require the installed owner layout host.
    shell.eval("__cePreview.jump('CSS_TESTS')");
    auto page = [&] {
        return layout(shell.snapshot_subtree("#shell"));
    };
    weft::MouseRouter mouse(shell, [&] {
        return page().page.geometry;
    });
    std::unique_ptr<GalleryOwnerExecutor> owner;
    if (threaded) {
        owner = gallery_owner(true, folder / "owner-trace.json", profile_control);
    }
    std::shared_ptr<const GalleryOwnerFrame> owner_frame;
    std::array<std::uint64_t, gallery_scene_count> rendered_revisions{};
    std::deque<double> owner_ms;
    std::deque<double> owner_age;
    std::deque<double> owner_queue;
    std::size_t owner_updates{};
    std::size_t reused_owner_frames{};
    double last_owner_active{};
    WindowFrameRate owner_rate;
    std::array<std::unique_ptr<weft::SceneRaster>, gallery_scene_count> scene_reference;
    if (owner && self_test) {
        for (auto& r : scene_reference) {
            r = std::make_unique<weft::SceneRaster>();
        }
    }
    std::array<std::unique_ptr<GalleryCanvasSession>, weft::probe_constants::gallery::canvas_case_count> sessions;
    if (!owner) {
        for (unsigned i = 0; i < weft::probe_constants::gallery::canvas_case_count; ++i) {
            sessions[i] = std::make_unique<GalleryCanvasSession>(i);
        }
    }
    std::vector<std::unique_ptr<GallerySceneSession>> scenes;
    std::vector<weft::NodeIdentity> scene_slots;
    std::vector<std::shared_ptr<const Asset>> scene_images(gallery_scene_count);
    std::array<int, gallery_scene_count> scene_asset_indices{};
    scene_asset_indices.fill(-1);
    std::array<bool, gallery_scene_count> scene_visible{};
    for (unsigned i = 0; i < gallery_scene_count; ++i) {
        const auto d = gallery_scene_descriptors[i];
        require(cases[d.catalog_index].group == d.group && cases[d.catalog_index].index == d.index,
                "scene/catalog manifest drift");
        if (!owner) {
            scenes.push_back(std::make_unique<GallerySceneSession>(d.group, d.index, d.width, d.height,
                                                                   i < weft::probe_constants::gallery::core_scene_count
                                                                       ? "scene-utilities.css"
                                                                       : "cluster-utilities.css"));
        }
        if (!owner) {
            scenes.back()->set_profiling(profile || !benchmark.empty());
        }
        scene_slots.push_back(shell.node_identity("#slot-" + cases[d.catalog_index].id));
    }
    const auto started = std::chrono::steady_clock::now();
    auto seconds = [&] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    };
    auto phase_seconds = [&] {
        return live_profiler.enabled || self_test || !benchmark.empty() ? seconds() : 0.;
    };
    if (live_profiler.enabled) {
        shell.eval("__cePreview.setProfiler(true)");
    }
    std::map<std::string, std::deque<double>> stages;
    double benchmark_start{};
    double benchmark_end{};
    GalleryPreviewSamples samples;
    WindowFrameRate fps;
    fps.reset(seconds());
    unsigned frames{};
    unsigned live_count{};
    unsigned settling{};
    unsigned pause_count{};
    int pressed = -1;
    int captured_scene = -1;
    bool inactive{};
    std::string scope;
    std::string detail = shell.eval("__cePreview.detail");
    double last_present{};
    double last_time{};
    double startup_ms{};
    std::size_t oracle_samples{};
    std::size_t canvas_inputs{};
    std::size_t cache_differences{};
    std::size_t geometry_checks{};
    unsigned pixel_error{};
    unsigned captures{};
    unsigned seen_cases{};
    std::array<unsigned, weft::probe_constants::gallery::canvas_case_count> raf_calls{};
    const char* groups[]{"CSS_TESTS",    "D3_TESTS",     "D3_INTERACTIVE_TESTS", "KLINE_TESTS", "WEBGL_TESTS",
                         "FRAMER_TESTS", "SPRING_TESTS", "ICON_TESTS",           "ANIME_TESTS", "LOTTIE_TESTS"};
    std::array<bool, weft::probe_constants::gallery::catalog_case_count> seen{};
    std::array<bool, weft::probe_constants::gallery::canvas_case_count> visible{};
    const std::array<weft::NodeIdentity, weft::probe_constants::gallery::canvas_case_count> slot_ids{
        shell.node_identity("#slot-webgl-00"), shell.node_identity("#slot-webgl-01")};
    const auto scroll_id = shell.node_identity("#scroller");
    const auto profiler_id = shell.node_identity("#profiler-toggle");
    std::uint64_t profiler_revision{};
    FramePacket static_packet;
    std::optional<weft::DomSnapshot> packet_input;
    std::array<int, weft::probe_constants::gallery::canvas_case_count> canvas_assets{-1, -1};
    std::size_t static_rebuilds{};
    std::map<std::uint64_t, unsigned> measured_serials;
    unsigned sample_epoch{};
    pipeline.completed_timing = [&](std::uint64_t serial, double ms) {
        if (const auto i = measured_serials.find(serial); i != measured_serials.end()) {
            if (i->second == sample_epoch) {
                GalleryPreviewSamples::add(samples.gpu, ms);
                live_profiler.add("GPU elapsed", seconds(), ms);
            }
            measured_serials.erase(i);
        }
    };
    auto reset = [&] {
        samples = {};
        ++sample_epoch;
        fps.reset(seconds());
        last_present = 0;
        settling = 0;
    };
    auto hud = [&] {
        std::ostringstream label;
        if (owner) {
            label << "Owner " << std::fixed << std::setprecision(1) << owner_rate.fps << " Hz | P50 "
                  << GalleryPreviewSamples::percentile(owner_ms, .5) << "ms | P99 "
                  << GalleryPreviewSamples::percentile(owner_ms, .99) << "ms | age "
                  << GalleryPreviewSamples::percentile(owner_age, .99) << "ms | inputQ "
                  << (owner->work().accepted_inputs - owner->work().processed_inputs - owner->work().coalesced_inputs);
        }
        if (live_profiler.enabled && live_profiler.last_draw >= 0 &&
            seconds() - live_profiler.last_draw < weft::probe_constants::gallery::hud_refresh_interval_s) {
            return;
        }
        auto overlay =
            live_profiler.enabled
                ? live_profiler.packet(fonts, fps, owner_rate, live_count,
                                       owner ? owner->work().accepted_inputs - owner->work().processed_inputs -
                                                   owner->work().coalesced_inputs
                                             : 0,
                                       seconds())
                : gallery_preview_hud(fonts, fps, live_count, samples, inactive, label.str(), false);
        gallery_append_detail(overlay, fonts, detail);
        presenter.overlay(overlay);
    };
    hud();
    auto jump = [&](unsigned group) {
        shell.eval("__cePreview.jump(" + json_string(groups[group]) + ")");
    };
    if (!benchmark.empty()) {
        const std::map<std::string, unsigned> cluster_groups{{"d3-interactive", 2}, {"scatter", 2}, {"kline", 3},
                                                             {"framer", 5},         {"spring", 6},  {"icons", 7},
                                                             {"anime", 8}};
        require(scenario == "css-top" || scenario == "css-middle" || scenario == "css-end" || scenario == "d3" ||
                    cluster_groups.contains(scenario),
                "benchmark scenario");
        if (cluster_groups.contains(scenario)) {
            jump(cluster_groups.at(scenario));
        }
        if (scenario == "scatter") {
            shell.eval("__cePreview.scroll(4668)");
        }
        if (scenario == "d3") {
            jump(1);
        }
        if (scenario == "css-middle") {
            shell.eval("document.getElementById('scroller').scrollTop=1000");
        }
        if (scenario == "css-end") {
            shell.eval("document.getElementById('scroller').scrollTop=1800");
        }
    }

    while (!window.state.closed) {
        const auto frame_trace_start =
            render_trace.enabled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const double tick = seconds();
        shell_layout_ms = shell_input_ms = 0;
        pipeline.poll();
        MSG key{};
        while (PeekMessageW(&key, window.state.hwnd, WM_KEYDOWN, WM_KEYDOWN, PM_REMOVE)) {
            if (key.wParam == VK_ESCAPE) {
                window.state.close_requested = true;
            }
            if (key.wParam == VK_HOME) {
                shell.eval("document.getElementById('scroller').scrollTop=0");
            }
            if (key.wParam == VK_END) {
                shell.eval("document.getElementById('scroller').scrollTop=1e9");
            }
            if (key.wParam == VK_PRIOR) {
                shell.eval("__cePreview.scroll(-840)");
            }
            if (key.wParam == VK_NEXT) {
                shell.eval("__cePreview.scroll(840)");
            }
            if (key.wParam == VK_F5) {
                reset();
                live_profiler.clear();
            }
            if (key.wParam == VK_F8) {
                shell.eval("__cePreview.setProfiler(!__cePreview.profiler)");
            }
        }
        window.messages();
        if (window.state.closed) {
            break;
        }
        if (window.state.minimized) {
            if (!inactive) {
                inactive = true;
                ++pause_count;
                reset();
                hud();
            }
            if (self_test) {
                require(PostMessageW(window.state.hwnd, WM_SIZE, SIZE_RESTORED,
                                     MAKELPARAM(weft::constants::uhd_width, weft::constants::uhd_height)),
                        "gallery restore");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(weft::probe_constants::gallery::minimized_poll_ms));
            continue;
        }
        if (window.state.resize) {
            pipeline.drain();
            presenter.resize(window.state.width, window.state.height);
            window.state.resize = false;
            reset();
        }
        if (inactive) {
            inactive = false;
            reset();
        }
        // Shared exact wall-clock timestamp, never a rounded timer timestamp.
        const double time = (benchmark.empty() ? seconds() * weft::constants::milliseconds_per_second
                                               : frames * weft::probe_constants::gallery::fixed_clock_step_ms) +
                            weft::probe_constants::gallery::host_clock_offset_ms;
        require(time >= last_time, "preview owner clock regressed");
        last_time = time;
        auto current = page();
        auto scene_rect = [&](unsigned index) {
            for (const auto& e : current.elements) {
                if (e.identity == scene_slots[index]) {
                    return e.rect;
                }
            }
            throw std::runtime_error("scene slot geometry absent");
        };
        auto slot_rect = [&](unsigned index) {
            for (const auto& e : current.elements) {
                if (e.identity == slot_ids[index]) {
                    return e.rect;
                }
            }
            throw std::runtime_error("preview slot geometry absent");
        };
        while (!window.state.input.empty()) {
            const auto value = window.state.input.front();
            window.state.input.pop_front();
            const auto point = value.viewport.logical(value.x, value.y);
            if (value.action == weft::MouseAction::Cancel && !value.wheel) {
                if (owner) {
                    require(gallery_owner_input(*owner, {-1, -1, value.action, 0, 0, time}) ==
                                GalleryOwnerExecutor::Accepted,
                            "owner cancel backpressure");
                } else {
                    for (auto& scene : scenes) {
                        scene->time = time;
                        scene->cancel_input();
                    }
                }
                pressed = captured_scene = -1;
                mouse.cancel();
                continue;
            }
            int target = -1;
            double x{};
            double y{};
            int scene_target = -1;
            for (unsigned i = 0; i < gallery_scene_count; ++i) {
                auto r = scene_rect(i);
                if (point[0] >= r[0] && point[0] < r[0] + r[2] &&
                    point[1] >= std::max(static_cast<double>(weft::probe_constants::gallery::shell_viewport_top),
                                         double(r[1])) &&
                    point[1] < std::min(static_cast<double>(weft::probe_constants::gallery::shell_viewport_bottom),
                                        double(r[1] + r[3]))) {
                    scene_target = int(i);
                    break;
                }
            }
            // The inspection host reserves wheel over the zoomable SVG for the
            // original D3 zoom handler. Other wheel input scrolls the shell.
            if (value.wheel) {
                if (scene_target == weft::probe_constants::gallery::scatter_scene) {
                    const auto r = scene_rect(weft::probe_constants::gallery::scatter_scene);
                    if (owner) {
                        GalleryOwnerInput input{weft::probe_constants::gallery::scatter_scene,
                                                -1,
                                                weft::MouseAction::Move,
                                                point[0] - r[0],
                                                point[1] - r[1],
                                                time};
                        input.wheel = true;
                        input.delta_y = value.delta_y;
                        input.buttons = value.buttons;
                        input.received = value.received;
                        require(gallery_owner_input(*owner, input) == GalleryOwnerExecutor::Accepted,
                                "owner wheel backpressure");
                    } else {
                        scenes[weft::probe_constants::gallery::scatter_scene]->time = time;
                        scenes[weft::probe_constants::gallery::scatter_scene]->input_value(
                            weft::MouseType::Move, float(point[0] - r[0]), float(point[1] - r[1]), value.buttons,
                            value.delta_y);
                    }
                } else {
                    mouse.wheel({value.delta_x, value.delta_y, point[0], point[1], double(value.screen_x),
                                 double(value.screen_y), time, value.ctrl, value.shift, value.alt, value.meta});
                    current = page();
                }
                continue;
            }
            if (value.action == weft::MouseAction::Down &&
                scene_target >= weft::probe_constants::gallery::core_scene_count) {
                captured_scene = scene_target;
            }
            const int recipient = captured_scene >= 0 ? captured_scene : scene_target;
            if (recipient >= 0) {
                const auto r = scene_rect(unsigned(recipient));
                if (owner) {
                    GalleryOwnerInput input{recipient, -1, value.action, point[0] - r[0], point[1] - r[1], time};
                    input.buttons = value.buttons;
                    input.received = value.received;
                    require(gallery_owner_input(*owner, input) == GalleryOwnerExecutor::Accepted,
                            "owner input backpressure");
                } else {
                    auto& scene = *scenes[unsigned(recipient)];
                    scene.time = time;
                    scene.pump();
                    scene.flush_scene();
                    if (scene.svg_geometry) {
                        scene.input_value(value.action == weft::MouseAction::Down ? weft::MouseType::Down
                                          : value.action == weft::MouseAction::Up ? weft::MouseType::Up
                                                                                  : weft::MouseType::Move,
                                          float(point[0] - r[0]), float(point[1] - r[1]), value.buttons);
                    } else {
                        if (value.action == weft::MouseAction::Move) {
                            scene.input(weft::MouseType::Move, float(point[0] - r[0]), float(point[1] - r[1]));
                        }
                        if (value.action == weft::MouseAction::Down) {
                            scene.pressed =
                                scene.raster.hit_test(scene.current, float(point[0] - r[0]), float(point[1] - r[1]));
                        }
                        if (value.action == weft::MouseAction::Up) {
                            auto hit =
                                scene.raster.hit_test(scene.current, float(point[0] - r[0]), float(point[1] - r[1]));
                            if (scene.pressed == hit) {
                                scene.input(weft::MouseType::Click, float(point[0] - r[0]), float(point[1] - r[1]));
                            }
                            scene.pressed = {};
                        }
                    }
                }
            }
            if (value.action == weft::MouseAction::Up) {
                captured_scene = -1;
            }
            if (owner && recipient < 0 && value.action == weft::MouseAction::Move) {
                require(gallery_owner_input(*owner, {-1, -1, value.action, 0, 0, time}) ==
                            GalleryOwnerExecutor::Accepted,
                        "owner hover exit backpressure");
            }
            if (!owner && value.action == weft::MouseAction::Move) {
                for (unsigned i = 0; i < gallery_scene_count; ++i) {
                    if (int(i) != recipient && scenes[i]->hovered.generation) {
                        scenes[i]->time = time;
                        scenes[i]->pump();
                        scenes[i]->input(weft::MouseType::Move, weft::probe_constants::gallery::offscreen_pointer,
                                         weft::probe_constants::gallery::offscreen_pointer);
                    }
                }
            }

            for (unsigned i = 0; i < weft::probe_constants::gallery::canvas_case_count; ++i) {
                const auto r = slot_rect(i);
                if (point[0] >= r[0] && point[0] < r[0] + r[2] &&
                    point[1] >= std::max(static_cast<double>(weft::probe_constants::gallery::shell_viewport_top),
                                         double(r[1])) &&
                    point[1] < std::min(static_cast<double>(weft::probe_constants::gallery::shell_viewport_bottom),
                                        double(r[1] + r[3]))) {
                    target = int(i);
                    x = (point[0] - r[0]) * weft::probe_constants::gallery::canvas_backing_width / r[2];
                    y = (point[1] - r[1]) * weft::probe_constants::gallery::canvas_backing_height / r[3];
                }
            }
            if (value.action == weft::MouseAction::Down) {
                pressed = target;
            }
            if (target >= 0 && value.action == weft::MouseAction::Move) {
                if (owner) {
                    require(gallery_owner_input(*owner, {-1, target, value.action, x, y, time}) ==
                                GalleryOwnerExecutor::Accepted,
                            "owner canvas move backpressure");
                } else {
                    sessions[unsigned(target)]->pump(time);
                    sessions[unsigned(target)]->input(0, x, y, time);
                }
                ++canvas_inputs;
            }
            if (value.action == weft::MouseAction::Up) {
                if (target >= 0 && pressed == target) {
                    if (owner) {
                        require(gallery_owner_input(*owner, {-1, target, value.action, x, y, time}) ==
                                    GalleryOwnerExecutor::Accepted,
                                "owner canvas click backpressure");
                    } else {
                        sessions[unsigned(target)]->pump(time);
                        sessions[unsigned(target)]->input(1, x, y, time);
                    }
                    ++canvas_inputs;
                }
                pressed = -1;
            }
            weft::MouseEvent e;
            e.client_x = point[0];
            e.client_y = point[1];
            e.time_ms = time;
            e.buttons = value.buttons;
            const auto begin =
                render_trace.enabled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            mouse.route(value.action, e);
            if (value.action == weft::MouseAction::Up) {
                auto selected = shell.eval("__cePreview.detail");
                if (selected != detail) {
                    detail = std::move(selected);
                    hud();
                }
            }
            current = page();
            if (render_trace.enabled()) {
                const auto ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
                shell_input_ms += ms;
                render_trace.emit("shell input", begin, ms, "{\"action\":" + std::to_string(int(value.action)) + "}");
            }
        }
        const auto toggle_revision = shell.subtree_revision(profiler_id);
        bool chosen = live_profiler.enabled;
        if (toggle_revision != profiler_revision) {
            profiler_revision = toggle_revision;
            chosen = shell.eval("__cePreview.profiler") == "true";
        }
        if (chosen != live_profiler.enabled) {
            live_profiler.enabled = chosen;
            live_profiler.clear();
            profile_control->enabled = chosen || !benchmark.empty();
            pipeline.set_timing(chosen || self_test || !benchmark.empty());
            for (auto& scene : scenes) {
                scene->set_profiling(chosen || !benchmark.empty());
            }
            hud();
        }
        pipeline.begin_frame_timing();
        if (owner) {
            GalleryOwnerRequest request;
            request.time = time;
            request.audit = self_test && !scroll_test && (frames == 21 || frames == 34);
            for (unsigned i = 0; i < gallery_scene_count; ++i) {
                auto r = scene_rect(i);
                request.scenes[i] = r[1] < weft::probe_constants::gallery::shell_viewport_bottom &&
                                    r[1] + r[3] > weft::probe_constants::gallery::shell_viewport_top;
            }
            for (unsigned i = 0; i < weft::probe_constants::gallery::canvas_case_count; ++i) {
                auto r = slot_rect(i);
                request.canvas[i] = r[1] < weft::probe_constants::gallery::shell_viewport_bottom &&
                                    r[1] + r[3] > weft::probe_constants::gallery::shell_viewport_top;
            }
            const auto sequence = owner->request(weft::probe_constants::gallery::owner_epoch, request);
            std::optional<GalleryOwnerExecutor::Publication> publication;
            // The deterministic correctness path includes Debug builds and
            // large original SVG geometry reads. Its host wait is separate
            // from each VM evaluation budget and the interactive wait bound.
            try {
                if (self_test || !benchmark.empty() || !owner_frame) {
                    publication = owner->wait(
                        weft::probe_constants::gallery::owner_epoch, sequence,
                        self_test ? std::chrono::seconds(weft::probe_constants::gallery::owner_self_test_wait_s)
                                  : std::chrono::seconds(weft::probe_constants::gallery::owner_wait_s));
                } else {
                    publication = owner->latest();
                }
            } catch (const std::exception& error) {
                throw std::runtime_error("gallery owner frame=" + std::to_string(frames) +
                                         " sequence=" + std::to_string(sequence) + ": " + error.what());
            }
            if (publication) {
                live_profiler.owner(seconds(), *publication);
                owner_frame = publication->value;
                last_owner_active = publication->active_ms;
                GalleryPreviewSamples::add(owner_ms, publication->active_ms);
                GalleryPreviewSamples::add(owner_queue, publication->queue_ms);
                owner_rate.presented(seconds());
                ++owner_updates;
            } else {
                ++reused_owner_frames;
            }
            require(bool(owner_frame), "owner initial complete frame");
            GalleryPreviewSamples::add(owner_age, std::max(0., time - owner_frame->time));
        }
        std::string next_scope;
        for (const auto& e : current.elements) {
            if (e.identity == scroll_id) {
                next_scope = format_host_time(e.scroll_top);
            }
        }
        live_count = 0;
        for (unsigned i = 0; i < weft::probe_constants::gallery::canvas_case_count; ++i) {
            const auto r = slot_rect(i);
            visible[i] = r[1] < weft::probe_constants::gallery::shell_viewport_bottom &&
                         r[1] + r[3] > weft::probe_constants::gallery::shell_viewport_top;
            live_count += visible[i];
            next_scope += visible[i] ? "/live" : "/hidden";
            if (owner) {
                raf_calls[i] = owner_frame->raf_calls[i];
                continue;
            }
            auto& s = *sessions[i];
            s.pump(time);
            const auto raf = s.vm.animation_frame(time, visible[i]);
            s.pump(time);
            raf_calls[i] += unsigned(raf.callbacks);
            require(!raf.exceptions && s.vm.stats().listener_exceptions == 0 && raf.callbacks == (visible[i] ? 1 : 0),
                    "preview live Canvas scheduling/input failed");
        }
        const auto scene_started = phase_seconds();
        double js_ms{};
        double snapshot_ms{};
        double scene_ms{};
        double raster_ms{};
        if (owner) {
            js_ms = owner_frame->js_ms;
            snapshot_ms = owner_frame->snapshot_ms;
            scene_ms = owner_frame->scene_ms;
        }
        for (unsigned i = 0; i < gallery_scene_count; ++i) {
            const auto r = scene_rect(i);
            scene_visible[i] = r[1] < weft::probe_constants::gallery::shell_viewport_bottom &&
                               r[1] + r[3] > weft::probe_constants::gallery::shell_viewport_top;
        }
        scene_gpu.retire_hidden(scene_visible);
        for (unsigned i = 0; i < gallery_scene_count; ++i) {
            const auto r = scene_rect(i);
            live_count += scene_visible[i];
            if (!owner) {
                auto& s = *scenes[i];
                if (scene_visible[i]) {
                    s.step(time, true);
                    js_ms += s.js_ms;
                    snapshot_ms += s.snapshot_ms;
                    scene_ms += s.scene_ms;
                } else {
                    s.time = time;
                    s.pump();
                    s.vm.animation_frame(time, false);
                    s.pump();
                }
            }
            const auto& frame = owner ? *owner_frame->scenes[i] : scenes[i]->current;
            const bool dirty = owner ? owner_frame->revisions[i] != rendered_revisions[i] : scenes[i]->dirty;
            const std::array<float, 4> crop{
                0, float(std::max(0., weft::probe_constants::gallery::shell_viewport_top - r[1])), float(r[2]),
                float(std::min(r[3], weft::probe_constants::gallery::shell_viewport_bottom - r[1]))};
            if (scene_visible[i] && (dirty || scene_gpu.needs_clip(i, crop))) {
                const auto paint_trace_start =
                    render_trace.enabled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                const auto paint_start = phase_seconds();
                auto snapshot = owner ? owner_frame->scenes[i] : std::make_shared<const weft::SceneFrame>(frame);
                scene_gpu.paint_owned(i, std::move(snapshot), weft::probe_constants::gallery::scene_raster_scale, crop,
                                      gallery_layer_manifest(i, frame, true));
                const auto paint_ms = (phase_seconds() - paint_start) * weft::constants::milliseconds_per_second;
                raster_ms += paint_ms;
                if (render_trace.enabled()) {
                    render_trace.emit(
                        "paint " + cases[gallery_scene_descriptors[i].catalog_index].id, paint_trace_start, paint_ms,
                        "{\"native\":" +
                            (scene_gpu.image(i)->last_native ? std::string("true") : std::string("false")) + "}");
                }
                if (!benchmark.empty() && frames >= weft::probe_constants::gallery::warmup_frames) {
                    GalleryPreviewSamples::add(stages["paint-" + cases[gallery_scene_descriptors[i].catalog_index].id],
                                               paint_ms);
                }
            }
            if (scene_visible[i] && (dirty || !scene_images[i])) {
                const auto raster_start = phase_seconds();
                if (self_test) {
                    const auto b =
                        owner ? scene_reference[i]->raster(frame, weft::probe_constants::gallery::scene_raster_scale)
                              : scenes[i]->bitmap();
                    scene_images[i] =
                        std::make_shared<const Asset>(Asset{b.width, b.height, Encoding::SrgbStraight, b.rgba});
                } else {
                    scene_images[i] =
                        std::make_shared<const Asset>(Asset{1, 1, Encoding::SrgbStraight, {5, 4, 3, 255}});
                }
                raster_ms += (phase_seconds() - raster_start) * weft::constants::milliseconds_per_second;
            }
            if (scene_visible[i]) {
                if (owner) {
                    rendered_revisions[i] = owner_frame->revisions[i];
                } else {
                    scenes[i]->dirty = false;
                }
            }
        }
        {
            const auto publish_start = phase_seconds();
            scene_gpu.publish();
            raster_ms += (phase_seconds() - publish_start) * weft::constants::milliseconds_per_second;
        }
        if (scope != next_scope) {
            scope = next_scope;
            reset();
            hud();
        }
        auto dom = shell.snapshot_subtree("#shell");
        shell_styles.normalize_selector_state(dom);
        if (!packet_input || *packet_input != dom) {
            std::map<std::string, std::shared_ptr<const Asset>> images;
            canvas_assets = {-1, -1};
            scene_asset_indices.fill(-1);
            for (unsigned i = 0; i < gallery_scene_count; ++i) {
                if (scene_visible[i]) {
                    images.emplace("slot-" + cases[gallery_scene_descriptors[i].catalog_index].id, scene_images[i]);
                }
            }
            for (unsigned i = 0; i < weft::probe_constants::gallery::canvas_case_count; ++i) {
                if (visible[i]) {
                    images.emplace(i ? "live-1" : "live-0",
                                   std::make_shared<const Asset>(Asset{1, 1, Encoding::SrgbStraight, {10, 8, 6, 255}}));
                }
            }
            // The authored shell is 1920x1080; device ppem and every clip are 2x.
            static_packet = gallery_preview_packet(current.page, fonts, images, &cache);
            packet_input = dom;
            ++static_rebuilds;
            for (unsigned i = 0; i < weft::probe_constants::gallery::canvas_case_count; ++i) {
                if (visible[i]) {
                    const auto asset = images.at(i ? "live-1" : "live-0");
                    for (std::size_t a = 0; a < static_packet.assets.size(); ++a) {
                        if (static_packet.assets[a] == asset) {
                            canvas_assets[i] = int(a);
                        }
                    }
                    require(canvas_assets[i] >= 0, "visible Canvas placeholder missing");
                }
            }
        }
        if (packet_input && *packet_input == dom) {
            for (unsigned i = 0; i < gallery_scene_count; ++i) {
                if (scene_visible[i] && scene_asset_indices[i] < 0) {
                    for (std::size_t a = 0; a < static_packet.assets.size(); ++a) {
                        if (static_packet.assets[a] == scene_images[i]) {
                            scene_asset_indices[i] = int(a);
                        }
                    }
                }
            }
        }
        auto packet = static_packet; // Complete owning packet, independent of older submissions.
        for (unsigned i = 0; i < gallery_scene_count; ++i) {
            if (scene_visible[i]) {
                require(scene_asset_indices[i] >= 0, "visible scene placeholder asset");
                packet.assets[unsigned(scene_asset_indices[i])] = scene_images[i];
            }
        }
        for (unsigned i = 0; i < weft::probe_constants::gallery::canvas_case_count; ++i) {
            if (visible[i]) {
                const auto bitmap =
                    owner ? owner_frame->canvas[i] : sessions[i]->vm.canvas_snapshot(sessions[i]->identity());
                require(canvas_assets[i] >= 0, "Canvas stale shell packet");
                packet.assets[unsigned(canvas_assets[i])] = std::make_shared<const Asset>(
                    Asset{bitmap.width, bitmap.height, Encoding::SrgbStraight, *bitmap.rgba});
            }
        }
        if (self_test && (frames < 16 || frames == 159)) {
            std::map<std::string, std::shared_ptr<const Asset>> images;
            for (unsigned i = 0; i < gallery_scene_count; ++i) {
                if (scene_visible[i]) {
                    images.emplace("slot-" + cases[gallery_scene_descriptors[i].catalog_index].id, scene_images[i]);
                }
            }
            for (unsigned i = 0; i < weft::probe_constants::gallery::canvas_case_count; ++i) {
                if (visible[i]) {
                    images.emplace(i ? "live-1" : "live-0", packet.assets[unsigned(canvas_assets[i])]);
                }
            }
            const auto full = gallery_preview_packet(current.page, fonts, images, nullptr);
            gallery_preview_packet_equal(packet, full);
            ++cache_differences;
        }
        const auto submit_start = phase_seconds();
        auto gpu_packet = packet;
        const auto external = scene_gpu.dependencies(gpu_packet, scene_asset_indices, scene_visible);
        const auto serial = pipeline.submit(
            std::move(gpu_packet),
            [&] {
                presenter.present();
            },
            presenter.overlay_lease(), external);
        scene_gpu.submitted(serial, scene_visible);
        ++frames;
        if (render_trace.enabled() && frame_trace_start != std::chrono::steady_clock::time_point{}) {
            render_trace.emit(
                "render frame", frame_trace_start, (seconds() - tick) * weft::constants::milliseconds_per_second,
                "{\"owner_time_ms\":" + format_host_time(owner_frame ? owner_frame->time : time) +
                    ",\"owner_age_ms\":" + format_host_time(owner_frame ? std::max(0., time - owner_frame->time) : 0) +
                    ",\"serial\":" + std::to_string(serial) + "}");
        }
        if (!startup_ms) {
            startup_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startup).count();
        }
        if (live_profiler.enabled) {
            ++live_profiler.render_samples;
            const auto t = seconds();
            live_profiler.add("Render CPU", t, (t - tick) * weft::constants::milliseconds_per_second);
            live_profiler.add("Frame age", t, owner_frame ? std::max(0., time - owner_frame->time) : 0);
            live_profiler.add("Shell layout", t, shell_layout_ms);
            live_profiler.add("Shell input", t, shell_input_ms);
            live_profiler.add("Raster", t, raster_ms);
            live_profiler.add("Prepare", t, gpu.prepare_ms);
            live_profiler.add("Encode", t, gpu.encode_ms);
            live_profiler.add("Present CPU", t, gpu.present_ms);
            live_profiler.add("Pipeline wait", t, gpu.wait_ms);
            if (t - live_profiler.last_draw >= weft::probe_constants::gallery::hud_refresh_interval_s) {
                hud();
            }
        }
        if (!benchmark.empty() && frames == weft::probe_constants::gallery::warmup_frames) {
            benchmark_start = seconds();
        }
        if (!benchmark.empty() && frames > weft::probe_constants::gallery::warmup_frames) {
            GalleryPreviewSamples::add(stages["owner_active"],
                                       owner ? last_owner_active : js_ms + snapshot_ms + scene_ms);
            GalleryPreviewSamples::add(stages["js"], js_ms);
            GalleryPreviewSamples::add(stages["snapshot"], snapshot_ms);
            GalleryPreviewSamples::add(stages["scene"], scene_ms);
            GalleryPreviewSamples::add(stages["raster"], raster_ms);
            GalleryPreviewSamples::add(stages["prepare"], gpu.prepare_ms);
            GalleryPreviewSamples::add(stages["encode"], gpu.encode_ms);
            GalleryPreviewSamples::add(stages["present"], gpu.present_ms);
            GalleryPreviewSamples::add(stages["submit"],
                                       (seconds() - submit_start) * weft::constants::milliseconds_per_second);
            GalleryPreviewSamples::add(stages["scene_total"],
                                       (submit_start - scene_started) * weft::constants::milliseconds_per_second);
        }
        if (presenter.last_visible || !benchmark.empty()) {
            ++settling;
            if (settling > weft::probe_constants::gallery::warmup_frames) {
                GalleryPreviewSamples::add(samples.cpu, (seconds() - tick) * weft::constants::milliseconds_per_second);
                if (live_profiler.enabled || self_test || !benchmark.empty()) {
                    measured_serials[serial] = sample_epoch;
                }
                if (last_present) {
                    GalleryPreviewSamples::add(samples.interval,
                                               (seconds() - last_present) * weft::constants::milliseconds_per_second);
                }
            }
            last_present = seconds();
            if (fps.presented(seconds())) {
                hud();
            }
        } else if (!self_test) {
            inactive = true;
            reset();
            hud();
            std::this_thread::sleep_for(
                std::chrono::milliseconds(weft::probe_constants::gallery::hidden_frame_sleep_ms));
        }
        if (!benchmark.empty() && frames == weft::probe_constants::gallery::fixed_clock_run_frames) {
            pipeline.drain();
            benchmark_end = seconds();
            window.state.close_requested = true;
        }
        if (self_test && !scroll_test) {
            // Pixel oracle and every original registry id, across the ten sections.
            if (frames <= weft::probe_constants::gallery::catalog_group_count) {
                pipeline.drain();
                auto oracle_packet = packet;
                for (unsigned i = weft::probe_constants::gallery::core_scene_count; i < gallery_scene_count; ++i) {
                    if (scene_visible[i]) {
                        const auto source = scene_gpu.read(i);
                        oracle_packet.assets[unsigned(scene_asset_indices[i])] = std::make_shared<const Asset>(
                            Asset{source.width, source.height, Encoding::SrgbStraight, source.rgba});
                    }
                }
                const auto pixels = gallery_output(gpu);
                for (int y = weft::probe_constants::gallery::oracle_grid_origin; y < weft::constants::uhd_height;
                     y += weft::probe_constants::gallery::oracle_grid_stride_y) {
                    for (int x = weft::probe_constants::gallery::oracle_grid_origin;
                         x < weft::probe_constants::gallery::oracle_grid_right;
                         x += weft::probe_constants::gallery::oracle_grid_stride_x) {
                        auto expected = cheetah_sample(oracle_packet, x, y);
                        const auto actual = pixels[std::size_t(y) * weft::constants::uhd_width + x];
                        for (unsigned c = 0; c < 4; ++c) {
                            if (auto error = unsigned(std::abs(int(expected[c]) - actual[c])); error > pixel_error) {
                                pixel_error = error;
                                if (error > weft::probe_constants::gallery::max_cpu_oracle_difference) {
                                    std::cerr << "window frame " << frames << " point " << x << "," << y << "," << c
                                              << " expected " << unsigned(expected[c]) << " actual "
                                              << unsigned(actual[c]) << " error " << error << "\n";
                                }
                            }
                        }
                        ++oracle_samples;
                    }
                }
                require(pixel_error <= weft::probe_constants::gallery::max_cpu_oracle_difference,
                        "4K gallery shell/canvas pixel oracle");
                save_png(folder / ("group-" + std::to_string(frames - 1) + ".png"), presenter.pixels(),
                         weft::constants::uhd_width, weft::constants::uhd_height);
                ++captures;
                for (unsigned i = 0; i < weft::probe_constants::gallery::catalog_case_count; ++i) {
                    const auto identity = shell.node_identity("#case-" + cases[i].id);
                    for (const auto& e : current.elements) {
                        if (e.identity == identity &&
                            e.rect[1] < weft::probe_constants::gallery::shell_viewport_bottom &&
                            e.rect[1] + e.rect[3] > weft::probe_constants::gallery::shell_viewport_top) {
                            seen[i] = true;
                        }
                    }
                }
                // Count registration separately from viewport visibility (tall groups scroll).
                if (frames == 1) {
                    require(shell.eval("Object.keys(__cePreview.cards).length") == "72" &&
                                shell.eval("__cePreview.groups.length") == "10",
                            "gallery catalogue dropped cases/groups");
                    for (const auto& c : cases) {
                        require(shell.eval("document.getElementById(" + json_string("case-" + c.id) +
                                           ").textContent.includes(" + json_string(c.label) + ")") == "true",
                                "gallery changed original label");
                    }
                    // Independent expected card geometry from fixed TestGallery:
                    // four/three/two/one columns, original heights and gaps.
                    const unsigned columns[]{4, 4, 1, 1, 1, 3, 3, 2, 4, 4};
                    const unsigned heights[]{288, 384, 1500, 800, 600, 320, 288, 288, 288, 288};
                    const unsigned gaps[]{12, 16, 16, 16, 16, 16, 16, 16, 12, 12};
                    double offset{};
                    double scroll{};
                    for (const auto& e : current.elements) {
                        if (e.identity == scroll_id) {
                            scroll = e.scroll_top;
                        }
                    }
                    for (unsigned group = 0; group < weft::probe_constants::gallery::catalog_group_count; ++group) {
                        unsigned count{};
                        const double w = (1600. - gaps[group] * (columns[group] - 1)) / columns[group];
                        for (const auto& c : cases) {
                            if (c.group == groups[group]) {
                                const auto identity = shell.node_identity("#case-" + c.id);
                                bool found{};
                                for (const auto& e : current.elements) {
                                    if (e.identity == identity) {
                                        const std::array<double, 4> expected{
                                            292 + (c.index % columns[group]) * (w + gaps[group]),
                                            104 + offset + 44 +
                                                (c.index / columns[group]) * (heights[group] + 40 + gaps[group]) -
                                                scroll,
                                            w, double(heights[group] + 40)};
                                        for (unsigned k = 0; k < 4; ++k) {
                                            require(std::abs(e.rect[k] - expected[k]) <= .02,
                                                    "gallery card geometry/column order changed");
                                            ++geometry_checks;
                                        }
                                        found = true;
                                        break;
                                    }
                                }
                                require(found, "gallery registered card missing geometry");
                                ++count;
                            }
                        }
                        offset +=
                            44 + std::ceil(double(count) / columns[group]) * (heights[group] + 40 + gaps[group]) + 40;
                    }
                    require(geometry_checks == 288, "gallery geometry coverage");
                }
                if (frames < weft::probe_constants::gallery::catalog_group_count) {
                    jump(frames);
                } else {
                    jump(4);
                }
            }
            if (frames == 11) { // Real queued mouse input, with scaled canvas coordinates.
                const auto r = slot_rect(0);
                const auto pt = MAKELPARAM(int((r[0] + 260) * weft::probe_constants::gallery::shell_device_scale),
                                           int((r[1] + 180) * weft::probe_constants::gallery::shell_device_scale));
                require(PostMessageW(window.state.hwnd, WM_MOUSEMOVE, 0, pt), "gallery queued particle input");
                shell.eval("__cePreview.scroll(480)");
            }
            if (frames == 12) {
                current = page();
                const auto r = slot_rect(1);
                const auto pt = MAKELPARAM(int((r[0] + 260) * weft::probe_constants::gallery::shell_device_scale),
                                           int((r[1] + 180) * weft::probe_constants::gallery::shell_device_scale));
                require(PostMessageW(window.state.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pt) &&
                            PostMessageW(window.state.hwnd, WM_LBUTTONUP, 0, pt),
                        "gallery queued fireworks click");
            }
            if (frames == 14) {
                pipeline.drain();
                save_png(folder / "canvas-input.png", presenter.pixels(), weft::constants::uhd_width,
                         weft::constants::uhd_height);
                require(canvas_inputs >= 2 && raf_calls[0] > 0 && raf_calls[1] > 0,
                        "gallery live visibility/input missing");
                shell.eval("__cePreview.jump('CSS_TESTS')");
            }
            if (frames == 15) { // Shell click diagnostic and a real HWND wheel queue.
                const auto pt = MAKELPARAM(650, 450);
                require(PostMessageW(window.state.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pt) &&
                            PostMessageW(window.state.hwnd, WM_LBUTTONUP, 0, pt),
                        "gallery diagnostic card click");
                POINT p{700, 700};
                require(ClientToScreen(window.state.hwnd, &p), "gallery wheel coordinates");
                require(PostMessageW(window.state.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, WORD(-WHEEL_DELTA)),
                                     MAKELPARAM(p.x, p.y)),
                        "gallery native wheel");
            }
            if (frames == 16) {
                require(shell.eval("__cePreview.detail.includes('css-00')") == "true" &&
                            std::stod(shell.eval("document.getElementById('scroller').scrollTop")) > 0,
                        "gallery native diagnostic click/wheel failed");
                pipeline.drain();
                require(PostMessageW(window.state.hwnd, WM_SIZE, SIZE_MINIMIZED, 0), "gallery minimize");
                jump(4);
            }
            if (frames == 20) {
                jump(0);
                shell.eval("__cePreview.scroll(400)");
            }
            if (frames == 21) {
                current = page();
                for (unsigned i : {static_cast<unsigned>(weft::probe_constants::gallery::css_hover_scene),
                                   static_cast<unsigned>(weft::probe_constants::gallery::css_toggle_scene)}) {
                    const auto r = scene_rect(i);
                    const auto pt =
                        MAKELPARAM(int((r[0] + r[2] / 2) * weft::probe_constants::gallery::shell_device_scale),
                                   int((r[1] + 60) * weft::probe_constants::gallery::shell_device_scale));
                    if (i == weft::probe_constants::gallery::css_hover_scene) {
                        require(PostMessageW(window.state.hwnd, WM_MOUSEMOVE, 0, pt), "native CSS hover queue");
                    } else {
                        require(PostMessageW(window.state.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pt) &&
                                    PostMessageW(window.state.hwnd, WM_LBUTTONUP, 0, pt),
                                "native CSS width click queue");
                    }
                }
            }
            if (frames == 22) {
                require((owner ? owner_frame->hover_transform
                               : scenes[weft::probe_constants::gallery::css_hover_scene]->vm.eval(
                                     "document.getElementById('root').childNodes[0].style.transform")) == "scale(1.4)",
                        "window CSS hover did not update React");
                require(
                    (owner
                         ? owner_frame->toggle_width
                         : scenes[weft::probe_constants::gallery::css_toggle_scene]->vm.eval(
                               "document.getElementById('root').childNodes[0].childNodes[0].childNodes[0].style.width")) ==
                        "100%",
                    "window CSS click did not update React");
            }
            if (frames == 24) {
                pipeline.drain();
                save_png(folder / "css-hover-input.png", presenter.pixels(), weft::constants::uhd_width,
                         weft::constants::uhd_height);
                shell.eval("document.getElementById('scroller').scrollTop=1000");
            }
            if (frames == 27) {
                pipeline.drain();
                save_png(folder / "css-middle.png", presenter.pixels(), weft::constants::uhd_width,
                         weft::constants::uhd_height);
                shell.eval("document.getElementById('scroller').scrollTop=1800");
            }
            if (frames == 31) {
                jump(8);
                shell.eval("__cePreview.scroll(600)");
            }
            if (frames == 32) {
                current = page();
                const auto r = scene_rect(weft::probe_constants::gallery::anime_drag_scene);
                const auto pt = MAKELPARAM(int((r[0] + 44) * weft::probe_constants::gallery::shell_device_scale),
                                           int((r[1] + 84) * weft::probe_constants::gallery::shell_device_scale));
                require(PostMessageW(window.state.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pt), "new scene queued drag down");
            }
            if (frames == 33) {
                current = page();
                const auto r = scene_rect(weft::probe_constants::gallery::anime_drag_scene);
                const auto pt = MAKELPARAM(int((r[0] + 124) * weft::probe_constants::gallery::shell_device_scale),
                                           int((r[1] + 84) * weft::probe_constants::gallery::shell_device_scale));
                require(PostMessageW(window.state.hwnd, WM_MOUSEMOVE, MK_LBUTTON, pt), "new scene queued drag move");
            }
            if (frames == 34) {
                const auto r = scene_rect(weft::probe_constants::gallery::anime_drag_scene);
                const auto pt = MAKELPARAM(int((r[0] + 500) * weft::probe_constants::gallery::shell_device_scale),
                                           int((r[1] + 84) * weft::probe_constants::gallery::shell_device_scale));
                require(PostMessageW(window.state.hwnd, WM_LBUTTONUP, 0, pt),
                        "new scene queued drag release outside card");
            }
            if (frames == 35) {
                require(!(owner ? owner_frame->anime_drag_transform
                                : scenes[weft::probe_constants::gallery::anime_drag_scene]->vm.eval(
                                      "document.querySelector('.drag-item').style.transform"))
                             .empty(),
                        "queued HWND/owner Anime drag did not change native style");
                jump(0);
            }
            if (frames == 30) {
                pipeline.drain();
                save_png(folder / "css-end.png", presenter.pixels(), weft::constants::uhd_width,
                         weft::constants::uhd_height);
                jump(1);
            }
            if (frames == 33) {
                pipeline.drain();
                save_png(folder / "d3-live.png", presenter.pixels(), weft::constants::uhd_width,
                         weft::constants::uhd_height);
                jump(0);
                shell.eval("document.getElementById('scroller').scrollTop=1000");
            }
            if (frames == 160) {
                pipeline.drain();
                require(fps.ready && fps.fps > 0 && pause_count > 0 && static_rebuilds < 30,
                        "gallery FPS/resume/static cache absent");
                save_png(folder / "window-4k.png", presenter.pixels(), weft::constants::uhd_width,
                         weft::constants::uhd_height);
                window.state.close_requested = true;
            }
        }
        if (self_test && !scroll_test) {
            if (frames == 95) {
                for (const auto& e : current.elements) {
                    if (e.identity == profiler_id) {
                        const auto pt =
                            MAKELPARAM(int((e.rect[0] + 30) * weft::probe_constants::gallery::shell_device_scale),
                                       int((e.rect[1] + 20) * weft::probe_constants::gallery::shell_device_scale));
                        require(PostMessageW(window.state.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pt) &&
                                    PostMessageW(window.state.hwnd, WM_LBUTTONUP, 0, pt),
                                "profiler real checkbox click");
                    }
                }
            }
            if (frames == 105 || frames == 110) {
                if (frames == 110) {
                    layouts_before_profile_click = shell_layout_builds;
                    captures_before_profile_click = window.state.capture_losses;
                }
                const auto r = scene_rect(frames == 105 ? weft::probe_constants::gallery::css_hover_scene
                                                        : weft::probe_constants::gallery::css_toggle_scene);
                const auto pt = MAKELPARAM(int((r[0] + r[2] / 2) * weft::probe_constants::gallery::shell_device_scale),
                                           int((r[1] + 60) * weft::probe_constants::gallery::shell_device_scale));
                if (frames == 105) {
                    require(PostMessageW(window.state.hwnd, WM_MOUSEMOVE, 0, pt), "profiled shared hover input");
                } else {
                    require(PostMessageW(window.state.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pt) &&
                                PostMessageW(window.state.hwnd, WM_LBUTTONUP, 0, pt),
                            "profiled shared click input");
                }
            }
            if (frames == 111) {
                require(shell_layout_builds == layouts_before_profile_click &&
                            window.state.capture_losses == captures_before_profile_click,
                        "normal click triggered full inspection layout or false capture cancel");
            }
            if (frames == 139) {
                require(live_profiler.enabled && live_profiler.owner_samples > 0 && live_profiler.render_samples > 0 &&
                            live_profiler.hud_updates > 1 && live_profiler.stats("Owner")[0] > 0 &&
                            live_profiler.stats("Input")[0] > 0,
                        "profiler did not enable owner curves/input timers");
                hud();
                pipeline.drain();
                save_png(folder / "profiler-detail-4k.png", presenter.pixels(), weft::constants::uhd_width,
                         weft::constants::uhd_height);
            }
            if (frames == 143) {
                require(PostMessageW(window.state.hwnd, WM_KEYDOWN, VK_F8, 0), "profiler keyboard toggle");
            }
            if (frames == 145) {
                require(!live_profiler.enabled && !profile_control->enabled.load() && !owner_frame->detail.enabled,
                        "profiler timers did not switch off");
            }
        }
        if (scroll_test) {
            auto wheel = [&](short delta) {
                POINT p{700, 700};
                require(ClientToScreen(window.state.hwnd, &p), "scroll regression wheel coordinates");
                require(
                    PostMessageW(window.state.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, WORD(delta)), MAKELPARAM(p.x, p.y)),
                    "scroll regression queued wheel");
            };
            if (frames <= 50) {
                wheel(-WHEEL_DELTA);
            }
            if (frames == 56) {
                require(std::abs(std::stod(shell.eval("document.getElementById('scroller').scrollTop")) - 3600) <=
                                .01 &&
                            scene_visible[weft::probe_constants::gallery::sunburst_scene],
                        "continuous wheel did not reach Sunburst");
                pipeline.drain();
                save_png(folder / "scroll-sunburst-4k.png", presenter.pixels(), weft::constants::uhd_width,
                         weft::constants::uhd_height);
                const auto actual = scene_gpu.read(weft::probe_constants::gallery::sunburst_scene);
                auto expected = ::scene_reference(std::filesystem::path(WEFT_SOURCE_DIR) /
                                                  "tests/fixtures/gallery-cluster-reference/d3_interactive-00-24.png");
                const auto crop = scene_gpu.image(weft::probe_constants::gallery::sunburst_scene)->clip.value();
                for (unsigned y = 0; y < expected.height; ++y) {
                    for (unsigned x = 0; x < expected.width; ++x) {
                        if (x < crop[0] * weft::probe_constants::gallery::scene_raster_scale ||
                            x >= crop[2] * weft::probe_constants::gallery::scene_raster_scale ||
                            y < crop[1] * weft::probe_constants::gallery::scene_raster_scale ||
                            y >= crop[3] * weft::probe_constants::gallery::scene_raster_scale) {
                            std::fill_n(expected.rgba.data() + (std::size_t(y) * expected.width + x) * 4, 4,
                                        std::uint8_t{});
                        }
                    }
                }
                require(cluster_image_match(actual, expected), "continuous wheel Sunburst browser pixels");
                ++captures;
            }
            if (frames >= 57 && frames <= 106) {
                wheel(WHEEL_DELTA);
            }
            if (frames == 112) {
                require(std::abs(std::stod(shell.eval("document.getElementById('scroller').scrollTop"))) <= .01 &&
                            scene_visible[0] && scene_gpu.hidden_retirements > 0,
                        "reverse wheel did not rebuild CSS source");
                pipeline.drain();
                save_png(folder / "scroll-return-css-4k.png", presenter.pixels(), weft::constants::uhd_width,
                         weft::constants::uhd_height);
                require(scene_difference(scene_gpu.read(0),
                                         ::scene_reference(std::filesystem::path(WEFT_SOURCE_DIR) /
                                                           "tests/fixtures/scene-gallery-reference/css-00-24.png"))
                            .passes(),
                        "reverse wheel CSS browser pixels");
                ++captures;
            }
            if (frames == 120) {
                require(scene_gpu.peak_imported_bytes <= weft::probe_constants::gallery::external_import_budget_bytes,
                        "continuous wheel exceeded original import budget");
                window.state.close_requested = true;
            }
        }
        if (input_benchmark) {
            auto send = [&](unsigned scene, double x, double y, UINT message, WPARAM buttons) {
                const auto r = scene_rect(scene);
                require(PostMessageW(window.state.hwnd, message, buttons,
                                     MAKELPARAM(int((r[0] + x) * weft::probe_constants::gallery::shell_device_scale),
                                                int((r[1] + y) * weft::probe_constants::gallery::shell_device_scale))),
                        "profile queued native mouse");
            };
            if ((scenario == "kline" || scenario == "scatter") && frames >= 24 && frames < 144) {
                const unsigned index = scenario == "kline" ? weft::probe_constants::gallery::kline_scene
                                                           : weft::probe_constants::gallery::scatter_scene;
                if (scenario == "scatter" && frames == 24) {
                    send(index, 320, 360, WM_LBUTTONDOWN, MK_LBUTTON);
                }
                for (unsigned sample = 0; sample < 4; ++sample) {
                    send(index,
                         scenario == "kline" ? 180 + (frames - 24) * 8 + sample * .5
                                             : 320 + (frames - 24) * .8 + sample * .2,
                         scenario == "kline" ? 250 : 360, WM_MOUSEMOVE, scenario == "scatter" ? MK_LBUTTON : 0);
                }
            }
            if (scenario == "scatter" && frames == 144) {
                send(weft::probe_constants::gallery::scatter_scene, 416, 360, WM_LBUTTONUP, 0);
            }
            if (scenario == "framer" && (frames == 30 || frames == 150)) {
                send(40, 259, 160, WM_LBUTTONDOWN, MK_LBUTTON);
                send(40, 259, 160, WM_LBUTTONUP, 0);
            }
            if (scenario == "anime" && (frames == 30 || frames == 150)) {
                send(55, 200, 190, WM_LBUTTONDOWN, MK_LBUTTON);
                send(55, 200, 190, WM_LBUTTONUP, 0);
            }
        }
        std::ostringstream title;
        title << "CaseElephant / 72 cases, 66 native / LIVE " << live_count << " / 3840x2160 / FPS " << std::fixed
              << std::setprecision(1) << fps.fps;
        const auto value = title.str();
        SetWindowTextW(window.state.hwnd, std::wstring(value.begin(), value.end()).c_str());
        require(shell.stats().listener_exceptions == 0 && !window.state.overflow, "gallery shell input failure");
    }
    std::size_t snapshots{};
    std::size_t snapshot_reuses{};
    std::size_t evals{};
    std::size_t host_calls{};
    std::size_t mutations{};
    std::size_t snapshot_nodes{};
    std::size_t typed_samples{};
    std::size_t paint_compilations{};
    for (const auto& s : scenes) {
        snapshots += s->snapshots;
        snapshot_reuses += s->snapshot_reuses;
        const auto js = s->vm.stats();
        evals += js.script_evaluations;
        host_calls += js.host_calls;
        mutations += js.dom_mutations;
        snapshot_nodes += js.snapshot_nodes;
        const auto work = s->scene.work();
        typed_samples += work.typed_samples;
        paint_compilations += work.paint_compilations;
    }
    if (owner_frame) {
        snapshots = owner_frame->snapshots;
        snapshot_reuses = owner_frame->snapshot_reuses;
        evals = owner_frame->evals;
        host_calls = owner_frame->host_calls;
        mutations = owner_frame->mutations;
        snapshot_nodes = owner_frame->snapshot_nodes;
        typed_samples = owner_frame->typed_samples;
        paint_compilations = owner_frame->paint_compilations;
    }
    const auto& native = scene_gpu.native_work();
    const auto raster_stats = scene_gpu.raster_work();
    std::string reasons = "{";
    for (const auto& [name, count] : native.reasons) {
        if (reasons.size() > 1) {
            reasons += ",";
        }
        reasons += json_string(name) + ":" + std::to_string(count);
    }
    reasons += "}";
    write_text(folder / "render-work.json",
               "{\"native_passes\":" + std::to_string(native.passes) + ",\"native_fallbacks\":" +
                   std::to_string(native.fallbacks) + ",\"native_draw_batches\":" + std::to_string(native.draws) +
                   ",\"text_builds\":" + std::to_string(native.text_builds) +
                   ",\"text_reuses\":" + std::to_string(native.text_reuses) +
                   ",\"native_layer_builds\":" + std::to_string(native.layer_builds) +
                   ",\"native_layer_reuses\":" + std::to_string(native.layer_reuses) +
                   ",\"layer_builds\":" + std::to_string(raster_stats.layer_builds) +
                   ",\"layer_reuses\":" + std::to_string(raster_stats.layer_reuses) +
                   ",\"layer_evictions\":" + std::to_string(raster_stats.layer_evictions) +
                   ",\"partial_paints\":" + std::to_string(scene_gpu.partial_paints) +
                   ",\"full_paints\":" + std::to_string(scene_gpu.full_paints) +
                   ",\"damaged_pixel_ratio\":" + format_host_time(scene_gpu.damaged_pixels / scene_gpu.full_pixels) +
                   ",\"fallback_reasons\":" + reasons + "}");
    const auto commands = scene_gpu.raster_work();
    write_text(
        folder / "owner-work.json",
        "{\"snapshots\":" + std::to_string(snapshots) + ",\"snapshot_reuses\":" + std::to_string(snapshot_reuses) +
            ",\"snapshot_nodes\":" + std::to_string(snapshot_nodes) +
            ",\"script_evaluations\":" + std::to_string(evals) + ",\"host_calls\":" + std::to_string(host_calls) +
            ",\"dom_mutations\":" + std::to_string(mutations) + ",\"typed_samples\":" + std::to_string(typed_samples) +
            ",\"paint_compilations\":" + std::to_string(paint_compilations) +
            ",\"paint_program_builds\":" + std::to_string(commands.paint_program_builds) +
            ",\"paint_program_reuses\":" + std::to_string(commands.paint_program_reuses) +
            ",\"paint_program_evictions\":" + std::to_string(commands.paint_program_evictions) +
            ",\"typed_draws\":" + std::to_string(commands.typed_draws) + "}");
    if (!benchmark.empty()) {
        std::string data = "{";
        for (const auto& [name, values] : stages) {
            if (data.size() > 1) {
                data += ",";
            }
            data += json_string(name) + ":" + GalleryPreviewSamples::json(values);
        }
        data += "}";
        write_text(
            folder / "benchmark.json",
            "{\"scenario\":" + json_string(benchmark) + ",\"input_benchmark\":" + (input_benchmark ? "true" : "false") +
                ",\"frames\":240,\"warm_frames\":224,\"fixed_clock_step_ms\":16.666666666666668,\"capture_readback\":false,\"warm_throughput_fps\":" +
                format_host_time(weft::probe_constants::gallery::fixed_clock_warm_frames /
                                 (benchmark_end - benchmark_start)) +
                ",\"stages_ms\":" + data + "}");
    }
    const auto shell_nodes = shell.stats().live_nodes;
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                                 sizeof(memory)) != 0,
            "gallery process memory stats");
    write_text(folder / "live-profiler.json",
               "{\"enabled_at_close\":" + (live_profiler.enabled ? std::string("true") : std::string("false")) +
                   ",\"owner_samples\":" + std::to_string(live_profiler.owner_samples) +
                   ",\"render_samples\":" + std::to_string(live_profiler.render_samples) +
                   ",\"hud_updates\":" + std::to_string(live_profiler.hud_updates) +
                   ",\"sample_limit_per_series\":600,\"hud_max_hz\":5,\"frame_limit_added\":false}");
    pipeline.drain();
    scene_gpu.release();
    if (owner) {
        owner->close();
        auto w = owner->work();
        write_text(folder / "thread-work.json",
                   "{\"profile_run_id\":" + std::to_string(profile_control->run_id) +
                       ",\"owner_updates\":" + std::to_string(owner_updates) + ",\"reused_owner_frames\":" +
                       std::to_string(reused_owner_frames) + ",\"requests\":" + std::to_string(w.requests) +
                       ",\"coalesced_requests\":" + std::to_string(w.coalesced_requests) + ",\"published\":" +
                       std::to_string(w.published) + ",\"discarded_frames\":" + std::to_string(w.discarded_frames) +
                       ",\"accepted_inputs\":" + std::to_string(w.accepted_inputs) +
                       ",\"processed_inputs\":" + std::to_string(w.processed_inputs) +
                       ",\"coalesced_inputs\":" + std::to_string(w.coalesced_inputs) +
                       ",\"peak_pending_inputs\":" + std::to_string(w.peak_pending_inputs) +
                       ",\"owner_active_ms\":" + GalleryPreviewSamples::json(owner_ms) +
                       ",\"frame_age_ms\":" + GalleryPreviewSamples::json(owner_age) +
                       ",\"queue_ms\":" + GalleryPreviewSamples::json(owner_queue) + "}");
    }
    for (auto& s : scenes) {
        s->unmount();
        require(s->vm.stats().live_nodes == weft::probe_constants::gallery::unmounted_live_nodes,
                "gallery scene teardown");
    }
    for (auto& s : sessions) {
        if (s) {
            s->vm.eval("__ceGallery.unmount()");
            s->vm.reset_document();
            s->vm.collect_garbage();
            require(s->vm.stats().canvas_contexts == 0, "gallery Canvas teardown");
        }
    }
    shell.reset_document();
    shell.collect_garbage();
    gpu.evict_assets(0, true);
    gpu.check_debug_messages();
    require(gpu.native_creations == gpu.native_releases && pipeline.submissions == pipeline.completions,
            "gallery completion retirement leak");
    for (const bool value : seen) {
        seen_cases += value;
    }
    write_text(
        folder / "preview-session.json",
        "{\"format\":\"gallery-preview-session-v1\",\"case_count\":72,\"native_pixel_cases\":66,\"original_full_gallery_rendered\":false,\"surface\":[3840,2160],\"logical_shell\":[1920,1080],\"canvas_backing\":[800,600],\"adapter\":" +
            json_string(gpu.adapter) + ",\"frames\":" + std::to_string(frames) +
            ",\"successful_visible_present_fps\":" + (fps.ready ? format_host_time(fps.fps) : "null") +
            ",\"last_visible_native_case_count\":" + std::to_string(live_count) +
            ",\"warm_window_frames\":600,\"warmup_frames\":16,\"reset_on_scroll_resize_visibility\":true,\"cpu_tick_ms\":" +
            GalleryPreviewSamples::json(samples.cpu) +
            ",\"gpu_producer_and_ui_elapsed_ms\":" + GalleryPreviewSamples::json(samples.gpu) +
            ",\"present_interval_ms\":" + GalleryPreviewSamples::json(samples.interval) +
            ",\"startup_including_72_observations_ms\":" + format_host_time(startup_ms) +
            ",\"gpu_cached_peak_bytes\":" + std::to_string(gpu.peak_cached_bytes) +
            ",\"gpu_cache_budget_bytes\":536870912,\"retired_textures\":" + std::to_string(gpu.native_releases) +
            ",\"actual_completed_submissions\":" + std::to_string(pipeline.completions) +
            ",\"peak_in_flight\":" + std::to_string(pipeline.peak_in_flight) + ",\"native_canvas_input_events\":" +
            std::to_string(canvas_inputs) + ",\"captures\":" + std::to_string(captures) +
            ",\"visible_cases_in_group_captures\":" + std::to_string(seen_cases) + ",\"cpu_pixel_samples\":" +
            std::to_string(oracle_samples) + ",\"cpu_pixel_max_difference\":" + std::to_string(pixel_error) +
            ",\"static_packet_rebuilds\":" + std::to_string(static_rebuilds) + ",\"cached_full_packet_checks\":" +
            std::to_string(cache_differences) + ",\"card_geometry_checks\":" + std::to_string(geometry_checks) +
            ",\"minimized_opportunities\":" + std::to_string(pause_count) +
            ",\"shell_native_nodes\":" + std::to_string(shell_nodes) +
            ",\"process_peak_working_set_bytes\":" + std::to_string(memory.PeakWorkingSetSize) +
            ",\"process_private_bytes_at_close\":" + std::to_string(memory.PrivateUsage) + ",\"self_test\":" +
            (self_test ? "true" : "false") + ",\"capture_copy_during_run\":" + (self_test ? "true" : "false") +
            ",\"scene_gpu_paints\":" + std::to_string(scene_gpu.paints) + ",\"scene_gpu_pool_slots\":" +
            std::to_string(scene_gpu.allocated) + ",\"scene_gpu_pool_reuses\":" + std::to_string(scene_gpu.reuses) +
            ",\"scene_gpu_imports_retired\":" + std::to_string(scene_gpu.retired) +
            ",\"scene_gpu_hidden_retirements\":" + std::to_string(scene_gpu.hidden_retirements) +
            ",\"scene_gpu_peak_live_slots\":" + std::to_string(scene_gpu.peak_live_slots) +
            ",\"scene_gpu_peak_imported_bytes\":" + std::to_string(scene_gpu.peak_imported_bytes) +
            ",\"scroll_regression\":" + (scroll_test ? "true" : "false") +
            ",\"scene_raster\":\"native D3D11 shapes/cached sources + D2D GPU fallback / linear RGBA16F external leases\",\"canvas_raster\":\"Direct2D SOFTWARE WIC\",\"ultralight_comparable\":false,\"mobile_verified\":false,\"debug_warning_error_count\":" +
            (gpu.debug_layer ? "0" : "null") + "}");
    std::cout
        << "PASS gallery preview: 72 catalogue cards, 64 original scene cases + 2 original Canvas, 4K, exact host clock, bounded two-frame completion\n";
    return 0;
}
