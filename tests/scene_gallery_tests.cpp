#define NOMINMAX
#include "../probes/gallery_scene_session.hpp"
#include "test_constants/scene.hpp"
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <cstring>
#include "support/check.hpp"
#include "scene_test_image.hpp"
#include "scene_geometry_compare.hpp"

using weft::testing::require;

int main(int argc, char** argv) {
    try {
        weft::testing::check_hresult(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM initialization");
        auto out = std::filesystem::path(argc > 1 ? argv[1] : "artifacts/scene-gallery");
        std::filesystem::create_directories(out);
        std::ostringstream report;
        report
            << "{\"format\":\"scene-gallery-native-v1\",\"seed\":3458213392,\"dpi\":2,\"frames_per_case\":121,\"cases\":[";
        unsigned cases{};
        SceneGeometryDifference geometry_difference;
        for (unsigned group = 0; group < 2; ++group) {
            for (unsigned index = 0; index < (group ? 8u : 26u); ++index) {
                const auto id = std::string(group ? "d3-" : "css-") + (index < 10 ? "0" : "") + std::to_string(index);
                std::cout << id << "..." << std::flush;
                GallerySceneSession s(group != 0, index, group ? 384.f : 387.f);
                const auto baseline = s.vm.stats();
                std::vector<std::uint8_t> previous;
                std::size_t changed{};
                unsigned captures{};
                double worst_mean{};
                double worst_iou = 1;
                for (unsigned frame = 0; frame <= weft::test_constants::scene::gallery_last_frame; ++frame) {
                    const double time = frame * weft::test_constants::scene::frame_interval_ms;
                    s.step(time);
                    if (!group && index >= 4 && index <= 7 && (frame == 18 || frame == 72)) {
                        s.input(index == 7 ? weft::MouseType::Click : weft::MouseType::Move,
                                frame == 72 && index != 7 ? -10.f : s.width / 2,
                                frame == 72 && index != 7 ? -10.f : 60.f);
                        s.step(time);
                    }
                    if (frame == 24 || frame == 63 || frame == 120) {
                        const auto& image = s.bitmap();
                        require(image.width == unsigned(s.width * weft::test_constants::scene::gallery_raster_scale) &&
                                    image.height ==
                                        unsigned(s.height * weft::test_constants::scene::gallery_raster_scale),
                                "scene pixel extent");
                        const auto ref = scene_reference(std::filesystem::path(WEFT_SOURCE_DIR) /
                                                         "tests/fixtures/scene-gallery-reference" /
                                                         (id + "-" + std::to_string(frame) + ".png"));
                        const auto diff = scene_difference(image, ref);
                        worst_mean = std::max(worst_mean, *std::max_element(diff.mean.begin(), diff.mean.end()));
                        worst_iou = std::min(worst_iou, diff.soft_iou);
                        if (!diff.passes()) {
                            std::cerr << id << " frame " << frame << " mean=" << worst_mean << " IoU=" << diff.soft_iou
                                      << '\n';
                        }
                        require(diff.passes(), "native scene outside declared Chromium image tolerances");
                        if (frame == 24) {
                            auto blank = image;
                            for (std::size_t i = 0; i < blank.rgba.size(); i += 4) {
                                blank.rgba[i] = weft::test_constants::scene::backdrop_red;
                                blank.rgba[i + 1] = weft::test_constants::scene::backdrop_green;
                                blank.rgba[i + 2] = weft::test_constants::scene::backdrop_blue;
                                blank.rgba[i + 3] = weft::constants::color_channel_max;
                            }
                            require(!scene_difference(blank, ref).passes(),
                                    "visual oracle accepted missing original content");
                        }
                        png(out / (id + "-" + std::to_string(frame) + ".png"), image);
                        compare_scene_geometry(s.raster, s.scene_geometry, s.current, s.svg_geometry, s.width, s.height,
                                               geometry_difference);
                        std::ofstream raw(out / (id + "-" + std::to_string(frame) + ".rgba"), std::ios::binary);
                        raw.write(reinterpret_cast<const char*>(image.rgba.data()), std::streamsize(image.rgba.size()));
                        if (!previous.empty()) {
                            for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
                                changed += std::memcmp(image.rgba.data() + i, previous.data() + i, 4) != 0;
                            }
                        }
                        previous = image.rgba;
                        ++captures;
                        std::ofstream geometry(out / (id + "-" + std::to_string(frame) + "-geometry.json"));
                        geometry << '[';
                        bool comma{};
                        for (const auto& e : s.current.elements) {
                            if (comma) {
                                geometry << ',';
                            }
                            comma = true;
                            geometry << '[' << e.rect[0] << ',' << e.rect[1] << ',' << e.rect[2] << ',' << e.rect[3]
                                     << ']';
                        }
                        geometry << "]\n";
                    }
                }
                require(s.vm.stats().listener_exceptions == 0, "React input exception");
                require(s.vm.eval("__ceGallery.state().errors.length") == "0", "original error boundary");
                if ((!group && index >= 4 && index != 5) || (group && (index == 4 || index == 5))) {
                    require(changed > 0, "original animated case stayed static");
                }
                auto retained = std::move(s.cached);
                s.unmount();
                require(s.vm.stats().live_nodes == weft::test_constants::scene::empty_document_live_nodes &&
                            s.vm.stats().canvas_contexts == 0,
                        "original unmount/reset node leak");
                require(!retained.rgba.empty(), "immutable bitmap lost after VM teardown");
                if (cases++) {
                    report << ',';
                }
                report << "{\"id\":\"" << id << "\",\"execution\":\"passed\",\"captures\":" << captures
                       << ",\"changed_pixels\":" << changed << ",\"live_nodes\":" << baseline.live_nodes
                       << ",\"peak_vm_bytes\":" << baseline.peak_vm_allocated_bytes
                       << ",\"worst_channel_mean\":" << worst_mean << ",\"minimum_soft_iou\":" << worst_iou << '}';
                std::cout << " OK (changed=" << changed << ")\n";
            }
        }
        report << "]}\n";
        std::ofstream(out / "native-report.json") << report.str();
        require(cases == 34, "original registry count");
        require(geometry_difference.max_rect <= weft::test_constants::scene::geometry_rect_tolerance_px &&
                    geometry_difference.max_matrix <= weft::test_constants::scene::geometry_matrix_tolerance,
                "portable geometry outside the reference tolerance");
        std::cout << "portable geometry vs D2D reference: " << geometry_difference.frames << " frames, "
                  << geometry_difference.elements << " elements, max rect difference " << geometry_difference.max_rect
                  << " px, max matrix difference " << geometry_difference.max_matrix << "; " << geometry_difference.hits
                  << " hit tests, " << geometry_difference.boundary_hits << " differing only at boundaries\n";
        std::cout
            << "PASS 26 original CSS + 8 D3: native owner, layout, raster, animation/input, retained values/unmount\n";
        CoUninitialize();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
