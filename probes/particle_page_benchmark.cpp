#include "gallery_scene_options.hpp"
#include "particle_benchmark.hpp"
#include "probe_common.hpp"
#include "probe_modes.hpp"
#include "paint/particles.hpp"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>

// The gallery's UI particle page at 4K (step 11.4c of docs/refactoring-plan.md): the page (PARTICLE_TESTS.js) laid out
// in 1920 x 1080 CSS px and drawn at scale 2 into a 3840 x 2160 target, at densities of its drifting motes from the
// page's own 300 per second up; its campfire, rune and chest (which opens itself every 2.5 s) as they are. At each
// density the page first runs 6 s of its clock (the motes live up to 5 s), then each frame advances the clock 1/60 s:
// the owner's step (timers, rAF, the DOM snapshot and the scene update) is timed on the CPU, then the pass as
// particle_benchmark.hpp times it. D3D11 on the adapter asked for, and Vulkan on this machine's device where built.
namespace probes {
    namespace {
        using namespace particle_benchmark;
        constexpr float page_width = target_width / scale;
        constexpr float page_height = target_height / scale;
        constexpr double frame_ms = 1000.0 / 60;
        constexpr double settle_ms = 6000;
        constexpr unsigned warmup_frames = 30;
        constexpr unsigned sample_frames = 240;
        constexpr unsigned warp_warmup_frames = 1;
        constexpr unsigned warp_sample_frames = 4;

        std::uint64_t particle_count(const weft::SceneNode& n) {
            std::uint64_t count{};
            for (const auto& e : n.particles) {
                count += weft::paint::particle_timing({e.emitter->emitter, e.emission}).count;
            }
            for (const auto& c : n.children) {
                count += particle_count(c);
            }
            return count;
        }

        // The page at one backend: its owner's steps and particles per level, sampled frames only.
        struct Page {
            std::unique_ptr<GallerySceneSession> session;
            std::vector<std::vector<double>> owner;
            std::vector<std::vector<double>> particles;
            std::vector<double> first_owner;
        };

        Source page_source(Page& page, const std::vector<unsigned>& densities, unsigned warmup) {
            page.owner.assign(densities.size(), {});
            page.particles.assign(densities.size(), {});
            page.first_owner.assign(densities.size(), 0);
            return [&page, densities, warmup](std::size_t level, unsigned index) -> const weft::SceneFrame& {
                auto& s = *page.session;
                if (index == 0) {
                    s.vm.eval("__ceParticlePage.setDensity(" + std::to_string(densities[level]) + ")");
                    const double until = s.time + settle_ms;
                    while (s.time + frame_ms <= until) {
                        s.step(s.time + frame_ms);
                    }
                }
                const auto began = std::chrono::steady_clock::now();
                s.step(s.time + frame_ms);
                const double owner =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
                if (index == 0) {
                    page.first_owner[level] = owner;
                }
                if (index >= warmup) {
                    page.owner[level].push_back(owner);
                    page.particles[level].push_back(double(particle_count(s.current.root)));
                }
                return s.current;
            };
        }

        std::string report(const char* backend, const std::string& adapter, const std::vector<unsigned>& densities,
                           const Page& page, const std::vector<Timings>& timings) {
            std::ostringstream out;
            out << "{\"backend\":" << json_string(backend) << ",\"adapter\":" << json_string(adapter)
                << ",\"densities\":[";
            for (std::size_t i = 0; i < densities.size(); ++i) {
                const auto& p = page.particles[i];
                double mean{};
                for (const auto v : p) {
                    mean += v / double(p.size());
                }
                out << (i ? "," : "") << "{\"motes_per_second\":" << densities[i]
                    << ",\"particles\":{\"mean\":" << std::fixed << std::setprecision(0) << mean
                    << ",\"max\":" << percentile(p, 1) << "},\"owner_ms\":" << stats(page.owner[i])
                    << ",\"render_cpu_ms\":" << stats(timings[i].cpu) << ",\"gpu_ms\":" << stats(timings[i].gpu)
                    << ",\"first_frame_ms\":{\"owner\":" << std::setprecision(3) << page.first_owner[i]
                    << ",\"render_cpu\":" << timings[i].first[1] << ",\"gpu\":" << timings[i].first[0] << "}}";
                std::cout << backend << ' ' << std::setw(6) << densities[i] << " motes/s, " << std::fixed
                          << std::setprecision(0) << mean << " particles: owner p50 " << std::setprecision(3)
                          << percentile(page.owner[i], .5) << " p99 " << percentile(page.owner[i], .99) << " max "
                          << percentile(page.owner[i], 1) << " ms; render CPU p50 " << percentile(timings[i].cpu, .5)
                          << " p99 " << percentile(timings[i].cpu, .99) << " ms; GPU p50 "
                          << percentile(timings[i].gpu, .5) << " p99 " << percentile(timings[i].gpu, .99) << " max "
                          << percentile(timings[i].gpu, 1) << " ms; first frame owner " << page.first_owner[i]
                          << " render " << timings[i].first[1] << " GPU " << timings[i].first[0] << " ms\n";
            }
            return out.str() + "]}";
        }

        std::unique_ptr<GallerySceneSession> page_session() {
            return std::make_unique<GallerySceneSession>("PARTICLE_TESTS", 0, page_width, page_height,
                                                         gallery_particle_page_options());
        }
    } // namespace

    int run_particle_page_benchmark(bool warp, const std::filesystem::path& folder) {
        std::filesystem::create_directories(folder);
        // WARP checks that the benchmark runs; the hardware run measures.
        const std::vector<unsigned> densities =
            warp ? std::vector<unsigned>{300} : std::vector<unsigned>{300, 3000, 12000, 24000, 48000};
        const unsigned warmup = warp ? warp_warmup_frames : warmup_frames;
        const unsigned samples = warp ? warp_sample_frames : sample_frames;
        Page direct_page{page_session()};
        direct_page.session->set_profiling(false);
        std::string adapter;
        const auto direct = particle_benchmark::d3d11(warp, densities.size(), warmup, samples,
                                                      page_source(direct_page, densities, warmup), adapter);
        std::string out = "{\"format\":\"weft-particle-page-benchmark-v1\",\"page\":[1920,1080],\"target\":[3840,"
                          "2160],\"scale\":2,\"settle_ms\":6000,\"warmup_frames\":" +
                          std::to_string(warmup) + ",\"sample_frames\":" + std::to_string(samples) +
#ifdef NDEBUG
                          ",\"build\":\"Release\"" +
#else
                          ",\"build\":\"Debug\"" +
#endif
                          ",\"passes\":[" + report("D3D11", adapter, densities, direct_page, direct);
        require(direct_page.session->vm.eval("__ceGallery.state().errors.length") == "0", "particle page errors");
#ifdef WEFT_PROBE_VULKAN
        if (!warp) {
            Page vulkan_page{page_session()};
            vulkan_page.session->set_profiling(false);
            std::string vulkan_adapter;
            const auto timings = particle_benchmark::vulkan(
                densities.size(), warmup, samples, page_source(vulkan_page, densities, warmup), vulkan_adapter);
            if (!timings.empty()) {
                out += "," + report("Vulkan", vulkan_adapter, densities, vulkan_page, timings);
            }
        }
#endif
        PROCESS_MEMORY_COUNTERS_EX memory{};
        require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                                     sizeof(memory)),
                "process memory");
        std::cout << "process peak working set " << memory.PeakWorkingSetSize / (1024 * 1024) << " MiB\n";
        write_text(folder / "particle-page.json",
                   out + "],\"process_peak_working_set_bytes\":" + std::to_string(memory.PeakWorkingSetSize) + "}");
        std::cout << "PASS particle page benchmark: " << densities.size() << " densities at 3840 x 2160, " << samples
                  << " frames each\n";
        return 0;
    }
} // namespace probes
