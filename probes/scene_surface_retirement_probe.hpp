#pragma once
#include "gallery_constants.hpp"

// The old scroll policy retained all 34 small cards, then exhausted the
// unchanged 512 MiB import budget on a subsequent 2x Sunburst version.
inline int run_scene_surface_retirement(bool warp, const std::filesystem::path& folder) {
    std::filesystem::create_directories(folder);
    std::string rejected;
    std::size_t negative_completions{};
    std::size_t submissions{};
    std::size_t completions{};
    std::size_t evictions{};
    std::size_t peak_slots{};
    std::uint64_t negative_bytes{};
    std::uint64_t peak_bytes{};
    auto frame = [](unsigned index) {
        auto f = std::make_shared<caseelephant::ProbeSceneFrame>();
        const auto d = gallery_scene_descriptors[index];
        f->root.identity = {1, index + 1, 1};
        f->root.tag = "div";
        f->root.box = {0, 0, d.width, d.height};
        f->root.style = {{"background-color", "#408020"}, {"overflow", "hidden"}};
        return f;
    };
    {
        Experiment gpu(warp, caseelephant::probe_constants::gallery::retirement_surface_size,
                       caseelephant::probe_constants::gallery::retirement_surface_size);
        gpu.cache_assets = true;
        TwoFramePipeline pipeline(gpu);
        SceneGpuSurfaces surfaces(gpu, pipeline);
        surfaces.native_paint = true;
        bool busy{};
        try {
            for (unsigned index = 0; index <= caseelephant::probe_constants::gallery::sunburst_scene; ++index) {
                pipeline.drain();
                for (unsigned version = 0; version < caseelephant::probe_constants::gallery::surface_versions_per_card;
                     ++version) {
                    std::array<bool, gallery_scene_count> visible{};
                    visible[index] = true;
                    auto& source = surfaces.paint_owned(index, frame(index));
                    surfaces.publish();
                    FramePacket packet;
                    packet.draws.push_back({{0, 0, caseelephant::probe_constants::gallery::retirement_surface_size,
                                             caseelephant::probe_constants::gallery::retirement_surface_size},
                                            {1, 1, 1, 1},
                                            0});
                    const auto serial = pipeline.submit(std::move(packet),
                                                        [] {
                                                        },
                                                        {}, {source.handle});
                    surfaces.submitted(serial, visible);
                }
            }
        } catch (const std::runtime_error& error) {
            rejected = error.what();
            busy = rejected.find("result=8 card=34") != std::string::npos;
        }
        require(busy, "old retained scroll policy did not reproduce Sunburst budget exhaustion");
        ce_render_external_info info{sizeof(info)};
        render_result(ce_renderer_get_external_info(gpu.ensure_backend().renderer, &info),
                      "negative scroll accounting");
        negative_bytes = info.texture_bytes;
        require(info.live_textures < caseelephant::probe_constants::gallery::external_texture_limit &&
                    negative_bytes <= caseelephant::probe_constants::gallery::external_import_budget_bytes,
                "negative scroll failed for an unexpected resource limit");
        surfaces.release();
        require(pipeline.submissions == pipeline.completions, "negative scroll lost actual completion");
        negative_completions = pipeline.completions;
        render_result(ce_renderer_get_external_info(gpu.ensure_backend().renderer, &info), "negative scroll cleanup");
        require(!info.live_textures && !info.texture_bytes, "failed producer import leaked leases");
        gpu.check_debug_messages();
    }
    {
        Experiment gpu(warp, caseelephant::probe_constants::gallery::retirement_surface_size,
                       caseelephant::probe_constants::gallery::retirement_surface_size);
        gpu.cache_assets = true;
        TwoFramePipeline pipeline(gpu);
        SceneGpuSurfaces surfaces(gpu, pipeline);
        surfaces.native_paint = true;
        // Advance through every scene and return to the largest card. Three
        // versions per visit stress both actual in-flight uses and re-entry.
        for (unsigned visit = 0; visit < gallery_scene_count + 2; ++visit) {
            const unsigned index = visit < gallery_scene_count ? visit
                                   : visit == gallery_scene_count
                                       ? caseelephant::probe_constants::gallery::sunburst_scene
                                       : 0;
            std::array<bool, gallery_scene_count> visible{};
            visible[index] = true;
            for (unsigned version = 0; version < caseelephant::probe_constants::gallery::surface_versions_per_card;
                 ++version) {
                pipeline.begin_frame_timing();
                surfaces.retire_hidden(visible);
                auto& source = surfaces.paint_owned(index, frame(index));
                surfaces.publish();
                FramePacket packet;
                packet.draws.push_back({{0, 0, caseelephant::probe_constants::gallery::retirement_surface_size,
                                         caseelephant::probe_constants::gallery::retirement_surface_size},
                                        {1, 1, 1, 1},
                                        0});
                const auto serial = pipeline.submit(std::move(packet),
                                                    [] {
                                                    },
                                                    {}, {source.handle});
                surfaces.submitted(serial, visible);
                // No notification has acknowledged the just-submitted version.
                std::array<bool, gallery_scene_count> hidden{};
                surfaces.retire_hidden(hidden);
                require(surfaces.image(index) == &source, "hidden collector retired an uncompleted version");
            }
            if (visit >= gallery_scene_count) {
                auto image = surfaces.read(index);
                for (auto [x, y] : std::array<std::pair<unsigned, unsigned>, 3>{
                         {{0, 0}, {image.width / 2, image.height / 2}, {image.width - 1, image.height - 1}}}) {
                    auto p = (std::size_t(y) * image.width + x) * 4;
                    require(image.rgba[p] == 64 && image.rgba[p + 1] == 128 && image.rgba[p + 2] == 32 &&
                                image.rgba[p + 3] == 255,
                            "scroll re-entry reused missing/stale pixels");
                }
            }
        }
        // A scroll boundary can expose portions of two full-size source cards.
        // Keep both visible through enough updates to populate their versions.
        std::array<bool, gallery_scene_count> pair{};
        pair[caseelephant::probe_constants::gallery::sunburst_scene] =
            pair[caseelephant::probe_constants::gallery::sunburst_scene + 1] = true;
        for (unsigned update = 0; update < 8; ++update) {
            pipeline.begin_frame_timing();
            surfaces.retire_hidden(pair);
            FramePacket packet;
            for (unsigned index : {static_cast<unsigned>(caseelephant::probe_constants::gallery::sunburst_scene),
                                   static_cast<unsigned>(caseelephant::probe_constants::gallery::sunburst_scene + 1)}) {
                surfaces.paint_owned(index, frame(index));
                packet.draws.push_back({{float((index - caseelephant::probe_constants::gallery::sunburst_scene) * 40),
                                         0, 40, caseelephant::probe_constants::gallery::retirement_surface_size},
                                        {1, 1, 1, 1},
                                        int(index - caseelephant::probe_constants::gallery::sunburst_scene)});
            }
            surfaces.publish();
            const std::vector<ce_render_handle> sources{
                surfaces.image(caseelephant::probe_constants::gallery::sunburst_scene)->handle,
                surfaces.image(caseelephant::probe_constants::gallery::sunburst_scene + 1)->handle};
            const auto serial = pipeline.submit(
                std::move(packet),
                [] {
                },
                {}, sources);
            surfaces.submitted(serial, pair);
        }
        std::array<bool, gallery_scene_count> hidden{};
        surfaces.retire_hidden(hidden);
        require(surfaces.image(caseelephant::probe_constants::gallery::sunburst_scene) &&
                    surfaces.image(caseelephant::probe_constants::gallery::sunburst_scene + 1),
                "visible pair retired before actual completion");
        pipeline.drain();
        surfaces.retire_hidden(hidden);
        require(!surfaces.live_slots(), "hidden completed backing survived collection");
        ce_render_external_info info{sizeof(info)};
        render_result(ce_renderer_get_external_info(gpu.ensure_backend().renderer, &info), "positive scroll cleanup");
        require(!info.live_textures && !info.texture_bytes, "positive scroll leaked imports");
        submissions = pipeline.submissions;
        completions = pipeline.completions;
        evictions = surfaces.hidden_retirements;
        peak_slots = surfaces.peak_live_slots;
        peak_bytes = surfaces.peak_imported_bytes;
        require(submissions == 206 && completions == 206 && peak_slots <= 6 &&
                    peak_bytes <= caseelephant::probe_constants::gallery::external_import_budget_bytes &&
                    evictions > 100,
                "scroll resource bound/completion coverage");
        surfaces.release();
        gpu.check_debug_messages();
    }
    // Repeated completed destruction must return the transparent-group
    // scratch reservation, not accumulate until otherwise valid cards fall
    // back after the 128 MiB budget. 140 x 1 MiB catches that accounting leak.
    {
        Experiment gpu(warp, caseelephant::probe_constants::gallery::retirement_surface_size,
                       caseelephant::probe_constants::gallery::retirement_surface_size);
        gpu.cache_assets = true;
        TwoFramePipeline pipeline(gpu);
        SceneGpuSurfaces surfaces(gpu, pipeline);
        surfaces.native_paint = true;
        auto grouped = frame(0);
        grouped->root.box = {0, 0, 256, 256};
        grouped->root.style = {{"opacity", "0.5"}, {"overflow", "hidden"}};
        auto child = grouped->root;
        child.identity.slot = 2;
        child.style = {{"background-color", "#408020"}};
        grouped->root.children.push_back(child);
        std::array<bool, gallery_scene_count> visible{};
        std::array<bool, gallery_scene_count> hidden{};
        visible[0] = true;
        for (unsigned visit = 0; visit < caseelephant::probe_constants::gallery::scratch_reentry_visits; ++visit) {
            auto& source = surfaces.paint_owned(0, grouped);
            require(source.last_native &&
                        surfaces.native_work().scratch_bytes() == caseelephant::constants::bytes_per_mib,
                    "transparent group backing missing/budget leaked");
            surfaces.publish();
            FramePacket packet;
            packet.draws.push_back({{0, 0, caseelephant::probe_constants::gallery::retirement_surface_size,
                                     caseelephant::probe_constants::gallery::retirement_surface_size},
                                    {1, 1, 1, 1},
                                    0});
            const auto serial = pipeline.submit(std::move(packet),
                                                [] {
                                                },
                                                {}, {source.handle});
            surfaces.submitted(serial, visible);
            surfaces.retire_hidden(hidden);
            require(surfaces.native_work().scratch_bytes() == caseelephant::constants::bytes_per_mib,
                    "uncompleted transparent group scratch retired");
            pipeline.drain();
            surfaces.retire_hidden(hidden);
            require(!surfaces.live_slots() && !surfaces.native_work().scratch_bytes(),
                    "completed transparent group scratch retained");
        }
        require(pipeline.submissions == caseelephant::probe_constants::gallery::scratch_reentry_visits &&
                    pipeline.completions == caseelephant::probe_constants::gallery::scratch_reentry_visits &&
                    surfaces.native_work().passes == caseelephant::probe_constants::gallery::scratch_reentry_visits &&
                    !surfaces.native_work().fallbacks,
                "transparent group retirement native coverage");
        surfaces.release();
        gpu.check_debug_messages();
    }
    write_text(
        folder / "report.json",
        "{\"format\":\"scene-surface-retirement-v1\",\"old_policy_rejection\":" + json_string(rejected) +
            ",\"old_policy_imported_bytes\":" + std::to_string(negative_bytes) +
            ",\"old_policy_completed_submissions\":" + std::to_string(negative_completions) +
            ",\"fixed_submissions\":" + std::to_string(submissions) + ",\"fixed_completions\":" +
            std::to_string(completions) + ",\"hidden_retirements\":" + std::to_string(evictions) +
            ",\"peak_live_slots\":" + std::to_string(peak_slots) +
            ",\"peak_imported_bytes\":" + std::to_string(peak_bytes) +
            ",\"group_scratch_reentry_submissions\":140,\"group_scratch_reentry_fallbacks\":0,\"group_scratch_bytes_after_retirement\":0,\"budget_bytes\":536870912,\"debug_warning_error_count\":0}");
    std::cout
        << "PASS reproduced old Sunburst budget rejection / 206 completed fixed submissions / two large visible cards / re-entry pixels / hidden actual retirement\n";
    return 0;
}
