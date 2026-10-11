#include "scene_gpu_surface_probe.hpp"
#include "gallery_scene_session.hpp"
#include "cheetah_probe.hpp"
#include "gallery_canvas_probe.hpp"
#include "gallery_layer_manifest.hpp"
#include "probe_modes.hpp"

namespace probes {
    int run_scene_gpu_test(bool warp, const std::filesystem::path& folder, bool optimized) {
        std::filesystem::create_directories(folder);
        Experiment gpu(warp, weft::probe_constants::gallery::scene_test_surface_size,
                       weft::probe_constants::gallery::scene_test_surface_size);
        gpu.cache_assets = true;
        TwoFramePipeline pipeline(gpu);
        SceneGpuSurfaces surfaces(gpu, pipeline);
        surfaces.retain_paint = surfaces.native_paint = optimized;
        std::ostringstream report;
        report << "{\"format\":\"scene-gpu-original-v1\",\"cases\":[";
        unsigned count{};
        unsigned captures{};
        double worst_mean{};
        double minimum_iou = 1;
        for (unsigned group = 0; group < 2; ++group) {
            for (unsigned index = 0;
                 index < (group ? static_cast<unsigned>(weft::probe_constants::gallery::d3_scene_count)
                                : static_cast<unsigned>(weft::probe_constants::gallery::css_scene_count));
                 ++index) {
                const unsigned slot = group ? weft::probe_constants::gallery::css_scene_count + index : index;
                const auto id = std::string(group ? "d3-" : "css-") + (index < 10 ? "0" : "") + std::to_string(index);
                std::cout << id << " GPU..." << std::flush;
                GallerySceneSession scene(group != 0, index,
                                          group ? static_cast<float>(weft::probe_constants::gallery::d3_card_size)
                                                : static_cast<float>(weft::probe_constants::gallery::css_card_width));
                std::array<bool, weft::probe_constants::gallery::core_scene_count> visible{};
                visible[slot] = true;
                for (unsigned frame = 0; frame <= weft::probe_constants::gallery::scene_reference_last_frame; ++frame) {
                    const double time = frame * weft::probe_constants::gallery::fixed_clock_step_ms;
                    scene.step(time);
                    if (!group && index >= 4 && index <= 7 && (frame == 18 || frame == 72)) {
                        scene.input(index == 7 ? weft::MouseType::Click : weft::MouseType::Move,
                                    frame == 72 && index != 7 ? -10.f : scene.width / 2,
                                    frame == 72 && index != 7 ? -10.f : 60.f);
                        scene.step(time);
                    }
                    auto& image = surfaces.paint_owned(slot, std::make_shared<const weft::SceneFrame>(scene.current),
                                                       weft::probe_constants::gallery::scene_raster_scale, std::nullopt,
                                                       optimized ? gallery_layer_manifest(slot, scene.current, true)
                                                                 : std::vector<weft::NodeIdentity>{});
                    surfaces.publish();
                    FramePacket packet;
                    packet.draws.push_back({{0, 0, float(image.width), float(image.height)}, {1, 1, 1, 1}, 0});
                    const auto serial = pipeline.submit(std::move(packet),
                                                        [] {
                                                        },
                                                        {}, {image.handle});
                    surfaces.submitted(serial, visible);
                    if (frame == 24 || frame == 63 || frame == 120) {
                        const auto actual = surfaces.read(slot);
                        const auto expected = scene_reference(std::filesystem::path(WEFT_SOURCE_DIR) /
                                                              "tests/fixtures/scene-gallery-reference" /
                                                              (id + "-" + std::to_string(frame) + ".png"));
                        const auto diff = scene_difference(actual, expected);
                        const auto mean = *std::max_element(diff.mean.begin(), diff.mean.end());
                        worst_mean = std::max(worst_mean, mean);
                        minimum_iou = std::min(minimum_iou, diff.soft_iou);
                        if (!diff.passes()) {
                            std::cerr << " frame=" << frame << " mean=" << mean << " IoU=" << diff.soft_iou << '\n';
                        }
                        std::vector<Pixel> pixels(std::size_t(actual.width) * actual.height);
                        std::memcpy(pixels.data(), actual.rgba.data(), actual.rgba.size());
                        save_png(folder / (id + "-" + std::to_string(frame) + ".png"), pixels, actual.width,
                                 actual.height);
                        require(diff.passes(), "GPU original scene outside unchanged Chromium tolerances");
                        ++captures;
                    }
                }
                scene.unmount();
                require(scene.vm.stats().live_nodes == weft::probe_constants::gallery::unmounted_live_nodes,
                        "GPU original scene unmount");
                if (count++) {
                    report << ',';
                }
                report << json_string(id);
                std::cout << " OK\n";
            }
        }
        // Translucent encoded texels must be decoded before bilinear filtering.
        // This is a separate CPU alpha/color oracle for the new conversion pass.
        weft::SceneFrame alpha;
        alpha.root.box = {0, 0, 80, 80};
        alpha.root.tag = "div";
        alpha.root.identity = {1, 1, 900};
        alpha.root.style = {{"background-color", "transparent"}, {"overflow", "hidden"}};
        for (unsigned i = 0; i < 2; ++i) {
            auto child = alpha.root;
            child.identity.slot = i + 2;
            child.box = {float(10 + i * 15), 10, 32, 32};
            child.style = {{"background-color", i ? "rgba(0,80,255,.6)" : "rgba(255,40,0,.25)"},
                           {"border-radius", "4px"}};
            alpha.root.children.push_back(child);
        }
        weft::SceneRaster software{weft::testing::thread_scene_text()};
        auto bitmap = software.raster(alpha, weft::probe_constants::gallery::scene_raster_scale);
        auto& source = surfaces.paint(0, alpha);
        surfaces.publish();
        FramePacket expected;
        expected.assets.push_back(
            std::make_shared<const Asset>(Asset{bitmap.width, bitmap.height, Encoding::SrgbStraight, bitmap.rgba}));
        expected.draws.push_back({{0, 0, 80, 80}, {.1f, .2f, .3f, .6f}});
        expected.draws.push_back({{8.25f, 9.75f, 64, 64}, {1, 1, 1, 1}, 0, false, false, .7f});
        auto input = expected;
        input.assets.clear();
        std::array<bool, weft::probe_constants::gallery::core_scene_count> visible{};
        visible[0] = true;
        auto serial = pipeline.submit(std::move(input),
                                      [] {
                                      },
                                      {}, {source.handle});
        surfaces.submitted(serial, visible);
        pipeline.drain();
        auto pixels = gallery_output(gpu);
        unsigned conversion_error{};
        for (int y = 3; y < 79; y += 3) {
            for (int x = 3; x < 79; x += 3) {
                auto reference = cheetah_sample(expected, x, y);
                auto actual = pixels[std::size_t(y) * gpu.width + x];
                for (unsigned c = 0; c < 4; ++c) {
                    if (auto error = unsigned(std::abs(int(reference.pixel[c]) - actual[c]));
                        error > conversion_error) {
                        conversion_error = error;
                        if (error > weft::probe_constants::gallery::max_cpu_oracle_difference) {
                            std::cerr << "conversion (" << x << "," << y << "," << c
                                      << ") CPU=" << unsigned(reference.pixel[c]) << " GPU=" << unsigned(actual[c])
                                      << " difference=" << error << '\n';
                        }
                    }
                }
            }
        }
        require(conversion_error <= weft::probe_constants::gallery::max_cpu_oracle_difference,
                "GPU encoded-alpha conversion/filter oracle");
        auto full = surfaces.read(0);
        auto& cropped = surfaces.paint(0, alpha, weft::probe_constants::gallery::scene_raster_scale,
                                       std::array<float, 4>{5, 4, 64, 70});
        surfaces.publish();
        input = expected;
        input.assets.clear();
        serial = pipeline.submit(std::move(input),
                                 [] {
                                 },
                                 {}, {cropped.handle});
        surfaces.submitted(serial, visible);
        auto clipped = surfaces.read(0);
        for (unsigned y = 0; y < 160; ++y) {
            for (unsigned x = 0; x < 160; ++x) {
                auto offset = (std::size_t(y) * 160 + x) * 4;
                const bool inside = x >= 10 && x < 128 && y >= 8 && y < 140;
                for (unsigned c = 0; c < 4; ++c) {
                    require(clipped.rgba[offset + c] == (inside ? full.rgba[offset + c] : 0),
                            "GPU viewport crop changed visible pixels or retained hidden pixels");
                }
            }
        }
        auto& restored = surfaces.paint(0, alpha);
        surfaces.publish();
        input = expected;
        input.assets.clear();
        serial = pipeline.submit(std::move(input),
                                 [] {
                                 },
                                 {}, {restored.handle});
        surfaces.submitted(serial, visible);
        require(surfaces.read(0).rgba == full.rgba, "GPU scroll reveal retained cropped pixels");
        surfaces.release();
        gpu.evict_assets(0, true);
        gpu.check_debug_messages();
        require(count == weft::probe_constants::gallery::core_scene_count && captures == 102 &&
                    pipeline.submissions == pipeline.completions,
                "GPU original capture/completion coverage");
        if (optimized) {
            std::cout << "native=" << surfaces.native_work().passes << " fallback=" << surfaces.native_work().fallbacks
                      << " layers=" << surfaces.native_work().layer_builds << "/" << surfaces.native_work().layer_reuses
                      << "\n";
        }
        if (optimized) {
            require(surfaces.native_work().passes > 1000 && surfaces.native_work().fallbacks > 500 &&
                        surfaces.native_work().layer_reuses > 300,
                    "original XHTML/SVG native and fallback paths unexercised");
        }
        report << "],\"optimized\":" << (optimized ? "true" : "false")
               << ",\"native_passes\":" << surfaces.native_work().passes
               << ",\"native_fallbacks\":" << surfaces.native_work().fallbacks
               << ",\"native_layer_builds\":" << surfaces.native_work().layer_builds
               << ",\"native_layer_reuses\":" << surfaces.native_work().layer_reuses << ",\"captures\":" << captures
               << ",\"frames\":" << pipeline.submissions << ",\"worst_channel_mean\":" << worst_mean
               << ",\"minimum_soft_iou\":" << minimum_iou << ",\"pool_slots\":" << surfaces.allocated
               << ",\"pool_reuses\":" << surfaces.reuses << ",\"retired_imports\":" << surfaces.retired
               << ",\"peak_in_flight\":" << pipeline.peak_in_flight
               << ",\"alpha_conversion_max_difference\":" << conversion_error
               << ",\"crop_reveal_verified\":true,\"debug_warnings\":0}";
        write_text(folder / "report.json", report.str());
        std::cout << "PASS GPU 26 CSS + 8 D3 / 102 unchanged Chromium references / actual completion retirement\n";
        return 0;
    }
} // namespace probes
