#include "gallery_interaction_profile.hpp"
#include "gallery_scene_manifest.hpp"
#include "gallery_scene_session.hpp"
#include "gallery_cluster_input.hpp"
#include "probe_modes.hpp"
#include "sampling_profiler.hpp"

#include <numbers>
#include <optional>

namespace probes {
    int run_gallery_interaction_profile(const std::filesystem::path& folder, const std::string& filter, bool sample) {
        std::filesystem::create_directories(folder);
        std::optional<SamplingProfiler> sampler;
        if (sample) {
            sampler.emplace(std::chrono::microseconds(weft::probe_constants::gallery::owner_sample_period_us));
        }
        std::ofstream rows(folder / "frames.jsonl");
        std::ofstream trace(folder / "trace.json");
        trace << "{\"traceEvents\":[";
        bool comma{};
        const auto epoch = std::chrono::steady_clock::now();
        auto elapsed = [](auto start) {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        };
        auto event = [&](const std::string& name, auto start, double duration) {
            if (comma) {
                trace << ',';
            }
            comma = true;
            trace << "{\"ph\":\"X\",\"pid\":1,\"tid\":1,\"name\":" << json_string(name)
                  << ",\"ts\":" << std::chrono::duration<double, std::micro>(start - epoch).count()
                  << ",\"dur\":" << duration * weft::probe_constants::gallery::microseconds_per_millisecond << '}';
        };
        auto stats = [](std::vector<double> v) {
            std::sort(v.begin(), v.end());
            auto q = [&](double fraction) {
                return v.empty() ? 0 : v[std::min(v.size() - 1, std::size_t(fraction * (v.size() - 1)))];
            };
            return "{\"samples\":" + std::to_string(v.size()) + ",\"p50\":" + format_host_time(q(.5)) +
                   ",\"p95\":" + format_host_time(q(.95)) + ",\"p99\":" + format_host_time(q(.99)) +
                   ",\"max\":" + format_host_time(q(1)) + "}";
        };
        std::ostringstream report;
        report
            << "{\"format\":\"gallery-owner-interaction-profile-v1\",\"clock_hz\":60,\"frames_per_case\":240,\"warmup\":16,\"gpu_included\":false,\"cases\":[";
        unsigned count{};
        for (unsigned index = weft::probe_constants::gallery::core_scene_count;
             index < unsigned(weft::probe_constants::gallery::cluster_scene_end); ++index) {
            const auto d = gallery_scene_descriptors[index];
            const bool scatter = index == weft::probe_constants::gallery::scatter_scene;
            const bool kline = index == weft::probe_constants::gallery::kline_scene;
            const bool spring = d.group == std::string("SPRING_TESTS");
            std::string category = scatter                                  ? "scatter"
                                   : kline                                  ? "kline"
                                   : d.group == std::string("FRAMER_TESTS") ? "framer"
                                   : spring                                 ? "spring"
                                   : d.group == std::string("ANIME_TESTS")  ? "anime"
                                                                            : "other";
            // Sustained drags: press on the draggable, move every frame for two seconds, release.
            const char* drag_anchor = index == 36   ? ".overlay"
                                      : index == 37 ? "svg"
                                      : index == 58 ? ".drag-item"
                                      : index == 59 ? ".snap-box"
                                      : index == 60 ? ".h-drag"
                                      : index == 61 ? ".throw-ball"
                                                    : nullptr;
            const bool drag = filter == "drag";
            if (drag ? !drag_anchor : category == "other" || (filter != "all" && filter != category)) {
                continue;
            }
            const auto id = std::string(d.group == std::string("ANIME_TESTS") ? "anime"
                                        : scatter || drag                     ? "d3_interactive"
                                                                              : category) +
                            "-" + (d.index < 10 ? "0" : "") + std::to_string(d.index);
            float drag_x{};
            float drag_y{};
            std::cout << id << " profile..." << std::flush;
            GallerySceneSession s(d.group, d.index, d.width, d.height, "cluster-utilities.css");
            gallery_cluster_prepare(s, id);
            s.profile = {};
            std::vector<double> ticks;
            std::vector<double> inputs;
            std::vector<double> steps;
            unsigned input_count{};
            unsigned stalls{};
            for (unsigned frame = 0; frame < weft::probe_constants::gallery::fixed_clock_run_frames; ++frame) {
                if (sampler) {
                    sampler->active(frame >= weft::probe_constants::gallery::warmup_frames);
                }
                auto start = std::chrono::steady_clock::now();
                s.time = frame * weft::probe_constants::gallery::fixed_clock_step_ms;
                auto before = s.profile;
                auto scene_before = s.scene.work();
                auto js_before = s.vm.stats();
                auto input_start = std::chrono::steady_clock::now();
                unsigned applied{};
                if (drag && frame >= 24 && frame <= 144) {
                    if (frame == 24) {
                        std::istringstream(
                            s.vm.eval(std::string("(()=>{const r=document.querySelector('") + drag_anchor +
                                      "').getBoundingClientRect();return (r.x+r.width/2)+' '+(r.y+r.height/2)})()")) >>
                            drag_x >> drag_y;
                    }
                    const double t = frame - 24;
                    s.input_value(frame == 24    ? weft::MouseType::Down
                                  : frame == 144 ? weft::MouseType::Up
                                                 : weft::MouseType::Move,
                                  drag_x + float(60 * std::sin(t * 2 * std::numbers::pi / 60)),
                                  drag_y + float(30 * std::sin(t * 2 * std::numbers::pi / 45)), frame == 144 ? 0 : 1);
                    applied = 1;
                }
                if (!drag && scatter && frame >= 24 && frame <= 143) {
                    s.input_value(frame == 24    ? weft::MouseType::Down
                                  : frame == 143 ? weft::MouseType::Up
                                                 : weft::MouseType::Move,
                                  320 + float(frame - 24) * .8f, 360 + float(frame % 17) * .3f, frame == 143 ? 0 : 1);
                    applied = 1;
                }
                if (!drag && scatter && (frame == 160 || frame == 200)) {
                    s.input_value(weft::MouseType::Move, 600, 400, 0, -100);
                    applied = 1;
                }
                if (!drag && kline && frame >= 24 && frame < 144) {
                    s.input_value(weft::MouseType::Move, 180 + float(frame - 24) * 8, 250 + float(frame % 11));
                    applied = 1;
                }
                if (!drag && !scatter && !kline && !spring) {
                    applied = gallery_cluster_inputs(s, frame);
                }
                auto input_ms = elapsed(input_start);
                if (applied) {
                    input_count += applied;
                    inputs.push_back(input_ms);
                    event(id + " input", input_start, input_ms);
                }
                auto step_start = std::chrono::steady_clock::now();
                s.step(s.time);
                auto step_ms = elapsed(step_start);
                if (sampler) {
                    sampler->active(false);
                }
                auto total = elapsed(start);
                event(id + " update", step_start, step_ms);
                if (frame >= weft::probe_constants::gallery::warmup_frames) {
                    ticks.push_back(total);
                    steps.push_back(step_ms);
                    stalls += total > weft::probe_constants::gallery::stall_threshold_ms;
                }
                auto after = s.vm.stats();
                auto scene_after = s.scene.work();
                rows << "{\"id\":" << json_string(id) << ",\"frame\":" << frame << ",\"input_count\":" << applied
                     << ",\"total_ms\":" << total << ",\"input_ms\":" << input_ms << ",\"step_ms\":" << step_ms
                     << ",\"layout_reads\":" << s.profile.layout_reads - before.layout_reads
                     << ",\"style_reads\":" << s.profile.style_reads - before.style_reads
                     << ",\"layout_read_ms\":" << s.profile.layout_ms - before.layout_ms
                     << ",\"style_read_ms\":" << s.profile.style_ms - before.style_ms
                     << ",\"flush_ms\":" << s.profile.flush_ms - before.flush_ms
                     << ",\"hit_ms\":" << s.profile.hit_ms - before.hit_ms
                     << ",\"dispatch_ms\":" << s.profile.dispatch_ms - before.dispatch_ms
                     << ",\"style_ms\":" << scene_after.style_ms - scene_before.style_ms
                     << ",\"build_ms\":" << scene_after.build_ms - scene_before.build_ms
                     << ",\"layout_ms\":" << scene_after.layout_ms - scene_before.layout_ms
                     << ",\"scene_build_ms\":" << s.profile.scene_build_ms - before.scene_build_ms
                     << ",\"geometry_ms\":" << s.profile.geometry_ms - before.geometry_ms
                     << ",\"snapshot_nodes\":" << after.snapshot_nodes - js_before.snapshot_nodes << "}\n";
            }
            require(!s.vm.stats().listener_exceptions, "profile listener errors");
            auto state = s.vm.eval("JSON.stringify(__ceGallery.state())");
            s.unmount();
            require(s.vm.stats().live_nodes == weft::probe_constants::gallery::unmounted_live_nodes,
                    "profile unmount leak");
            if (count++) {
                report << ',';
            }
            report << "{\"id\":" << json_string(id) << ",\"input_count\":" << input_count
                   << ",\"warm_ticks_ms\":" << stats(ticks) << ",\"update_ms\":" << stats(steps)
                   << ",\"input_ms\":" << stats(inputs) << ",\"ticks_over_16_67_ms\":" << stalls
                   << ",\"layout_reads\":" << s.profile.layout_reads << ",\"style_reads\":" << s.profile.style_reads
                   << ",\"layout_read_ms\":" << s.profile.layout_ms << ",\"style_read_ms\":" << s.profile.style_ms
                   << ",\"hit_ms\":" << s.profile.hit_ms << ",\"dispatch_ms\":" << s.profile.dispatch_ms
                   << ",\"final_state\":" << state << '}';
            write_text(folder / "progress.json",
                       "{\"completed_cases\":" + std::to_string(count) + ",\"last_case\":" + json_string(id) + "}");
            rows.flush();
            trace.flush();
            std::cout << " " << stats(ticks) << "\n" << std::flush;
        }
        trace << "]}";
        report << "],\"case_count\":" << count << '}';
        write_text(folder / "report.json", report.str());
        require(count > 0, "profile scenario empty");
        if (sampler) {
            std::cout << sampler->write(folder / "samples.txt") << " owner stack samples / samples.txt\n";
        }
        std::cout << "PASS owner interaction profile / " << count << " original cases / trace.json\n";
        return 0;
    }
} // namespace probes
