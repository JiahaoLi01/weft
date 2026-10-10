#pragma once
#include "gallery_constants.hpp"

int run_complex_css_test(bool warp, const std::filesystem::path& folder) {
    std::filesystem::create_directories(folder);
    Experiment gpu(warp, caseelephant::probe_constants::gallery::complex_css_surface_width,
                   caseelephant::probe_constants::gallery::complex_css_surface_height);
    gpu.cache_assets = true;
    TwoFramePipeline pipeline(gpu);
    SceneGpuSurfaces surfaces(gpu, pipeline);
    caseelephant::SceneRasterProbe reference;
    unsigned comparisons{};
    double worst_mean{};
    double minimum_iou = 1;
    std::size_t projected{};
    std::size_t uploads{};
    std::size_t uniform{};
    std::size_t fallback{};
    caseelephant::ProbeSceneFrame stable;
    for (unsigned frame = 0; frame < caseelephant::probe_constants::gallery::complex_css_projective_frames; ++frame) {
        using Node = caseelephant::ProbeSceneNode;
        caseelephant::ProbeSceneFrame scene;
        auto& root = scene.root;
        root.tag = "div";
        root.identity = {2, 1, 1};
        root.box = {0, 0, caseelephant::probe_constants::gallery::complex_css_scene_width,
                    caseelephant::probe_constants::gallery::complex_css_scene_height};
        root.style = {{"background-color", "#050403"}, {"overflow", "hidden"}};
        Node container;
        container.tag = "div";
        container.identity = {2, 2, 1};
        container.box = {12.25f, 8.5f, 150, 120};
        container.style = {{"opacity", frame % 3 == 0 ? ".7" : "1"},
                           {"transform", "translate(3px,2px) rotate(5deg)"},
                           {"overflow", "hidden"}};
        Node plane;
        plane.tag = "div";
        plane.identity = {2, 3, 1 + frame / 8};
        plane.box = {23.25f, 12.5f, 84, 70};
        const auto angle = frame == 33 ? 90 : float(int((frame * 47) % 261) - 130);
        plane.style = {{"background-color", frame % 8 < 4 ? "rgba(212,168,67,.6)" : "#225577"},
                       {"transform", "perspective(400px) rotateY(" + std::to_string(angle) + "deg)"},
                       {"transform-origin", frame % 8 < 4 ? "25% 70%" : "50% 50%"},
                       {"opacity", ".65"},
                       {"border-top-left-radius", "12px"},
                       {"border-top-right-radius", "4px"},
                       {"border-bottom-right-radius", "9px"},
                       {"border-bottom-left-radius", "7px"}};
        for (const auto* edge : {"top", "right", "bottom", "left"}) {
            plane.style[std::string("border-") + edge + "-width"] = "2px";
            plane.style[std::string("border-") + edge + "-color"] = "#ffffff";
        }
        Node label;
        label.tag = "div";
        label.identity = {2, 4, plane.identity.generation};
        label.box = {10, 10, 55, 28};
        label.text = frame % 8 < 4 ? "Flip" : "New";
        label.style = {{"font-size", frame % 8 < 4 ? "14px" : "18px"}, {"color", "#ff3355"}};
        plane.children.push_back(label);
        container.children.push_back(plane);
        root.children.push_back(container);
        Node ring;
        ring.tag = "div";
        ring.identity = {2, 5, 1};
        ring.box = {110, 54, 54, 54};
        ring.style = {{"transform", "scale(" + std::to_string(.6f + float(frame % 8) * .08f) + ")"},
                      {"opacity", frame % 2 ? ".35" : ".75"},
                      {"background-color", "rgba(0,80,255,.2)"}};
        for (const auto* corner : {"top-left", "top-right", "bottom-right", "bottom-left"}) {
            ring.style[std::string("border-") + corner + "-radius"] = frame % 4 == 0 ? "10px" : "50%";
        }
        for (const auto* edge : {"top", "right", "bottom", "left"}) {
            ring.style[std::string("border-") + edge + "-width"] = "4px";
            ring.style[std::string("border-") + edge + "-color"] = "rgba(212,168,67,.6)";
        }
        if (frame % 4 == 1) {
            ring.style["border-top-color"] = "#ff3344";
        }
        if (frame % 4 == 2) {
            ring.style["border-left-width"] = "8px";
        }
        if (frame % 4 == 3) {
            ring.style["border-right-style"] = "none";
        }
        root.children.push_back(ring);
        if (frame == 63) {
            stable = scene;
        }
        const float scale = frame % 3 == 0 ? 1.f : frame % 3 == 1 ? 1.5f : 2.f;
        const auto expected = reference.raster(scene, scale);
        auto& image = surfaces.paint(0, scene, scale);
        surfaces.publish();
        FramePacket packet;
        packet.draws.push_back({{0, 0, float(image.width), float(image.height)}, {1, 1, 1, 1}, 0});
        std::array<bool, caseelephant::probe_constants::gallery::core_scene_count> visible{};
        visible[0] = true;
        const auto serial = pipeline.submit(std::move(packet),
                                            [] {
                                            },
                                            {}, {image.handle});
        surfaces.submitted(serial, visible);
        const auto actual = surfaces.read(0);
        const auto diff = scene_difference(actual, expected);
        const auto mean = *std::max_element(diff.mean.begin(), diff.mean.end());
        worst_mean = std::max(worst_mean, mean);
        minimum_iou = std::min(minimum_iou, diff.soft_iou);
        if (frame % 16 == 0 || !diff.passes()) {
            for (auto pair : {std::pair{"gpu", &actual}, std::pair{"software", &expected}}) {
                std::vector<Pixel> pixels(std::size_t(pair.second->width) * pair.second->height);
                std::memcpy(pixels.data(), pair.second->rgba.data(), pair.second->rgba.size());
                save_png(folder / (std::string(pair.first) + "-" + std::to_string(frame) + ".png"), pixels,
                         pair.second->width, pair.second->height);
            }
        }
        if (!diff.passes()) {
            std::cerr << "complex frame=" << frame << " mean=" << mean << " IoU=" << diff.soft_iou << '\n';
        }
        require(diff.passes(), "projective/ring mutations differ from full software reference");
        ++comparisons;
        // A completed surface can switch CPU/GPU targets without stale bitmaps.
        if (frame % 9 == 0) {
            require(image.painter.raster(scene, scale).rgba == expected.rgba, "GPU to software target switch");
        }
        const auto stats = image.painter.work();
        projected = std::max(projected, stats.gpu_projective_draws);
        uploads = std::max(uploads, stats.gpu_plane_uploads);
        uniform = std::max(uniform, stats.uniform_border_draws);
        fallback = std::max(fallback, stats.fallback_border_draws);
    }
    require(projected > 10 && uploads > 0 && uniform > 0 && fallback > 0, "complex CSS missed fast or fallback paths");
    for (unsigned frame = 0; frame < caseelephant::probe_constants::gallery::complex_css_border_frames; ++frame) {
        using Node = caseelephant::ProbeSceneNode;
        caseelephant::ProbeSceneFrame scene;
        auto& root = scene.root;
        root.tag = "div";
        root.identity = {3, 1, 1};
        root.box = {0, 0, caseelephant::probe_constants::gallery::complex_css_scene_width,
                    caseelephant::probe_constants::gallery::complex_css_scene_height};
        root.style = {{"background-color", "#050403"}, {"overflow", "hidden"}};
        Node group;
        group.tag = "div";
        group.identity = {3, 2, 1 + frame / 16};
        group.box = {28.25f, 22.5f, 60.1f + frame * .17f, 54.2f};
        group.style = {{"opacity", frame % 2 ? ".37" : ".73"},
                       {"background-color", "rgba(220,100,30,.27)"},
                       {"color", frame % 2 ? "#3399cc" : "#dd4477"},
                       {"transform", "rotate(" + std::to_string(int(frame % 7) * 13 - 39) + "deg) scale(" +
                                         (frame % 12 == 0 ? std::string("4.5,2.5") : std::string("1.1,.85")) + ")"},
                       {"transform-origin", "15% 75%"},
                       {"overflow", frame % 3 == 0 ? "hidden" : "visible"}};
        unsigned side{};
        for (const auto* edge : {"top", "right", "bottom", "left"}) {
            const auto key = std::string("border-") + edge;
            group.style[key + "-width"] = std::to_string(1.1f + float((frame + side) % 9) * .37f) + "px";
            group.style[key + "-color"] = side % 2 ? "currentcolor" : "rgba(212,168,67,.42)";
            group.style[key + "-style"] = frame % 11 == 0 && side == 2 ? "none" : "solid";
            ++side;
        }
        side = 0;
        for (const auto* corner : {"top-left", "top-right", "bottom-right", "bottom-left"}) {
            group.style[std::string("border-") + corner + "-radius"] =
                std::to_string(float((frame + side++ * 7) % 48)) + "px";
        }
        // Overlap and overflow defeat distributing group opacity or clipping
        // the group to its own border box. Keep both in the full oracle.
        Node red;
        red.tag = "div";
        red.identity = {3, 3, 1};
        red.box = {-11.5f, 16.25f, 54, 36};
        red.style = {{"background-color", "rgba(255,30,20,.6)"}};
        Node blue = red;
        blue.identity = {3, 4, 1};
        blue.box = {13.25f, 28.5f, 62, 42};
        blue.style = {{"background-color", "rgba(20,60,255,.7)"}, {"opacity", ".55"}, {"transform", "rotate(-17deg)"}};
        group.children = {red, blue};
        root.children.push_back(group);
        const float scale = frame % 4 == 0 ? 1.f : frame % 4 == 1 ? 1.5f : frame % 4 == 2 ? 2.f : 4.f;
        const auto expected = reference.raster(scene, scale);
        auto& image = surfaces.paint(0, scene, scale);
        surfaces.publish();
        FramePacket packet;
        packet.draws.push_back({{0, 0, float(image.width), float(image.height)}, {1, 1, 1, 1}, 0});
        std::array<bool, caseelephant::probe_constants::gallery::core_scene_count> visible{};
        visible[0] = true;
        const auto serial = pipeline.submit(std::move(packet),
                                            [] {
                                            },
                                            {}, {image.handle});
        surfaces.submitted(serial, visible);
        const auto actual = surfaces.read(0);
        const auto diff = scene_difference(actual, expected);
        worst_mean = std::max(worst_mean, *std::max_element(diff.mean.begin(), diff.mean.end()));
        minimum_iou = std::min(minimum_iou, diff.soft_iou);
        if (!diff.passes()) {
            std::cerr << "border geometry frame=" << frame << " IoU=" << diff.soft_iou << '\n';
            for (auto pair : {std::pair{"border-gpu", &actual}, std::pair{"border-software", &expected}}) {
                std::vector<Pixel> pixels(std::size_t(pair.second->width) * pair.second->height);
                std::memcpy(pixels.data(), pair.second->rgba.data(), pair.second->rgba.size());
                save_png(folder / (std::string(pair.first) + "-" + std::to_string(frame) + ".png"), pixels,
                         pair.second->width, pair.second->height);
            }
        }
        require(diff.passes(), "cached border geometry / overlapping opacity / overflow differs from full software");
        ++comparisons;
    }
    const auto before = surfaces.raster_work();
    for (unsigned frame = 0; frame < caseelephant::probe_constants::gallery::complex_css_angle_frames; ++frame) {
        stable.root.children[0].children[0].style["transform"] =
            "perspective(400px) rotateY(" + std::to_string(frame * 3) + "deg)";
        const auto expected = reference.raster(stable, caseelephant::probe_constants::gallery::scene_raster_scale);
        auto& image = surfaces.paint(0, stable, caseelephant::probe_constants::gallery::scene_raster_scale);
        surfaces.publish();
        FramePacket packet;
        packet.draws.push_back({{0, 0, float(image.width), float(image.height)}, {1, 1, 1, 1}, 0});
        std::array<bool, caseelephant::probe_constants::gallery::core_scene_count> visible{};
        visible[0] = true;
        const auto serial = pipeline.submit(std::move(packet),
                                            [] {
                                            },
                                            {}, {image.handle});
        surfaces.submitted(serial, visible);
        const auto diff = scene_difference(surfaces.read(0), expected);
        require(diff.passes(), "retained projective source differs after changing only angle");
        worst_mean = std::max(worst_mean, *std::max_element(diff.mean.begin(), diff.mean.end()));
        minimum_iou = std::min(minimum_iou, diff.soft_iou);
        ++comparisons;
    }
    const auto angles = surfaces.raster_work();
    const auto reuse_uploads = angles.gpu_plane_uploads - before.gpu_plane_uploads;
    require(angles.gpu_projective_draws - before.gpu_projective_draws ==
                    caseelephant::probe_constants::gallery::complex_css_angle_frames &&
                reuse_uploads <= 3,
            "angle-only motion re-uploaded immutable plane contents");
    caseelephant::ScriptDomProbe owner({caseelephant::probe_constants::gallery::vm_stack_bytes,
                                        caseelephant::probe_constants::gallery::scene_vm_execution_budget_ms});
    owner.eval(scene_read(CE_SOURCE_DIR "/probes/web_host_bootstrap.js"));
    owner.eval(
        "globalThis.root=document.getElementById('root');root.setAttribute('style','position:relative;overflow:hidden;background:#050403');globalThis.g=document.createElement('div');g.setAttribute('style','position:absolute;left:24.25px;top:17.5px;width:100px;height:90px;overflow:visible;border-radius:13px;border-width:3px;border-color:currentcolor;color:#dd4455;background:rgba(20,60,100,.3);transform-origin:15% 75%;animation:motion 1s linear infinite alternate;transition-property:background-color,color;transition-duration:200ms;transition-timing-function:linear');root.appendChild(g);globalThis.a=document.createElement('div');a.setAttribute('style','position:absolute;left:-11px;top:14px;width:68px;height:42px;background:rgba(255,30,20,.6);z-index:1');globalThis.b=document.createElement('div');b.setAttribute('style','position:absolute;left:25px;top:30px;width:68px;height:45px;background:rgba(20,60,255,.7);z-index:2');g.appendChild(a);g.appendChild(b)");
    const std::string timeline =
        "@keyframes motion{from{transform:translate(0,0) rotate(-25deg) scale(.8,1.1);opacity:.31}to{transform:translate(18%,9px) rotate(33deg) scale(1.1,.85);opacity:.79}}";
    caseelephant::SceneProbe typed("", timeline, [&](auto text, const auto& style) {
        return reference.measure(text, style);
    });
    caseelephant::SceneProbe full(
        "", timeline,
        [&](auto text, const auto& style) {
            return reference.measure(text, style);
        },
        false);
    for (unsigned frame = 0; frame < caseelephant::probe_constants::gallery::complex_css_typed_frames; ++frame) {
        if (frame % 13 == 3) {
            owner.eval("g.style.color='" + std::string(frame % 26 < 13 ? "#224466" : "#ff8844") +
                       "';g.style.backgroundColor='" +
                       std::string(frame % 26 < 13 ? "rgba(200,40,80,.6)" : "rgba(20,120,240,.3)") + "'");
        }
        if (frame % 17 == 4) {
            owner.eval("a.style.zIndex='" + std::to_string(frame % 34 < 17 ? 3 : 1) + "'");
        }
        if (frame % 19 == 5) {
            owner.eval("g.style.borderLeftWidth='" + std::to_string(frame % 38 < 19 ? 6 : 3) +
                       "px';g.style.overflow='" + std::string(frame % 38 < 19 ? "hidden" : "visible") + "'");
        }
        if (frame % 23 == 7) {
            owner.eval("g.style.width='" + std::to_string(frame % 46 < 23 ? 110 : 100) + "px';g.style.borderRadius='" +
                       std::to_string(frame % 46 < 23 ? 21 : 13) + "px'");
        }
        if (frame == 60) {
            owner.eval("g.removeChild(b)");
        }
        if (frame == 62) {
            owner.eval("g.appendChild(b)");
        }
        if (frame == 70) {
            owner.eval("g.style.transform='matrix(1,.1,-.2,1,3,5)';g.style.animation='none'");
        }
        if (frame == 75) {
            owner.eval("g.style.transform='translateY(12%) scaleX(.8) scaleY(1.1) rotate(.25)'");
        }
        if (frame == 80) {
            owner.eval(
                "g.style.transform='none';g.style.transitionProperty='transform';g.style.transitionDuration='250ms'");
        }
        if (frame == 81) {
            owner.eval("g.style.transform='rotate(35deg)'");
        }
        if (frame == 85) {
            owner.eval("g.style.transform='rotate(-15deg)'");
        }
        if (frame == 90) {
            owner.eval("g.style.animation='motion 1s linear infinite alternate'");
        }
        auto dom = owner.snapshot_subtree("#root");
        auto scene = typed.frame(dom, caseelephant::probe_constants::gallery::complex_css_scene_width,
                                 caseelephant::probe_constants::gallery::complex_css_scene_height,
                                 frame * caseelephant::probe_constants::gallery::complex_css_typed_frame_ms);
        auto oracle = full.frame(dom, caseelephant::probe_constants::gallery::complex_css_scene_width,
                                 caseelephant::probe_constants::gallery::complex_css_scene_height,
                                 frame * caseelephant::probe_constants::gallery::complex_css_typed_frame_ms);
        require(scene.root == oracle.root, "typed sampling changed reference style or layout");
        const float scale = frame % 3 == 0 ? 1.f : frame % 3 == 1 ? 1.5f : 2.f;
        const auto expected = reference.raster(oracle, scale);
        auto& image = surfaces.paint(0, scene, scale);
        surfaces.publish();
        FramePacket packet;
        packet.draws.push_back({{0, 0, float(image.width), float(image.height)}, {1, 1, 1, 1}, 0});
        std::array<bool, caseelephant::probe_constants::gallery::core_scene_count> visible{};
        visible[0] = true;
        const auto serial = pipeline.submit(std::move(packet),
                                            [] {
                                            },
                                            {}, {image.handle});
        surfaces.submitted(serial, visible);
        const auto diff = scene_difference(surfaces.read(0), expected);
        if (!diff.passes()) {
            std::cerr << "typed frame=" << frame << " IoU=" << diff.soft_iou << '\n';
        }
        require(diff.passes(), "typed paint / retained commands differ from full software");
        worst_mean = std::max(worst_mean, *std::max_element(diff.mean.begin(), diff.mean.end()));
        minimum_iou = std::min(minimum_iou, diff.soft_iou);
        ++comparisons;
    }
    // More programs than one painter can retain. Eviction during recursion
    // must not invalidate the parent replay or borrow any child node pointer.
    for (unsigned frame = 0; frame < caseelephant::probe_constants::gallery::complex_css_capacity_frames; ++frame) {
        caseelephant::ProbeSceneFrame scene;
        scene.root.identity = {99, 1, 1};
        scene.root.tag = "div";
        scene.root.box = {0, 0, caseelephant::probe_constants::gallery::complex_css_scene_width,
                          caseelephant::probe_constants::gallery::complex_css_scene_height};
        scene.root.style = {{"background-color", "#050403"}, {"opacity", ".7"}, {"overflow", "hidden"}};
        for (unsigned i = 0; i < caseelephant::probe_constants::gallery::complex_css_capacity_children; ++i) {
            caseelephant::ProbeSceneNode child;
            child.identity = {1000 + i, 1 + frame / 2, 99};
            child.tag = "div";
            child.box = {float(i % 30) * 5, float(i / 30) * 10, 8, 13};
            child.style = {{"background-color", i % 2 ? "rgba(200,80,40,.6)" : "rgba(20,90,200,.7)"},
                           {"z-index", std::to_string(i % 7)}};
            child.paint = caseelephant::compile_scene_paint_values(child.style);
            scene.root.children.push_back(std::move(child));
        }
        if (frame % 2) {
            std::reverse(scene.root.children.begin(), scene.root.children.end());
        }
        const auto expected = reference.raster(scene, caseelephant::probe_constants::gallery::scene_raster_scale);
        auto& image = surfaces.paint(0, scene, caseelephant::probe_constants::gallery::scene_raster_scale);
        surfaces.publish();
        FramePacket packet;
        packet.draws.push_back({{0, 0, float(image.width), float(image.height)}, {1, 1, 1, 1}, 0});
        std::array<bool, caseelephant::probe_constants::gallery::core_scene_count> visible{};
        visible[0] = true;
        const auto serial = pipeline.submit(std::move(packet),
                                            [] {
                                            },
                                            {}, {image.handle});
        surfaces.submitted(serial, visible);
        const auto diff = scene_difference(surfaces.read(0), expected);
        require(diff.passes(), "paint program capacity eviction/order/generation changed pixels");
        worst_mean = std::max(worst_mean, *std::max_element(diff.mean.begin(), diff.mean.end()));
        minimum_iou = std::min(minimum_iou, diff.soft_iou);
        ++comparisons;
    }
    const auto after = surfaces.raster_work();
    require(typed.work().typed_samples > 100 && after.typed_draws > 100 && after.paint_program_reuses > 100 &&
                after.paint_program_builds > 100 && after.paint_program_evictions > 0,
            "typed/retained paths not exercised");
    surfaces.release();
    gpu.evict_assets(0, true);
    gpu.check_debug_messages();
    require(after.border_geometry_builds > 256 && after.border_geometry_reuses > 24,
            "border geometry eviction/reuse paths not exercised");
    require(pipeline.completions == comparisons && surfaces.retired == comparisons,
            "complex CSS GPU lifetime imbalance");
    write_text(
        folder / "report.json",
        "{\"format\":\"complex-css-reference-v2\",\"comparisons\":" + std::to_string(comparisons) +
            ",\"worst_channel_mean\":" + format_host_time(worst_mean) + ",\"minimum_soft_iou\":" +
            format_host_time(minimum_iou) + ",\"gpu_projective_draws_per_painter_max\":" + std::to_string(projected) +
            ",\"gpu_plane_uploads_per_painter_max\":" + std::to_string(uploads) +
            ",\"uniform_border_draws_per_painter_max\":" + std::to_string(uniform) +
            ",\"fallback_border_draws_per_painter_max\":" + std::to_string(fallback) +
            ",\"angle_only_draws\":24,\"angle_only_uploads\":" + std::to_string(reuse_uploads) +
            ",\"border_mutations\":96,\"border_geometry_builds\":" + std::to_string(after.border_geometry_builds) +
            ",\"border_geometry_reuses\":" + std::to_string(after.border_geometry_reuses) +
            ",\"typed_frames\":128,\"program_capacity_frames\":4,\"paint_program_evictions\":" +
            std::to_string(after.paint_program_evictions) +
            ",\"paint_program_builds\":" + std::to_string(after.paint_program_builds) + ",\"paint_program_reuses\":" +
            std::to_string(after.paint_program_reuses) + ",\"typed_draws\":" + std::to_string(after.typed_draws) +
            ",\"retired_imports\":" + std::to_string(surfaces.retired) +
            ",\"debug_warnings\":0,\"software_target_switches\":8}");
    std::cout
        << "PASS complex CSS: 316 projective/border/typed mutations, software reference, target switches, actual retirement\n";
    return 0;
}
