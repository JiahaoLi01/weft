#pragma once

#include "probe_common.hpp"
#include "flow_probe.hpp"
#include "gallery_constants.hpp"

namespace probes {
    // Original gallery components, native Canvas raster, positioned values and the
    // existing renderer ABI. Only the owner below resolves Canvas identities.
    struct GalleryCanvasSession {
        weft::ScriptDom vm{{weft::probe_constants::gallery::vm_stack_bytes,
                            weft::probe_constants::gallery::host_vm_execution_budget_ms}};
        weft::FontSet fonts{{WEFT_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf"}};
        std::string css = read_file(WEFT_SOURCE_DIR "/examples/crpg-gallery/canvas-profile.css");

        explicit GalleryCanvasSession(unsigned index) {
            vm.install_canvas_host(weft::make_d2d_canvas_factory());
            weft::install_script_platform(vm, weft::ScriptPlatformModule::WebHost);
            weft::install_script_platform(vm, weft::ScriptPlatformModule::AnimationFrame);
            vm.eval(read_file(WEFT_SOURCE_DIR "/examples/crpg-gallery/native/platform.js"));
            vm.eval(read_file(WEFT_SOURCE_DIR "/examples/crpg-gallery/native/WEBGL_TESTS.js"));
            vm.install_layout_host([this](const weft::DomSnapshot& document) {
                return weft::layout_document(flow_find(document, "root"), css,
                                             weft::probe_constants::gallery::canvas_backing_width,
                                             weft::probe_constants::gallery::canvas_backing_height, fonts)
                    .elements;
            });
            // Same deterministic host seed in the real Chromium reference. The
            // original component bodies and drawing calls remain unchanged.
            vm.eval(
                "(()=>{let seed=0xce202610;Math.random=()=>((seed=(Math.imul(seed,1664525)+1013904223)>>>0)/4294967296)})()");
            vm.eval("__ceGallery.mount('WEBGL_TESTS'," + std::to_string(index) + ")");
            pump(weft::probe_constants::gallery::initial_clock_ms);
            require(
                vm.eval(
                    "document.querySelector('canvas').width===800 && document.querySelector('canvas').height===600") ==
                    "true",
                "original Canvas effect did not mount");
        }

        void pump(double time) {
            for (unsigned turn = 0; turn < weft::probe_constants::gallery::task_pump_turn_limit; ++turn) {
                if (vm.eval("__ceHost.pump(" + format_host_time(time) + ")") == "0") {
                    return;
                }
            }
            throw std::runtime_error("WEFT_GALLERY_TASK_LIMIT: ready turns");
        }

        weft::NodeIdentity identity() {
            return vm.node_identity("canvas");
        }

        void input(unsigned index, double x, double y, double time) {
            weft::MouseEvent e;
            e.type = index ? weft::MouseType::Click : weft::MouseType::Move;
            e.client_x = x;
            e.client_y = y;
            e.time_ms = time;
            require(vm.dispatch_mouse(identity(), e) == weft::DispatchResult::DefaultAllowed,
                    "native Canvas input rejected");
        }

        void size(bool reduced) {
            vm.eval(
                reduced
                    ? "document.getElementById('root').style.width='400px';document.getElementById('root').style.height='300px'"
                    : "document.getElementById('root').style.width='800px';document.getElementById('root').style.height='600px'");
        }

        FramePacket packet() {
            const auto page = weft::layout_document(vm.snapshot_subtree("#root"), css,
                                                    weft::probe_constants::gallery::canvas_backing_width,
                                                    weft::probe_constants::gallery::canvas_backing_height, fonts)
                                  .page;
            FramePacket out;
            for (const auto& p : page.paints) {
                require(p.paint.kind == weft::PaintKind::Rectangle && p.image_source.empty() && p.rounded_clips.empty(),
                        "Canvas sample paints only rectangles and its canvas");
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

    inline std::vector<Pixel> gallery_output(Experiment& gpu) {
        gpu.immediate->CopyResource(gpu.staging.Get(), gpu.output.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check_hresult(gpu.immediate->Map(gpu.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                      "Canvas completed readback");
        std::vector<Pixel> out(std::size_t(gpu.width) * gpu.height);
        for (UINT y = 0; y < gpu.height; ++y) {
            std::memcpy(out.data() + std::size_t(y) * gpu.width,
                        static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(y) * mapped.RowPitch,
                        gpu.width * 4);
        }
        gpu.immediate->Unmap(gpu.staging.Get(), 0);
        return out;
    }

    inline std::vector<Pixel> gallery_reference(const std::filesystem::path& path) {
        const auto image = weft::testing::read_png(path);
        require(image.width == weft::probe_constants::gallery::canvas_backing_width &&
                    image.height == weft::probe_constants::gallery::canvas_backing_height,
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
            return maximum <= weft::probe_constants::gallery::canvas_max_channel_difference &&
                   mean[0] <= weft::probe_constants::gallery::canvas_max_mean_difference_r &&
                   mean[1] <= weft::probe_constants::gallery::canvas_max_mean_difference_g &&
                   mean[2] <= weft::probe_constants::gallery::canvas_max_mean_difference_b &&
                   mean[3] <= weft::probe_constants::gallery::canvas_max_mean_difference_a &&
                   bright_mean <= weft::probe_constants::gallery::canvas_max_bright_mean_difference &&
                   bright_union >= weft::probe_constants::gallery::canvas_min_bright_pixels &&
                   bright_iou >= weft::probe_constants::gallery::canvas_min_bright_iou;
        }
    };

    inline CanvasDifference gallery_difference(const std::vector<Pixel>& actual, const std::vector<Pixel>& expected) {
        require(actual.size() == expected.size(), "Canvas comparison dimensions");
        CanvasDifference out;
        std::size_t intersection{};
        double bright_error{};
        for (std::size_t i = 0; i < actual.size(); ++i) {
            const auto a = actual[i];
            const auto b = expected[i];
            const bool ah =
                std::max({a[0], a[1], a[2]}) > weft::probe_constants::gallery::canvas_bright_channel_threshold;
            const bool bh =
                std::max({b[0], b[1], b[2]}) > weft::probe_constants::gallery::canvas_bright_channel_threshold;
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
} // namespace probes
