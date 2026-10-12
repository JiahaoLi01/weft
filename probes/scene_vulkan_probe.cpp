#include "gallery_layer_manifest.hpp"
#include "gallery_scene_session.hpp"
#include "probe_modes.hpp"
#include "scene_gpu_surface_probe.hpp"
#include "render/backends/vulkan/scene_renderer.hpp"

namespace probes {
    namespace vk = weft::render::vulkan;

    namespace {
        // Encoded premultiplied BGRA rows as straight RGBA, as SceneGpuSurfaces::read decodes the D3D11 targets.
        weft::SceneBitmap straight(const std::vector<std::uint8_t>& bgra, unsigned w, unsigned h) {
            weft::SceneBitmap out{w, h, std::vector<std::uint8_t>(bgra.size())};
            constexpr unsigned max = weft::constants::color_channel_max;
            for (std::size_t i = 0; i < bgra.size(); i += 4) {
                const unsigned alpha = bgra[i + 3];
                out.rgba[i + 3] = std::uint8_t(alpha);
                for (unsigned c = 0; c < 3; ++c) {
                    out.rgba[i + c] =
                        alpha ? std::uint8_t(std::min(max, (unsigned(bgra[i + 2 - c]) * max + alpha / 2) / alpha)) : 0;
                }
            }
            return out;
        }
    } // namespace

    // The gallery's 26 CSS and 8 D3 scenes through the native Vulkan scene pass on this machine's device (the one whose
    // name contains adapter, when given): each capture frame against the D3D11 native pass on WARP, and against the
    // unchanged Chromium references. Skipped (77) without a Vulkan device.
    int run_scene_vulkan_test(const std::filesystem::path& folder, const std::string& adapter) {
        std::filesystem::create_directories(folder);
        vk::DeviceOptions options;
        options.adapter = adapter;
        std::string unavailable;
        const auto device = vk::Device::open(options, unavailable);
        if (!device) {
            std::cout << "SKIP Vulkan scene: " << unavailable << '\n';
            return weft::probe_constants::skipped_exit_code;
        }
        weft::render::vulkan::SceneRenderer vulkan(*device, weft::testing::thread_scene_text());
        Experiment gpu(true, weft::probe_constants::gallery::scene_test_surface_size,
                       weft::probe_constants::gallery::scene_test_surface_size);
        gpu.cache_assets = true;
        TwoFramePipeline pipeline(gpu);
        SceneGpuSurfaces surfaces(gpu, pipeline);
        surfaces.retain_paint = surfaces.native_paint = true;
        const auto scale = float(weft::probe_constants::gallery::scene_raster_scale);
        unsigned captures{};
        int worst_difference{};
        std::size_t far_pixels{};
        double worst_mean{};
        double minimum_iou = 1;
        std::ostringstream cases;
        for (unsigned group = 0; group < 2; ++group) {
            for (unsigned index = 0;
                 index < (group ? static_cast<unsigned>(weft::probe_constants::gallery::d3_scene_count)
                                : static_cast<unsigned>(weft::probe_constants::gallery::css_scene_count));
                 ++index) {
                const unsigned slot = group ? weft::probe_constants::gallery::css_scene_count + index : index;
                const auto id = std::string(group ? "d3-" : "css-") + (index < 10 ? "0" : "") + std::to_string(index);
                std::cout << id << " Vulkan..." << std::flush;
                GallerySceneSession scene(group != 0, index,
                                          group ? static_cast<float>(weft::probe_constants::gallery::d3_card_size)
                                                : static_cast<float>(weft::probe_constants::gallery::css_card_width));
                std::array<bool, weft::probe_constants::gallery::core_scene_count> visible{};
                visible[slot] = true;
                int case_difference{};
                for (unsigned frame = 0; frame <= weft::probe_constants::gallery::scene_reference_last_frame; ++frame) {
                    const double time = frame * weft::probe_constants::gallery::fixed_clock_step_ms;
                    scene.step(time);
                    if (!group && index >= 4 && index <= 7 && (frame == 18 || frame == 72)) {
                        scene.input(index == 7 ? weft::MouseType::Click : weft::MouseType::Move,
                                    frame == 72 && index != 7 ? -10.f : scene.width / 2,
                                    frame == 72 && index != 7 ? -10.f : 60.f);
                        scene.step(time);
                    }
                    if (frame != 24 && frame != 63 && frame != 120) {
                        continue;
                    }
                    const auto layers = gallery_layer_manifest(slot, scene.current, true);
                    auto& image = surfaces.paint_owned(slot, std::make_shared<const weft::SceneFrame>(scene.current),
                                                       scale, std::nullopt, layers);
                    surfaces.publish();
                    FramePacket packet;
                    packet.draws.push_back({{0, 0, float(image.width), float(image.height)}, {1, 1, 1, 1}, 0});
                    const auto serial = pipeline.submit(std::move(packet),
                                                        [] {
                                                        },
                                                        {}, {image.handle});
                    surfaces.submitted(serial, visible);
                    const auto d3d11 = surfaces.read(slot);
                    const auto drawn =
                        vulkan.draw(scene.current, scale, d3d11.width, d3d11.height, std::nullopt, slot, layers);
                    require(drawn.has_value(), "Vulkan scene pass fell back");
                    const auto actual = straight(*drawn, d3d11.width, d3d11.height);
                    for (std::size_t i = 0; i < actual.rgba.size(); i += 4) {
                        int worst{};
                        for (unsigned c = 0; c < 4; ++c) {
                            worst = std::max(worst, std::abs(int(actual.rgba[i + c]) - int(d3d11.rgba[i + c])));
                        }
                        case_difference = std::max(case_difference, worst);
                        far_pixels += worst > weft::probe_constants::gallery::vulkan_scene_near_level;
                    }
                    const auto expected = scene_reference(std::filesystem::path(WEFT_SOURCE_DIR) /
                                                          "tests/fixtures/scene-gallery-reference" /
                                                          (id + "-" + std::to_string(frame) + ".png"));
                    const auto diff = scene_difference(actual, expected);
                    const auto mean = *std::max_element(diff.mean.begin(), diff.mean.end());
                    worst_mean = std::max(worst_mean, mean);
                    minimum_iou = std::min(minimum_iou, diff.soft_iou);
                    std::vector<Pixel> pixels(std::size_t(actual.width) * actual.height);
                    std::memcpy(pixels.data(), actual.rgba.data(), actual.rgba.size());
                    save_png(folder / (id + "-" + std::to_string(frame) + ".png"), pixels, actual.width, actual.height);
                    if (!diff.passes()) {
                        std::cerr << " frame=" << frame << " mean=" << mean << " IoU=" << diff.soft_iou << '\n';
                    }
                    require(diff.passes(), "Vulkan scene outside unchanged Chromium tolerances");
                    ++captures;
                }
                scene.unmount();
                worst_difference = std::max(worst_difference, case_difference);
                cases << (cases.tellp() ? "," : "") << "{\"id\":" << json_string(id)
                      << ",\"max_difference_from_d3d11\":" << case_difference << '}';
                std::cout << " OK (max difference from D3D11 " << case_difference << ")\n";
            }
        }
        surfaces.release();
        gpu.check_debug_messages();
        std::cout << "Vulkan scene: " << captures << " captures, max difference from D3D11 " << worst_difference
                  << ", pixels past " << weft::probe_constants::gallery::vulkan_scene_near_level << " levels "
                  << far_pixels << ", worst Chromium mean " << worst_mean << ", minimum IoU " << minimum_iou
                  << ", validation messages " << device->validation_messages << '\n';
        if (device->validation_messages) {
            std::cerr << device->first_validation_message << '\n';
        }
        require(captures == 102 && !vulkan.fallbacks && vulkan.plane_passes > 0 && vulkan.layer_builds > 0 &&
                    !device->validation_messages,
                "Vulkan scene coverage or validation");
        require(worst_difference <= weft::probe_constants::gallery::max_vulkan_scene_difference &&
                    far_pixels <= weft::probe_constants::gallery::max_vulkan_scene_far_pixels,
                "Vulkan scene pass differs from the D3D11 one");
        std::ostringstream report;
        report << "{\"format\":\"scene-vulkan-v1\",\"adapter\":" << json_string(device->name)
               << ",\"validation\":" << (device->validation ? "true" : "false") << ",\"cases\":[" << cases.str()
               << "],\"captures\":" << captures << ",\"max_difference_from_d3d11\":" << worst_difference
               << ",\"pixels_past_near_level\":" << far_pixels << ",\"worst_channel_mean\":" << worst_mean
               << ",\"minimum_soft_iou\":" << minimum_iou << ",\"native_passes\":" << vulkan.passes
               << ",\"plane_passes\":" << vulkan.plane_passes << ",\"layer_builds\":" << vulkan.layer_builds
               << ",\"layer_reuses\":" << vulkan.layer_reuses << ",\"uploads\":" << vulkan.uploads
               << ",\"draws\":" << vulkan.draws << ",\"validation_messages\":" << device->validation_messages << '}';
        write_text(folder / "report.json", report.str());
        std::cout << "PASS Vulkan native scene pass: 26 CSS + 8 D3 / 102 Chromium references / D3D11 comparison\n";
        return 0;
    }
} // namespace probes
