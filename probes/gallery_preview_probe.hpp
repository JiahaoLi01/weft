#pragma once

#include "probe_common.hpp"
#include "gallery_scene_manifest.hpp"
#include "gallery_scene_options.hpp"
#include "cheetah_probe.hpp"
#include "gallery_canvas_probe.hpp"
#include "gallery_live_profiler.hpp"
#include "gallery_constants.hpp"
#include "probe_modes.hpp"

namespace probes {
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
        const auto footer = cheetah_packet(page, fonts, {});
        auto list = std::make_shared<weft::paint::DisplayList>(*packet.list);
        require(bool(weft::paint::append(*list, *footer.list)), "gallery detail footer");
        packet.list = std::move(list);
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

    inline std::string gallery_preview_gap(const std::string& group) {
        if (group == "CSS_TESTS" || group == "D3_TESTS") {
            return "Native original CSS/SVG scene; compiled fixed Tailwind adapter; 121-frame raster/input references";
        }
        if (gallery_cluster_group(group)) {
            return "Native original scene; shared SVG/input/layout/animation profile; browser captures and input trace";
        }
        if (group == "TOOLTIP_TESTS") {
            return "Native CRPG nested tooltip page; smoke as a UI shader effect; Chrome frame-by-frame reference";
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

    inline std::string gallery_preview_json(const std::vector<GalleryPreviewCase>& cases) {
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
                               : c.group == "CSS_TESTS" || c.group == "D3_TESTS" || gallery_cluster_group(c.group) ||
                                       c.group == "TOOLTIP_TESTS"
                                   ? "native_scene_profile"
                                   : "not_passed")
                << '}';
        }
        return out.str() + ']';
    }

    // The catalogue's cases the window leaves out: its Three.js cases (WebGL is not a product target), whose place the
    // nested tooltip page takes.
    inline constexpr std::array<std::string_view, weft::probe_constants::gallery::retired_case_count>
        gallery_retired_cases{"webgl-02", "webgl-03"};

    // The window's catalogue: the original cases in their order, less the retired ones, then the nested tooltip page.
    inline std::vector<GalleryPreviewCase> gallery_preview_observe() {
        weft::ScriptDom registry({weft::probe_constants::gallery::vm_stack_bytes,
                                  weft::probe_constants::gallery::host_vm_execution_budget_ms});
        const auto directory = std::filesystem::path(WEFT_SOURCE_DIR) / "examples/crpg-gallery/native";
        registry.eval(read_file(directory / "case-list.js"));
        require(registry.eval("__ceGalleryCases.length") == "72", "gallery inventory changed; review preview scope");
        std::map<std::string, std::string> scripts;
        std::vector<GalleryPreviewCase> cases;
        for (unsigned i = 0; i < weft::probe_constants::gallery::catalog_case_count; ++i) {
            const auto lookup = "__ceGalleryCases[" + std::to_string(i) + "]";
            GalleryPreviewCase c;
            c.id = registry.eval(lookup + ".id");
            if (std::find(gallery_retired_cases.begin(), gallery_retired_cases.end(), c.id) !=
                gallery_retired_cases.end()) {
                continue;
            }
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
                const auto it = std::find_if(gallery_scene_descriptors.begin(), gallery_scene_descriptors.end(),
                                             [&](const auto& d) {
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
                            for (unsigned turn = 0; turn < weft::probe_constants::gallery::task_pump_turn_limit;
                                 ++turn) {
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
        GalleryPreviewCase page{"tooltip-00", "TOOLTIP_TESTS", "CRPG: Nested Tooltip + Smoke"};
        page.reason = gallery_preview_gap(page.group);
#ifdef WEFT_REACT_GC_STRESS
        page.execution = "not_run_gc_instrumentation";
#else
        {
            auto session = gallery_scene_session(weft::probe_constants::gallery::tooltip_page_scene);
            for (unsigned f = 0; f < weft::probe_constants::gallery::observation_frames; ++f) {
                session->step(f * weft::probe_constants::gallery::observation_step_ms +
                              weft::probe_constants::gallery::host_clock_offset_ms);
            }
            require(session->vm.eval("__ceGallery.state().active.label") == page.label, "nested tooltip page label");
            page.execution = "no_error_observed";
            session->unmount();
        }
#endif
        cases.push_back(std::move(page));
        require(cases.size() == std::size_t(weft::probe_constants::gallery::window_case_count),
                "gallery window catalogue");
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
                   ",\"p95\":" + format_host_time(percentile(v, .95)) +
                   ",\"p99\":" + format_host_time(percentile(v, .99)) +
                   ",\"max\":" + format_host_time(percentile(v, 1)) + "}";
        }
    };

    inline FramePacket gallery_preview_hud(weft::FontSet& fonts, const WindowFrameRate& rate, unsigned live,
                                           const GalleryPreviewSamples& samples, bool paused,
                                           std::string owner_label = {}, bool detailed = true, bool vsync = true) {
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
        text(fps.str() + "  |  LIVE " + std::to_string(live) + " / " + std::to_string(gallery_native_case_count), 58,
             32);
        if (!detailed) {
            text(std::string(vsync ? "VSync on" : "VSync off") + " (F7) | F8 / sidebar: detailed profiler", 95, 22);
            return cheetah_packet(page, fonts, {});
        }
        std::ostringstream cpu;
        cpu << "CPU p50 / p95 / p99: " << std::fixed << std::setprecision(2)
            << GalleryPreviewSamples::percentile(samples.cpu, .5) << " / "
            << GalleryPreviewSamples::percentile(samples.cpu, .95) << " / "
            << GalleryPreviewSamples::percentile(samples.cpu, .99) << " ms";
        text(cpu.str(), 99, 24);
        text("3840 x 2160 | D3D11 | CSS / SVG GPU at 2x", 137, 24);
        text(owner_label.empty()
                 ? std::to_string(gallery_native_case_count) + " native cases / " +
                       std::to_string(weft::probe_constants::gallery::window_case_count - gallery_native_case_count) +
                       " diagnostic cases; GPU vector raster"
                 : owner_label,
             177, 21);
        return cheetah_packet(page, fonts, {});
    }

    inline FramePacket gallery_preview_packet(weft::PositionedPage page, weft::FontSet& fonts,
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

    inline void gallery_preview_packet_equal(const FramePacket& cached, const FramePacket& full) {
        const auto& a = *cached.list;
        const auto& b = *full.list;
        require(a.items.size() == b.items.size() && a.external_textures == b.external_textures,
                "gallery cached/full item count");
        for (std::size_t i = 0; i < b.items.size(); ++i) {
            const auto& x = a.items[i];
            const auto& y = b.items[i];
            require(x.kind == y.kind && x.transform == y.transform && x.clip == y.clip && x.width == y.width &&
                        x.height == y.height && x.opacity == y.opacity && x.clips_count == y.clips_count,
                    "gallery cached/full paint/clip order");
            for (std::uint32_t c = 0; c < x.clips_count; ++c) {
                const auto& p = a.clips[x.clips_begin + c];
                const auto& q = b.clips[y.clips_begin + c];
                require(p.rect == q.rect && p.radius_x == q.radius_x && p.radius_y == q.radius_y,
                        "gallery cached/full rounded clips");
            }
            if (x.kind == weft::paint::Item::Kind::Box) {
                require(a.boxes[x.payload].background == b.boxes[y.payload].background, "gallery cached/full box");
            } else if (x.kind == weft::paint::Item::Kind::Image) {
                const auto& p = a.images[x.payload];
                const auto& q = b.images[y.payload];
                require(p.uv == q.uv && p.color == q.color && p.stroke == q.stroke && p.outline == q.outline &&
                            (p.asset < a.assets.size()) == (q.asset < b.assets.size()),
                        "gallery cached/full image");
                if (p.asset < a.assets.size()) {
                    const auto& s = *a.assets[p.asset];
                    const auto& t = *b.assets[q.asset];
                    require(s.width == t.width && s.height == t.height && s.encoding == t.encoding &&
                                s.bytes == t.bytes,
                            "gallery cached/full asset content");
                }
            }
        }
    }
} // namespace probes
