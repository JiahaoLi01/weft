#include "gallery_cluster_input.hpp"
#include "gallery_scene_manifest.hpp"
#include "gallery_scene_session.hpp"
#include "particle_benchmark.hpp"
#include "probe_common.hpp"
#include "probe_modes.hpp"
#include "sampling_profiler.hpp"

#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <optional>
#include <sstream>

// The gallery interactions step 12 of docs/refactoring-plan.md speeds up, as passes on both backends: the K-line card
// swept by the pointer without a button (kline-sweep) and the scatter plot dragged (scatter-drag). Each card runs 2 s
// of its clock first; then each frame moves the pointer along the gallery live benchmark's path for that scenario, as
// far as an update of the K-line card in the gallery window goes (path_step_s), and advances the clock 1/60 s, untimed,
// and the card's frame is drawn at scale 2 into a 3840 x 2160 target (the scatter plot's lower part lies past it) with
// the particle benchmarks' loops (particle_benchmark.hpp): CPU from the frame to its submitted pass, GPU by timestamps.
// D3D11 on the adapter asked for, and Vulkan on this machine's device where built. Sampled, the passes' stacks are sampled
// from the sampled frames' passes on (their GPU waits included) into render-samples-<backend>.txt.
namespace probes {
    namespace {
        using namespace particle_benchmark;
        constexpr double frame_ms = 1000.0 / 60;
        constexpr double settle_ms = 2000;
        constexpr unsigned warmup_frames = 30;
        constexpr unsigned sample_frames = 240;
        constexpr unsigned warp_warmup_frames = 1;
        constexpr unsigned warp_sample_frames = 4;
        // Seconds of the pointer's path per frame: the gallery window updates the swept K-line card about 137 times a
        // second. The paths repeat each second, which no whole number of steps spans within a run, so no frame shows
        // what an earlier one showed (whose images the caches would still hold).
        constexpr double path_step_s = .00731;

        struct Scenario {
            const char* name;
            int scene;
        };

        constexpr Scenario scenarios[]{{"kline-sweep", weft::probe_constants::gallery::kline_scene},
                                       {"scatter-drag", weft::probe_constants::gallery::scatter_scene}};

        // A session per scenario, at its card's size.
        std::vector<std::unique_ptr<GallerySceneSession>> sessions() {
            std::vector<std::unique_ptr<GallerySceneSession>> out;
            for (const auto& s : scenarios) {
                const auto& d = gallery_scene_descriptors[s.scene];
                auto session =
                    std::make_unique<GallerySceneSession>(d.group, d.index, d.width, d.height, "cluster-utilities.css");
                gallery_cluster_prepare(*session, std::string(d.group) + "-" + std::to_string(d.index));
                session->set_profiling(false);
                out.push_back(std::move(session));
            }
            return out;
        }

        Source source(std::vector<std::unique_ptr<GallerySceneSession>>& cards, SamplingProfiler* sampler,
                      unsigned warmup) {
            return [&cards, sampler, warmup](std::size_t level, unsigned index) -> const weft::SceneFrame& {
                if (sampler) {
                    sampler->active(false);
                }
                auto& s = *cards[level];
                if (index == 0) {
                    const double until = s.time + settle_ms;
                    while (s.time + frame_ms <= until) {
                        s.step(s.time + frame_ms);
                    }
                }
                // The live benchmark's paths (gallery_preview_probe.cpp), u seconds in: a Lissajous sweep across the
                // K-line's candles, or the scatter plot pressed at its anchor and dragged around it.
                const double u = index * path_step_s;
                const double turn = 2 * std::numbers::pi;
                if (level == 0) {
                    s.input_value(weft::MouseType::Move, float(800 + 560 * std::sin(u * turn * 2)),
                                  float(400 + 360 * std::sin(u * turn * 3 + std::numbers::pi / 4)));
                } else {
                    s.input_value(index ? weft::MouseType::Move : weft::MouseType::Down,
                                  float(320 + 60 * std::sin(u * turn)), float(360 + 30 * std::sin(u * turn * 4 / 3)),
                                  1);
                }
                s.step(s.time + frame_ms);
                require(!s.vm.stats().listener_exceptions, "interaction listener exception");
                if (sampler) {
                    sampler->active(index >= warmup);
                }
                return s.current;
            };
        }

        std::string report(const char* backend, const std::string& adapter, const std::vector<Timings>& timings) {
            std::ostringstream out;
            out << "{\"backend\":" << json_string(backend) << ",\"adapter\":" << json_string(adapter)
                << ",\"scenarios\":[";
            for (std::size_t i = 0; i < timings.size(); ++i) {
                const auto& t = timings[i];
                out << (i ? "," : "") << "{\"scenario\":" << json_string(scenarios[i].name)
                    << ",\"render_cpu_ms\":" << stats(t.cpu) << ",\"gpu_ms\":" << stats(t.gpu)
                    << ",\"first_frame_ms\":{\"render_cpu\":" << std::fixed << std::setprecision(3) << t.first[1]
                    << ",\"gpu\":" << t.first[0] << "},\"per_frame\":{\"draws\":" << t.draws
                    << ",\"text_builds\":" << t.text_builds << ",\"path_builds\":" << t.path_builds << "}}";
                std::cout << backend << ' ' << std::setw(12) << scenarios[i].name << ": render CPU p50 " << std::fixed
                          << std::setprecision(3) << percentile(t.cpu, .5) << " p95 " << percentile(t.cpu, .95)
                          << " p99 " << percentile(t.cpu, .99) << " max " << percentile(t.cpu, 1) << " ms; GPU p50 "
                          << percentile(t.gpu, .5) << " p99 " << percentile(t.gpu, .99) << " ms; per frame "
                          << std::setprecision(1) << t.draws << " draws, " << t.text_builds << " text and "
                          << t.path_builds << " path builds; first frame render " << std::setprecision(3) << t.first[1]
                          << " GPU " << t.first[0] << " ms\n";
            }
            return out.str() + "]}";
        }
    } // namespace

    int run_scene_interaction_benchmark(bool warp, const std::filesystem::path& folder, bool sampled) {
        std::filesystem::create_directories(folder);
        // WARP checks that the benchmark runs; the hardware run measures.
        const unsigned warmup = warp ? warp_warmup_frames : warmup_frames;
        const unsigned samples = warp ? warp_sample_frames : sample_frames;
        const auto period = std::chrono::microseconds(weft::probe_constants::gallery::owner_sample_period_us);
        std::optional<SamplingProfiler> sampler;
        if (sampled) {
            sampler.emplace(period);
        }
        auto direct_cards = sessions();
        std::string adapter;
        const auto direct =
            particle_benchmark::d3d11(warp, std::size(scenarios), warmup, samples,
                                      source(direct_cards, sampler ? &*sampler : nullptr, warmup), adapter);
        if (sampler) {
            sampler->write(folder / "render-samples-d3d11.txt");
        }
        std::string out = "{\"format\":\"weft-scene-interaction-benchmark-v1\",\"target\":[3840,2160],\"scale\":2,"
                          "\"settle_ms\":2000,\"warmup_frames\":" +
                          std::to_string(warmup) + ",\"sample_frames\":" + std::to_string(samples) +
#ifdef NDEBUG
                          ",\"build\":\"Release\"" +
#else
                          ",\"build\":\"Debug\"" +
#endif
                          ",\"passes\":[" + report("D3D11", adapter, direct);
#ifdef WEFT_PROBE_VULKAN
        if (!warp) {
            auto vulkan_cards = sessions();
            std::string vulkan_adapter;
            std::optional<SamplingProfiler> vulkan_sampler;
            if (sampled) {
                vulkan_sampler.emplace(period);
            }
            const auto timings = particle_benchmark::vulkan(
                std::size(scenarios), warmup, samples,
                source(vulkan_cards, vulkan_sampler ? &*vulkan_sampler : nullptr, warmup), vulkan_adapter);
            if (vulkan_sampler) {
                vulkan_sampler->write(folder / "render-samples-vulkan.txt");
            }
            if (!timings.empty()) {
                out += "," + report("Vulkan", vulkan_adapter, timings);
            }
        }
#endif
        PROCESS_MEMORY_COUNTERS_EX memory{};
        require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                                     sizeof(memory)),
                "process memory");
        std::cout << "process peak working set " << memory.PeakWorkingSetSize / (1024 * 1024) << " MiB\n";
        write_text(folder / "scene-interaction.json",
                   out + "],\"process_peak_working_set_bytes\":" + std::to_string(memory.PeakWorkingSetSize) + "}");
        std::cout << "PASS scene interaction benchmark: " << std::size(scenarios) << " scenarios at 3840 x 2160, "
                  << samples << " frames each\n";
        return 0;
    }
} // namespace probes
