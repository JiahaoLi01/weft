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
                        // The native pass updating each draw's view constants itself draws the same pixels as the
                        // pass binding them by offset.
                        if (optimized && frame == weft::probe_constants::gallery::scene_reference_last_frame) {
                            auto& native = surfaces.native_renderer();
                            const bool offsets = native.constant_offsets;
                            native.constant_offsets = false;
                            auto& again =
                                surfaces.paint_owned(slot, std::make_shared<const weft::SceneFrame>(scene.current),
                                                     weft::probe_constants::gallery::scene_raster_scale, std::nullopt,
                                                     gallery_layer_manifest(slot, scene.current, true));
                            native.constant_offsets = offsets;
                            surfaces.publish();
                            FramePacket repeat;
                            repeat.draws.push_back({{0, 0, float(again.width), float(again.height)}, {1, 1, 1, 1}, 0});
                            surfaces.submitted(pipeline.submit(std::move(repeat),
                                                               [] {
                                                               },
                                                               {}, {again.handle}),
                                               visible);
                            require(surfaces.read(slot).rgba == actual.rgba,
                                    "native pass with per-draw constant updates");
                        }
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
        int shadow_error{};
        unsigned shadow_pixels{};
        // Box shadows on the GPU match the reference raster's CPU pixels wherever only shadow shows (box edges
        // antialias differently): two layers, the topmost listed first, one blurred and spread, one hard, under a
        // rounded box at a fractional offset.
        {
            weft::SceneFrame shade;
            shade.root.box = {0, 0, 80, 80};
            shade.root.tag = "div";
            shade.root.identity = {1, 1, 901};
            shade.root.style = {{"background-color", "#ffffff"}};
            auto caster = shade.root;
            caster.identity.slot = 2;
            caster.box = {22.25f, 18.5f, 40, 36};
            caster.style = {{"background-color", "#2050d0"},
                            {"border-top-left-radius", "6px"},
                            {"border-top-right-radius", "6px"},
                            {"border-bottom-right-radius", "12px"},
                            {"border-bottom-left-radius", "6px"},
                            {"box-shadow", "-8px -6px 0px 0px #ff0000, 3px 6px 8px 2px rgba(0, 0, 0, 0.6)"}};
            shade.root.children.push_back(caster);
            const auto scale = weft::probe_constants::gallery::scene_raster_scale;
            const auto reference = software.raster(shade, float(scale));
            const auto shadows_before = surfaces.native_work().shadow_builds;
            const auto fallbacks_before = surfaces.native_work().fallbacks;
            auto& shaded = surfaces.paint(0, shade);
            surfaces.publish();
            FramePacket packet;
            packet.draws.push_back({{0, 0, float(shaded.width), float(shaded.height)}, {1, 1, 1, 1}, 0});
            serial = pipeline.submit(std::move(packet),
                                     [] {
                                     },
                                     {}, {shaded.handle});
            surfaces.submitted(serial, visible);
            const auto drawn = surfaces.read(0);
            require(drawn.width == reference.width && drawn.height == reference.height, "GPU shadow surface size");
            for (unsigned y = 0; y < drawn.height; ++y) {
                for (unsigned x = 0; x < drawn.width; ++x) {
                    // Device pixel centers more than 2 px outside the box's rectangle.
                    const float cx = (x + .5f) / scale;
                    const float cy = (y + .5f) / scale;
                    const float margin = 2.f / scale;
                    if (cx > caster.box[0] - margin && cx < caster.box[0] + caster.box[2] + margin &&
                        cy > caster.box[1] - margin && cy < caster.box[1] + caster.box[3] + margin) {
                        continue;
                    }
                    const auto offset = (std::size_t(y) * drawn.width + x) * 4;
                    if (reference.rgba[offset] < 200 || reference.rgba[offset + 1] < 200) {
                        ++shadow_pixels;
                    }
                    for (unsigned c = 0; c < 4; ++c) {
                        shadow_error = std::max(
                            shadow_error, std::abs(int(drawn.rgba[offset + c]) - int(reference.rgba[offset + c])));
                    }
                }
            }
            std::cout << "shadow oracle: " << shadow_pixels << " shadow pixels, max difference " << shadow_error
                      << '\n';
            require(shadow_pixels > weft::probe_constants::gallery::min_shadow_oracle_pixels &&
                        shadow_error <= weft::probe_constants::gallery::max_shadow_oracle_difference,
                    "GPU box shadow differs from the reference raster");
            require(!optimized || (surfaces.native_work().shadow_builds > shadows_before &&
                                   surfaces.native_work().fallbacks == fallbacks_before),
                    "native box shadow fell back");
        }

        // A GPU frame against the reference raster's CPU pixels: the mean channel difference, the largest one in solid
        // regions (the reference uniform within solid_region_tolerance over 5 x 5 pixels), the pixels that differ by
        // more than vector_outlier_level, and the reference's inked pixels (some channel below ink_level).
        struct OracleDifference {
            double mean{};
            int solid{};
            unsigned outliers{};
            unsigned ink{};
        };

        const auto compare = [](const weft::SceneBitmap& drawn, const weft::SceneBitmap& reference) {
            OracleDifference out;
            const auto w = int(drawn.width);
            const auto h = int(drawn.height);
            double total{};
            for (std::size_t i = 0; i < drawn.rgba.size(); ++i) {
                total += std::abs(int(drawn.rgba[i]) - int(reference.rgba[i]));
            }
            for (std::size_t i = 0; i < drawn.rgba.size(); i += 4) {
                int worst{};
                for (int c = 0; c < 4; ++c) {
                    worst = std::max(worst, std::abs(int(drawn.rgba[i + c]) - int(reference.rgba[i + c])));
                }
                out.outliers += worst > weft::probe_constants::gallery::vector_outlier_level;
            }
            out.mean = total / double(drawn.rgba.size());
            const auto at = [&](const weft::SceneBitmap& b, int x, int y, int c) {
                return int(b.rgba[(std::size_t(y) * b.width + x) * 4 + c]);
            };
            for (int y = 2; y < h - 2; ++y) {
                for (int x = 2; x < w - 2; ++x) {
                    bool solid = true;
                    for (int dy = -2; solid && dy <= 2; ++dy) {
                        for (int dx = -2; solid && dx <= 2; ++dx) {
                            for (int c = 0; c < 4; ++c) {
                                solid = solid && std::abs(at(reference, x + dx, y + dy, c) - at(reference, x, y, c)) <=
                                                     weft::probe_constants::gallery::solid_region_tolerance;
                            }
                        }
                    }
                    if (solid) {
                        for (int c = 0; c < 4; ++c) {
                            out.solid = std::max(out.solid, std::abs(at(drawn, x, y, c) - at(reference, x, y, c)));
                        }
                    }
                }
            }
            for (std::size_t i = 0; i < reference.rgba.size(); i += 4) {
                out.ink += std::min({reference.rgba[i], reference.rgba[i + 1], reference.rgba[i + 2]}) <
                           weft::probe_constants::gallery::ink_level;
            }
            return out;
        };
        const auto save = [&](const weft::SceneBitmap& b, const char* name) {
            std::vector<Pixel> pixels(std::size_t(b.width) * b.height);
            std::memcpy(pixels.data(), b.rgba.data(), b.rgba.size());
            save_png(folder / name, pixels, b.width, b.height);
        };
        // SVG on the GPU matches the reference raster's CPU pixels: fills by rule, strokes with miter, round and bevel
        // joins and every cap, dashes, an opacity group and transforms. Edges antialias differently (exact area
        // coverage against Direct2D's), so the frame's mean difference and its solid regions are compared.
        double vector_mean{};
        int vector_solid{};
        unsigned vector_outliers{};
        {
            using Pairs = std::initializer_list<std::pair<const char*, const char*>>;
            const auto element = [](const char* tag, std::uint32_t slot, Pairs attributes, Pairs style) {
                weft::SceneNode e;
                e.identity = {1, 1, slot};
                e.tag = tag;
                e.space = weft::constants::svg_namespace_uri;
                for (const auto& [k, v] : attributes) {
                    e.attributes[k] = v;
                }
                for (const auto& [k, v] : style) {
                    e.style[k] = v;
                }
                return e;
            };
            weft::SceneFrame drawing;
            drawing.root.box = {0, 0, 80, 80};
            drawing.root.tag = "div";
            drawing.root.identity = {1, 1, 910};
            drawing.root.style = {{"background-color", "#ffffff"}};
            auto svg = element("svg", 911, {{"viewBox", "0 0 40 40"}}, {});
            svg.box = {0, 0, 80, 80};
            auto group = element("g", 918, {{"transform", "rotate(15 20 20)"}}, {{"opacity", "0.6"}});
            group.children = {element("rect", 919,
                                      {{"x", "24"}, {"y", "20"}, {"width", "10"}, {"height", "8"}, {"rx", "2"}},
                                      {{"fill", "#a020a0"}}),
                              element("circle", 920, {{"cx", "10"}, {"cy", "26"}, {"r", "4"}},
                                      {{"fill", "none"}, {"stroke", "#000000"}, {"stroke-width", "1"}})};
            svg.children = {
                element("path", 912, {{"d", "M20 2 L24 36 L4 12 L36 12 L16 36 Z"}},
                        {{"fill", "#2050d0"}, {"fill-rule", "evenodd"}}),
                element("polyline", 913, {{"points", "4 30 20 33 6 37"}},
                        {{"fill", "none"}, {"stroke", "#d02020"}, {"stroke-width", "1.5"}}),
                element(
                    "polyline", 914, {{"points", "26 36 30 30 34 36"}},
                    {{"fill", "none"}, {"stroke", "#2080d0"}, {"stroke-width", "1.5"}, {"stroke-linejoin", "round"}}),
                element("path", 915, {{"d", "M4 4 Q20 0 36 4"}},
                        {{"fill", "none"},
                         {"stroke", "#20a040"},
                         {"stroke-width", "1"},
                         {"stroke-dasharray", "3 1.5"},
                         {"stroke-linecap", "round"}}),
                element("line", 916, {{"x1", "2"}, {"y1", "20"}, {"x2", "12"}, {"y2", "22"}},
                        {{"stroke", "#000000"}, {"stroke-width", "2"}, {"stroke-linecap", "square"}}),
                element("rect", 921, {{"x", "1"}, {"y", "26"}, {"width", "8"}, {"height", "12"}},
                        {{"fill", "#64c2a6"}, {"fill-opacity", "0.7"}}),
                element("rect", 922, {{"x", "30"}, {"y", "14"}, {"width", "9"}, {"height", "6"}},
                        {{"fill", "#d4a843"}, {"fill-opacity", "0.55"}, {"stroke", "#0a0806"}, {"stroke-width", "1"}}),
                element(
                    "ellipse", 917,
                    {{"cx", "30"}, {"cy", "8"}, {"rx", "6"}, {"ry", "3"}, {"transform", "scale(1 1.2)"}},
                    {{"fill", "none"}, {"stroke", "#806000"}, {"stroke-width", "0.8"}, {"stroke-linejoin", "bevel"}}),
                group};
            // A clip-path rectangle under a rotation clips in the element's own space.
            auto defs = element("defs", 923, {}, {});
            auto clip_path = element("clipPath", 924, {{"id", "turned"}}, {});
            clip_path.children = {
                element("rect", 925, {{"x", "16"}, {"y", "16"}, {"width", "10"}, {"height", "8"}, {"rx", "2"}}, {})};
            defs.children = {clip_path};
            svg.children.insert(svg.children.begin(), defs);
            svg.children.push_back(element("rect", 926,
                                           {{"x", "12"},
                                            {"y", "12"},
                                            {"width", "18"},
                                            {"height", "16"},
                                            {"clip-path", "url(#turned)"},
                                            {"transform", "rotate(25 21 20)"}},
                                           {{"fill", "#20a0a0"}}));
            drawing.root.children.push_back(svg);
            // A rotated box with overflow hidden clips its larger child to its rounded padding box.
            weft::SceneNode turned;
            turned.identity = {1, 1, 927};
            turned.tag = "div";
            turned.box = {44, 44, 30, 24};
            turned.style = {{"overflow", "hidden"},
                            {"transform", "rotate(25deg)"},
                            {"border-top-left-radius", "8px"},
                            {"border-bottom-right-radius", "6px"},
                            {"border-top-width", "2px"},
                            {"border-right-width", "2px"},
                            {"border-bottom-width", "2px"},
                            {"border-left-width", "2px"},
                            {"border-top-color", "#000000"},
                            {"border-right-color", "#000000"},
                            {"border-bottom-color", "#000000"},
                            {"border-left-color", "#000000"},
                            {"background-color", "#f0e0a0"}};
            weft::SceneNode spill;
            spill.identity = {1, 1, 928};
            spill.tag = "div";
            spill.box = {-10, -10, 60, 50};
            spill.style = {{"background-color", "#2060c0"}};
            turned.children = {spill};
            drawing.root.children.push_back(turned);
            const auto scale = weft::probe_constants::gallery::scene_raster_scale;
            const auto reference = software.raster(drawing, float(scale));
            const auto paths_before = surfaces.native_work().path_builds;
            const auto fallbacks_before = surfaces.native_work().fallbacks;
            auto& drawn_surface = surfaces.paint(0, drawing);
            surfaces.publish();
            FramePacket packet;
            packet.draws.push_back({{0, 0, float(drawn_surface.width), float(drawn_surface.height)}, {1, 1, 1, 1}, 0});
            serial = pipeline.submit(std::move(packet),
                                     [] {
                                     },
                                     {}, {drawn_surface.handle});
            surfaces.submitted(serial, visible);
            const auto drawn = surfaces.read(0);
            require(drawn.width == reference.width && drawn.height == reference.height, "GPU SVG surface size");
            // A stroked rectangle ends its stroke square at the corners in both (Chromium's miter); the pixel inside
            // the outer corner of the stroke is the stroke's color.
            for (const auto* corner : {drawn.rgba.data() + (std::size_t(54) * drawn.width + 157) * 4,
                                       reference.rgba.data() + (std::size_t(54) * reference.width + 157) * 4}) {
                require(corner[0] <= weft::probe_constants::gallery::corner_stroke_level &&
                            corner[1] <= weft::probe_constants::gallery::corner_stroke_level,
                        "stroked rectangle corner");
            }
            // The translucent rectangle (#64c2a6 at fill-opacity .7, on white) quantizes as Chromium's colors do,
            // halves up: alpha .7 * 255 = 178.5 becomes 179.
            {
                const auto level = [](float v) {
                    return int(std::floor(v * weft::constants::color_channel_max + .5f));
                };
                const float a = .7f;
                const std::array<int, 3> color{0x64, 0xc2, 0xa6};
                const auto* p = drawn.rgba.data() + (std::size_t(110) * drawn.width + 8) * 4;
                for (int c = 0; c < 3; ++c) {
                    const int quantized = level(color[c] / float(weft::constants::color_channel_max) * a) +
                                          weft::constants::color_channel_max - level(a);
                    require(int(p[c]) == quantized, "GPU translucent fill quantization");
                }
            }
            const auto difference = compare(drawn, reference);
            vector_mean = difference.mean;
            vector_solid = difference.solid;
            vector_outliers = difference.outliers;
            const auto ink = difference.ink;
            save(drawn, "vector-oracle.png");
            save(reference, "vector-oracle-reference.png");
            std::cout << "vector oracle: mean difference " << vector_mean << ", solid regions max " << vector_solid
                      << ", outliers " << vector_outliers << '\n';
            require(ink > weft::probe_constants::gallery::min_vector_oracle_ink &&
                        vector_mean <= weft::probe_constants::gallery::max_vector_oracle_mean &&
                        vector_solid <= weft::probe_constants::gallery::max_vector_oracle_solid &&
                        vector_outliers <= weft::probe_constants::gallery::max_vector_oracle_outliers,
                    "GPU SVG differs from the reference raster");
            require(!optimized || (surfaces.native_work().path_builds > paths_before &&
                                   surfaces.native_work().fallbacks == fallbacks_before),
                    "native SVG fell back");
        }
        // Perspective planes on the GPU match the reference raster's CPU pixels: each plane's content drawn flat (a
        // rounded, bordered box with children, a gradient under opacity, a plane inside a plane) and projected; an
        // edge-on plane draws nothing. The reference samples its planes without antialiasing their edges, so the
        // frame's mean difference and its solid regions are compared.
        double plane_mean{};
        int plane_solid{};
        unsigned plane_outliers{};
        // Color glyphs (COLRv1 emoji of Segoe UI Emoji) drawn natively match the reference raster's DirectWrite: the
        // same paint, antialiased differently.
        double color_mean{};
        int color_solid{};
        unsigned color_outliers{};
        {
            weft::SceneFrame frame;
            frame.root.box = {0, 0, 220, 48};
            frame.root.tag = "div";
            frame.root.identity = {1, 1, 960};
            frame.root.style = {{"background-color", "#ffffff"}};
            weft::SceneNode label;
            label.identity = {1, 1, 961};
            label.tag = "div";
            label.box = {6, 8, 208, 32};
            label.text = "A\xF0\x9F\x94\xA5\xE2\x9A\x94\xF0\x9F\x92\x8E\xE2\x98\xA0 42";
            label.style = {{"font-size", "22px"}, {"color", "#303030"}};
            frame.root.children = {label};
            const auto scale = weft::probe_constants::gallery::scene_raster_scale;
            const auto reference = software.raster(frame, float(scale));
            const auto references_before = surfaces.native_work().reference_builds;
            const auto fallbacks_before = surfaces.native_work().fallbacks;
            auto& drawn_surface = surfaces.paint(0, frame);
            surfaces.publish();
            FramePacket packet;
            packet.draws.push_back({{0, 0, float(drawn_surface.width), float(drawn_surface.height)}, {1, 1, 1, 1}, 0});
            serial = pipeline.submit(std::move(packet),
                                     [] {
                                     },
                                     {}, {drawn_surface.handle});
            surfaces.submitted(serial, visible);
            const auto drawn = surfaces.read(0);
            require(drawn.width == reference.width && drawn.height == reference.height, "GPU color text surface size");
            const auto difference = compare(drawn, reference);
            color_mean = difference.mean;
            color_solid = difference.solid;
            color_outliers = difference.outliers;
            save(drawn, "color-text-oracle.png");
            save(reference, "color-text-oracle-reference.png");
            std::cout << "color text oracle: mean difference " << color_mean << ", solid regions max " << color_solid
                      << ", outliers " << color_outliers << ", ink " << difference.ink << '\n';
            require(difference.ink > weft::probe_constants::gallery::min_color_text_oracle_ink &&
                        color_mean <= weft::probe_constants::gallery::max_color_text_oracle_mean &&
                        color_solid <= weft::probe_constants::gallery::max_color_text_oracle_solid &&
                        color_outliers <= weft::probe_constants::gallery::max_color_text_oracle_outliers,
                    "GPU color text differs from the reference raster");
            require(!optimized || (surfaces.native_work().reference_builds == references_before &&
                                   surfaces.native_work().fallbacks == fallbacks_before),
                    "native color text used the reference raster or fell back");
        }
        // An explicit layer at a whole device pixel offset draws what its content draws inline: the layer's image
        // holds the content texel for pixel, so only its 8-bit premultiplied intermediate may differ.
        int layer_difference{};
        {
            weft::SceneFrame frame;
            frame.root.box = {0, 0, 120, 80};
            frame.root.tag = "div";
            frame.root.identity = {1, 1, 950};
            frame.root.style = {{"background-color", "#ffffff"}};
            weft::SceneNode card;
            card.identity = {1, 1, 951};
            card.tag = "div";
            card.box = {10, 12, 90, 56};
            card.style = {{"background-color", "#3060c0"},      {"border-top-left-radius", "6px"},
                          {"border-top-right-radius", "6px"},   {"border-bottom-right-radius", "6px"},
                          {"border-bottom-left-radius", "6px"}, {"border-top-width", "2px"},
                          {"border-right-width", "2px"},        {"border-bottom-width", "2px"},
                          {"border-left-width", "2px"},         {"border-top-color", "#102040"},
                          {"border-right-color", "#102040"},    {"border-bottom-color", "#102040"},
                          {"border-left-color", "#102040"}};
            weft::SceneNode badge;
            badge.identity = {1, 1, 952};
            badge.tag = "div";
            badge.box = {8, 8, 30, 14};
            badge.style = {{"background-color", "#f0c030"}, {"opacity", "0.8"}};
            weft::SceneNode label;
            label.identity = {1, 1, 953};
            label.tag = "div";
            label.box = {8, 28, 70, 20};
            label.text = "Layer 42";
            label.style = {{"font-size", "12px"}, {"color", "#ffffff"}};
            card.children = {badge, label};
            frame.root.children = {card};
            const auto draw = [&](std::vector<weft::NodeIdentity> layers) {
                auto& drawn_surface = surfaces.paint_owned(0, std::make_shared<const weft::SceneFrame>(frame),
                                                           weft::probe_constants::gallery::scene_raster_scale,
                                                           std::nullopt, std::move(layers));
                surfaces.publish();
                FramePacket packet;
                packet.draws.push_back(
                    {{0, 0, float(drawn_surface.width), float(drawn_surface.height)}, {1, 1, 1, 1}, 0});
                serial = pipeline.submit(std::move(packet),
                                         [] {
                                         },
                                         {}, {drawn_surface.handle});
                surfaces.submitted(serial, visible);
                return surfaces.read(0);
            };
            const auto layers_before = surfaces.native_work().layer_builds;
            const auto fallbacks_before = surfaces.native_work().fallbacks;
            const auto inline_drawn = draw({});
            const auto layered = draw({card.identity});
            require(inline_drawn.width == layered.width && inline_drawn.height == layered.height,
                    "GPU layer surface size");
            for (std::size_t i = 0; i < layered.rgba.size(); ++i) {
                layer_difference =
                    std::max(layer_difference, std::abs(int(layered.rgba[i]) - int(inline_drawn.rgba[i])));
            }
            save(layered, "layer-oracle.png");
            std::cout << "layer oracle: max difference " << layer_difference << '\n';
            require(layer_difference <= weft::probe_constants::gallery::max_layer_oracle_difference,
                    "GPU layer differs from its content drawn inline");
            require(!optimized || (surfaces.native_work().layer_builds == layers_before + 1 &&
                                   surfaces.native_work().fallbacks == fallbacks_before),
                    "native layer fell back");
        }
        {
            using Pairs = std::initializer_list<std::pair<const char*, const char*>>;
            const auto box = [](std::uint32_t slot, std::array<float, 4> rect, Pairs style) {
                weft::SceneNode n;
                n.identity = {1, 1, slot};
                n.tag = "div";
                n.box = rect;
                for (const auto& [k, v] : style) {
                    n.style[k] = v;
                }
                return n;
            };
            weft::SceneFrame frame;
            frame.root = box(940, {0, 0, 160, 100}, {{"background-color", "#ffffff"}});
            auto card = box(941, {12, 14, 60, 70},
                            {{"transform", "perspective(220px) rotateY(40deg)"},
                             {"background-color", "#2a6fdb"},
                             {"border-top-left-radius", "10px"},
                             {"border-top-right-radius", "10px"},
                             {"border-bottom-right-radius", "10px"},
                             {"border-bottom-left-radius", "10px"},
                             {"border-top-width", "3px"},
                             {"border-right-width", "3px"},
                             {"border-bottom-width", "3px"},
                             {"border-left-width", "3px"},
                             {"border-top-color", "#102040"},
                             {"border-right-color", "#102040"},
                             {"border-bottom-color", "#102040"},
                             {"border-left-color", "#102040"}});
            card.children = {box(942, {8, 8, 30, 20}, {{"background-color", "#f0c030"}}),
                             box(943, {14, 36, 34, 24},
                                 {{"transform", "perspective(120px) rotateY(-35deg)"},
                                  {"background-color", "#20a060"},
                                  {"border-top-left-radius", "4px"},
                                  {"border-bottom-right-radius", "4px"}})};
            auto faded = box(944, {90, 16, 56, 64},
                             {{"transform", "perspective(160px) rotateY(-55deg)"},
                              {"opacity", "0.85"},
                              {"background-image", "linear-gradient(90deg, #f0a020, #c02060)"}});
            faded.children = {box(945, {10, 12, 36, 18},
                                  {{"background-color", "#ffffff"},
                                   {"border-top-left-radius", "9px"},
                                   {"border-top-right-radius", "9px"},
                                   {"border-bottom-right-radius", "9px"},
                                   {"border-bottom-left-radius", "9px"}})};
            auto edge_on = box(946, {60, 86, 40, 10},
                               {{"transform", "perspective(100px) rotateY(90deg)"}, {"background-color", "#000000"}});
            frame.root.children = {card, faded, edge_on};
            const auto scale = weft::probe_constants::gallery::scene_raster_scale;
            const auto reference = software.raster(frame, float(scale));
            const auto planes_before = surfaces.native_work().plane_passes;
            const auto fallbacks_before = surfaces.native_work().fallbacks;
            auto& drawn_surface = surfaces.paint(0, frame);
            surfaces.publish();
            FramePacket packet;
            packet.draws.push_back({{0, 0, float(drawn_surface.width), float(drawn_surface.height)}, {1, 1, 1, 1}, 0});
            serial = pipeline.submit(std::move(packet),
                                     [] {
                                     },
                                     {}, {drawn_surface.handle});
            surfaces.submitted(serial, visible);
            const auto drawn = surfaces.read(0);
            require(drawn.width == reference.width && drawn.height == reference.height, "GPU plane surface size");
            // The edge-on plane leaves its row white in both.
            for (const auto* row : {&drawn, &reference}) {
                const auto* p = row->rgba.data() + (std::size_t(91 * scale) * row->width + 80 * scale) * 4;
                require(p[0] == 255 && p[1] == 255 && p[2] == 255, "edge-on plane drew");
            }
            const auto difference = compare(drawn, reference);
            plane_mean = difference.mean;
            plane_solid = difference.solid;
            plane_outliers = difference.outliers;
            save(drawn, "plane-oracle.png");
            save(reference, "plane-oracle-reference.png");
            std::cout << "plane oracle: mean difference " << plane_mean << ", solid regions max " << plane_solid
                      << ", outliers " << plane_outliers << ", ink " << difference.ink << '\n';
            require(difference.ink > weft::probe_constants::gallery::min_plane_oracle_ink &&
                        plane_mean <= weft::probe_constants::gallery::max_plane_oracle_mean &&
                        plane_solid <= weft::probe_constants::gallery::max_plane_oracle_solid &&
                        plane_outliers <= weft::probe_constants::gallery::max_plane_oracle_outliers,
                    "GPU perspective planes differ from the reference raster");
            // Three plane contents: two planes and the one inside the first.
            require(!optimized || (surfaces.native_work().plane_passes == planes_before + 3 &&
                                   surfaces.native_work().fallbacks == fallbacks_before),
                    "native planes fell back");
        }
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
            // XHTML, SVG and perspective planes all draw natively.
            const auto& native = surfaces.native_work();
            require(native.passes > weft::probe_constants::gallery::min_native_scene_passes && native.fallbacks == 0 &&
                        native.layer_reuses > weft::probe_constants::gallery::min_native_layer_reuses &&
                        native.path_builds > 0 && native.plane_passes > 0,
                    "original XHTML/SVG/plane native paths unexercised or fell back");
            // Layers and color glyphs draw natively: the native path never uses the reference raster.
            require(native.layer_builds > 0 && native.reference_builds == 0, "native path used the reference raster");
        }
        report << "],\"optimized\":" << (optimized ? "true" : "false")
               << ",\"native_passes\":" << surfaces.native_work().passes
               << ",\"native_fallbacks\":" << surfaces.native_work().fallbacks
               << ",\"fallback_reasons\":" << fallback_reasons_json(surfaces.native_work())
               << ",\"native_layer_builds\":" << surfaces.native_work().layer_builds
               << ",\"native_layer_reuses\":" << surfaces.native_work().layer_reuses
               << ",\"native_plane_passes\":" << surfaces.native_work().plane_passes
               << ",\"native_reference_builds\":" << surfaces.native_work().reference_builds
               << ",\"captures\":" << captures << ",\"frames\":" << pipeline.submissions
               << ",\"worst_channel_mean\":" << worst_mean << ",\"minimum_soft_iou\":" << minimum_iou
               << ",\"pool_slots\":" << surfaces.allocated << ",\"pool_reuses\":" << surfaces.reuses
               << ",\"retired_imports\":" << surfaces.retired << ",\"peak_in_flight\":" << pipeline.peak_in_flight
               << ",\"alpha_conversion_max_difference\":" << conversion_error
               << ",\"vector_oracle_mean\":" << vector_mean << ",\"vector_oracle_solid_max\":" << vector_solid
               << ",\"vector_oracle_outliers\":" << vector_outliers << ",\"shadow_oracle_pixels\":" << shadow_pixels
               << ",\"shadow_oracle_max_difference\":" << shadow_error << ",\"plane_oracle_mean\":" << plane_mean
               << ",\"plane_oracle_solid_max\":" << plane_solid << ",\"plane_oracle_outliers\":" << plane_outliers
               << ",\"layer_oracle_max_difference\":" << layer_difference
               << ",\"color_text_oracle_mean\":" << color_mean << ",\"color_text_oracle_solid_max\":" << color_solid
               << ",\"color_text_oracle_outliers\":" << color_outliers
               << ",\"crop_reveal_verified\":true,\"debug_warnings\":0}";
        write_text(folder / "report.json", report.str());
        std::cout << "PASS GPU 26 CSS + 8 D3 / 102 unchanged Chromium references / actual completion retirement\n";
        return 0;
    }
} // namespace probes
