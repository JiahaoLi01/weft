#include "probe_common.hpp"
#include "gallery_scene_manifest.hpp"
#include "gallery_layer_manifest.hpp"
#include "scene_gpu_surface_probe.hpp"
#include "gallery_owner_probe.hpp"
#include "gallery_preview_probe.hpp"
#include "gallery_constants.hpp"
#include "probe_modes.hpp"

namespace probes {
    int run_render_stages(bool warp, const std::filesystem::path& folder) {
        std::filesystem::create_directories(folder);
        Experiment gpu(warp, weft::probe_constants::gallery::render_stages_surface_width,
                       weft::probe_constants::gallery::render_stages_surface_height);
        gpu.cache_assets = true;
        TwoFramePipeline pipeline(gpu);
        SceneGpuSurfaces surfaces(gpu, pipeline);
        surfaces.retain_paint = true;
        weft::SceneRaster reference{weft::testing::thread_scene_text()};
        weft::SceneGeometry geometry{weft::testing::thread_scene_text()};
        unsigned comparisons{};
        double worst_mean{};
        double minimum_iou = 1;
        for (unsigned f = 0; f < weft::probe_constants::gallery::render_stages_frames; ++f) {
            surfaces.native_paint = f >= weft::probe_constants::gallery::render_stages_native_frame;
            weft::SceneFrame scene;
            auto& root = scene.root;
            root.identity = {1, 1, 1};
            root.tag = "div";
            root.space = weft::constants::xhtml_namespace_uri;
            root.box = {0, 0, weft::probe_constants::gallery::render_stages_scene_width,
                        weft::probe_constants::gallery::render_stages_scene_height};
            root.style = {{"background-color", "#203044"}, {"overflow", "hidden"}};
            std::vector<weft::NodeIdentity> ids;
            for (unsigned i = 0; i < weft::probe_constants::gallery::render_stages_node_count; ++i) {
                auto n = root;
                n.children.clear();
                n.identity = {i + 2, 1 + f / 40, 1};
                n.box = {float(10 + i % 8 * 38), float(14 + i / 8 * 74), 28, 32};
                n.style = {{"background-color", i % 2 ? "rgba(212,168,67,.65)" : "#2a4a8a"},
                           {"transform", "translate(" + std::to_string(f % 20 * .31f) + "px, " +
                                             std::to_string(f % 8 * .26f) + "px) rotate(" +
                                             std::to_string(int(f % 23) - 11) + "deg)"},
                           {"opacity", f % 4 ? ".7" : "1"},
                           {"border-top-left-radius", "7px"},
                           {"border-bottom-right-radius", "12px"}};
                if (f >= weft::probe_constants::gallery::render_stages_native_frame && i == 0) {
                    n.identity.generation = f + 1;
                }
                if (i == 0) {
                    n.box[2] = 80;
                    n.box[3] = 48;
                    if (f < weft::probe_constants::gallery::render_stages_native_frame) {
                        n.style["overflow"] = "hidden";
                    }
                    auto c = n;
                    c.identity = {40, 1 + f / 40, 1};
                    c.box = {5, 6, 34, 32};
                    c.style = {{"background-color", "rgba(255,0,80,.5)"}};
                    n.children.push_back(c);
                    c.identity.slot = 41;
                    c.box[0] = 24;
                    c.style["background-color"] = "rgba(0,200,255,.5)";
                    n.children.push_back(c);
                }
                if (f >= weft::probe_constants::gallery::render_stages_native_frame && i == 5) {
                    n.style["background-image"] =
                        (f % 3 == 0   ? "linear-gradient(45deg, rgba(255,0,0,.5), #2a4a8a)"
                         : f % 3 == 1 ? "radial-gradient(circle, #d4a843, transparent)"
                                      : "conic-gradient(from 30deg, #d4a843, #8b1a1a, #d4a843)");
                }
                if (f >= weft::probe_constants::gallery::render_stages_native_frame && i == 6) {
                    for (const auto* edge : {"top", "right", "bottom", "left"}) {
                        n.style[std::string("border-") + edge + "-width"] = "3px";
                        n.style[std::string("border-") + edge + "-color"] =
                            std::string(edge) == "top" ? "#ffffff" : "rgba(212,168,67,.5)";
                    }
                }
                if (f >= 80 && i == 2) {
                    n.style["background-color"] = (f % 3 ? "#ffffff" : "#8b1a1a");
                }
                if (f >= weft::probe_constants::gallery::render_stages_native_frame && i == 7) {
                    n.style["opacity"] = "50%";
                    n.paint = weft::compile_scene_paint_values(n.style);
                }
                if (f % 17 == 0 && i == 3) {
                    n.box[2] = 34;
                }
                if (f % 29 == 0 && i == 4) {
                    n.style["z-index"] = "3";
                }
                if (f >= 120 && i == 1) {
                    n.text = "UI";
                    n.style["color"] = f % 2 ? "#ffffff" : "#ff0000";
                    n.style["font-size"] = "12px";
                }
                ids.push_back(n.identity);
                root.children.push_back(n);
            }
            if (f % 31 == 0) {
                std::reverse(root.children.begin(), root.children.end());
            }
            if (f % 37 == 0) {
                root.children.pop_back();
            }
            scene.elements = geometry.geometry(scene);
            auto value = std::make_shared<const weft::SceneFrame>(scene);
            const auto crop = ((f >= 100 && f < 110) || (f >= 260 && f < 270))
                                  ? std::optional<std::array<float, 4>>{{0, 12, 320, 164}}
                                  : std::nullopt;
            auto& image = surfaces.paint_owned(0, value, weft::probe_constants::gallery::scene_raster_scale, crop,
                                               f >= weft::probe_constants::gallery::render_stages_native_frame
                                                   ? std::vector<weft::NodeIdentity>{root.children.front().identity}
                                                   : ids);
            surfaces.publish();
            FramePacket p;
            p.draws.push_back({{0, 0, weft::probe_constants::gallery::render_stages_surface_width,
                                weft::probe_constants::gallery::render_stages_surface_height},
                               {1, 1, 1, 1},
                               0});
            std::array<bool, weft::probe_constants::gallery::core_scene_count> visible{};
            visible[0] = true;
            auto serial = pipeline.submit(std::move(p),
                                          [] {
                                          },
                                          {}, {image.handle});
            surfaces.submitted(serial, visible);
            if (f % 2 == 1) {
                auto actual = surfaces.read(0);
                auto expected = reference.raster(scene, weft::probe_constants::gallery::scene_raster_scale);
                if (crop) {
                    for (unsigned y = 0; y < expected.height; ++y) {
                        if (y < 24 || y >= 328) {
                            std::fill_n(expected.rgba.data() + std::size_t(y) * expected.width * 4, expected.width * 4,
                                        std::uint8_t{0});
                        }
                    }
                }
                auto d = scene_difference(actual, expected);
                auto mean = *std::max_element(d.mean.begin(), d.mean.end());
                worst_mean = std::max(worst_mean, mean);
                minimum_iou = std::min(minimum_iou, d.soft_iou);
                if (!d.passes()) {
                    for (auto pair : {std::pair{"native", &actual}, std::pair{"reference", &expected}}) {
                        std::vector<Pixel> px(std::size_t(pair.second->width) * pair.second->height);
                        std::memcpy(px.data(), pair.second->rgba.data(), pair.second->rgba.size());
                        save_png(folder / (std::string(pair.first) + "-failure.png"), px, pair.second->width,
                                 pair.second->height);
                    }
                }
                if (!d.passes()) {
                    std::cerr << "retained f=" << f << " mean=" << mean << " iou=" << d.soft_iou << '\n';
                }
                require(d.passes(), "explicit layer/damage differs from independent full raster");
                ++comparisons;
                if (f % 40 == 1) {
                    std::vector<Pixel> pixels(std::size_t(actual.width) * actual.height);
                    std::memcpy(pixels.data(), actual.rgba.data(), actual.rgba.size());
                    save_png(folder / ("retained-" + std::to_string(f) + ".png"), pixels, actual.width, actual.height);
                }
            }
        }
        const auto work = surfaces.raster_work();
        require(surfaces.native_work().passes > 10 && surfaces.native_work().layer_builds > 100 &&
                    surfaces.native_work().image_evictions > 0,
                "native pass unexercised");
        // Layers and color glyphs draw natively: the native path never uses the reference raster.
        require(surfaces.native_work().reference_builds == 0, "native path used the reference raster");
        require(work.layer_reuses > 100 && work.layer_builds > 32 && work.layer_evictions > 0 &&
                    surfaces.partial_paints > 0,
                "retained/damage/capacity paths unexercised");
        surfaces.release();
        gpu.evict_assets(0, true);
        gpu.check_debug_messages();
        require(pipeline.submissions == pipeline.completions && pipeline.peak_in_flight == 2,
                "retained real two-frame retirement");
        std::ostringstream report;
        report << "{\"native_passes\":" << surfaces.native_work().passes
               << ",\"native_fallbacks\":" << surfaces.native_work().fallbacks
               << ",\"native_layer_builds\":" << surfaces.native_work().layer_builds
               << ",\"native_layer_reuses\":" << surfaces.native_work().layer_reuses
               << ",\"native_image_evictions\":" << surfaces.native_work().image_evictions
               << ",\"native_reference_builds\":" << surfaces.native_work().reference_builds
               << ",\"comparisons\":" << comparisons << ",\"worst_channel_mean\":" << worst_mean
               << ",\"minimum_soft_iou\":" << minimum_iou << ",\"layer_builds\":" << work.layer_builds
               << ",\"layer_reuses\":" << work.layer_reuses << ",\"layer_evictions\":" << work.layer_evictions
               << ",\"partial_paints\":" << surfaces.partial_paints << ",\"full_paints\":" << surfaces.full_paints
               << ",\"damaged_pixel_ratio\":" << surfaces.damaged_pixels / surfaces.full_pixels
               << ",\"submissions\":" << pipeline.submissions << ",\"completions\":" << pipeline.completions
               << ",\"debug_warnings\":0}";
        write_text(folder / "report.json", report.str());
        std::cout << "PASS retained layers / local damage / two actual frames\n";
        return 0;
    }

    // Engine-shaped async host: reliable inputs and refresh requests are game
    // calls; the render side consumes complete snapshots without owner waits.
    // One artificial owner stall tests overload, so these are stress timings.
    int run_owner_async(bool warp, const std::filesystem::path& folder) {
        std::filesystem::create_directories(folder);
        auto owner = gallery_owner();
        Experiment gpu(warp, weft::constants::uhd_width, weft::constants::uhd_height);
        gpu.cache_assets = true;
        TwoFramePipeline pipeline(gpu);
        SceneGpuSurfaces surfaces(gpu, pipeline);
        surfaces.retain_paint = surfaces.native_paint = true;
        std::shared_ptr<const GalleryOwnerFrame> frame;
        std::array<std::uint64_t, weft::probe_constants::gallery::core_scene_count> revisions{};
        std::array<bool, gallery_scene_count> visible{};
        for (unsigned i = weft::probe_constants::gallery::owner_async_first_scene;
             i < weft::probe_constants::gallery::owner_async_scene_end; ++i) {
            visible[i] = true;
        }
        std::deque<double> game_calls;
        std::deque<double> render_ms;
        std::deque<double> owner_ms;
        std::deque<double> age_ms;
        unsigned updates{};
        unsigned reuses{};
        const auto started = std::chrono::steady_clock::now();
        auto now = [&] {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        };
        // Startup is explicitly synchronized and kept out of hot game calls.
        GalleryOwnerRequest init;
        init.scenes = visible;
        frame = owner
                    ->wait(weft::probe_constants::gallery::owner_epoch,
                           owner->request(weft::probe_constants::gallery::owner_epoch, init))
                    .value;
        const auto loop_started = std::chrono::steady_clock::now();
        std::exception_ptr game_error;
        const auto render_thread = std::this_thread::get_id();
        std::jthread game([&] {
            try {
                require(std::this_thread::get_id() != render_thread, "game/render thread isolation");
                for (unsigned f = 0; f < weft::probe_constants::gallery::owner_async_frames; ++f) {
                    const auto time = now();
                    const auto game_start = now();
                    if (f % 19 == 0) {
                        require(
                            owner->input(weft::probe_constants::gallery::owner_epoch,
                                         {weft::probe_constants::gallery::css_toggle_scene, -1, weft::MouseAction::Down,
                                          100, 40, time}) == GalleryOwnerExecutor::Accepted &&
                                owner->input(weft::probe_constants::gallery::owner_epoch,
                                             {weft::probe_constants::gallery::css_toggle_scene, -1,
                                              weft::MouseAction::Up, 100, 40, time}) == GalleryOwnerExecutor::Accepted,
                            "async reliable game input");
                    }
                    GalleryOwnerRequest request;
                    request.time = time;
                    request.scenes = visible;
                    if (f == 64) {
                        require(owner->input(weft::probe_constants::gallery::owner_epoch,
                                             {-1, -1, weft::MouseAction::Move, 0, 0, time,
                                              weft::probe_constants::gallery::max_test_delay_ms}) ==
                                    GalleryOwnerExecutor::Accepted,
                                "owner reliable stall injection");
                        for (unsigned i = 0; i < 16; ++i) {
                            owner->request(weft::probe_constants::gallery::owner_epoch, request);
                        }
                    }
                    owner->request(weft::probe_constants::gallery::owner_epoch, request);
                    GalleryPreviewSamples::add(game_calls, now() - game_start);
                    std::this_thread::sleep_until(
                        loop_started +
                        std::chrono::microseconds((f + 1) * weft::probe_constants::gallery::owner_async_period_us));
                }
            } catch (...) {
                game_error = std::current_exception();
            }
        });
        for (unsigned f = 0; f < weft::probe_constants::gallery::owner_async_frames; ++f) {
            const auto deadline = loop_started + std::chrono::microseconds(
                                                     (f + 1) * weft::probe_constants::gallery::owner_async_period_us);
            const auto time = now();
            const auto render_start = now();
            if (auto value = owner->latest()) {
                frame = value->value;
                ++updates;
                GalleryPreviewSamples::add(owner_ms, value->active_ms);
            } else {
                ++reuses;
            }
            GalleryPreviewSamples::add(age_ms, std::max(0., time - frame->time));
            pipeline.poll();
            pipeline.begin_frame_timing();
            std::vector<weft_render_handle> external;
            FramePacket packet;
            for (unsigned i = weft::probe_constants::gallery::owner_async_first_scene;
                 i < weft::probe_constants::gallery::owner_async_scene_end; ++i) {
                const auto& scene = *frame->scenes[i];
                if (frame->revisions[i] != revisions[i]) {
                    surfaces.paint_owned(i, frame->scenes[i], weft::probe_constants::gallery::scene_raster_scale,
                                         std::nullopt, gallery_layer_manifest(i, scene, true));
                    revisions[i] = frame->revisions[i];
                }
            }
            surfaces.publish();
            for (unsigned i = weft::probe_constants::gallery::owner_async_first_scene;
                 i < weft::probe_constants::gallery::owner_async_scene_end; ++i) {
                auto* image = surfaces.image(i);
                require(image && image->handle.renderer_id, "async complete surface missing");
                auto index = int(external.size());
                external.push_back(image->handle);
                packet.draws.push_back({{float((i - weft::probe_constants::gallery::owner_async_first_scene) %
                                               weft::probe_constants::gallery::owner_async_grid_columns *
                                               weft::probe_constants::gallery::owner_async_cell_width),
                                         float((i - weft::probe_constants::gallery::owner_async_first_scene) /
                                               weft::probe_constants::gallery::owner_async_grid_columns *
                                               weft::probe_constants::gallery::owner_async_cell_height),
                                         weft::probe_constants::gallery::owner_async_card_width,
                                         weft::probe_constants::gallery::owner_async_cell_height},
                                        {1, 1, 1, 1},
                                        index});
            }
            auto serial = pipeline.submit(
                std::move(packet),
                [] {
                },
                {}, external);
            surfaces.submitted(serial, visible);
            GalleryPreviewSamples::add(render_ms, now() - render_start);
            std::this_thread::sleep_until(deadline);
        }
        game.join();
        if (game_error) {
            std::rethrow_exception(game_error);
        }
        owner->close();
        auto work = owner->work();
        pipeline.drain();
        surfaces.release();
        gpu.evict_assets(0, true);
        gpu.check_debug_messages();
        const unsigned minimum_updates =
#ifdef WEFT_REACT_GC_STRESS
            1; // Allocation-by-allocation GC is a lifetime check, not a 120 Hz budget.
#else
            20;
#endif
        std::cout << "async updates=" << updates << " reused=" << reuses << " coalesced=" << work.coalesced_requests
                  << " inputs=" << work.accepted_inputs << "/" << work.processed_inputs << '\n';
        require(work.accepted_inputs == work.processed_inputs && work.accepted_inputs == 27 &&
                    work.coalesced_requests > 0 && updates > minimum_updates && reuses > 0,
                "async stall lost inputs or failed to coalesce");
        require(pipeline.submissions == pipeline.completions && pipeline.peak_in_flight == 2,
                "async actual GPU retirement");
        std::ostringstream report;
        report
            << "{\"separate_game_render_owner_threads\":true,\"synthetic_host_hz\":120,\"artificial_owner_stall_ms\":50,\"frames\":240,\"owner_updates\":"
            << updates << ",\"reused_frames\":" << reuses << ",\"coalesced_requests\":" << work.coalesced_requests
            << ",\"accepted_inputs\":" << work.accepted_inputs << ",\"processed_inputs\":" << work.processed_inputs
            << ",\"game_queue_call_ms\":" << GalleryPreviewSamples::json(game_calls)
            << ",\"render_thread_ms\":" << GalleryPreviewSamples::json(render_ms)
            << ",\"owner_active_ms\":" << GalleryPreviewSamples::json(owner_ms)
            << ",\"frame_age_ms\":" << GalleryPreviewSamples::json(age_ms)
            << ",\"submissions\":" << pipeline.submissions << ",\"completions\":" << pipeline.completions
            << ",\"debug_warnings\":0}";
        write_text(folder / "report.json", report.str());
        std::cout
            << "PASS async owner: input order, coalesced overload, immutable complete GPU frames, actual retirement\n";
        return 0;
    }
} // namespace probes
