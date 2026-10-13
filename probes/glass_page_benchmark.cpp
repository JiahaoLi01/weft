#include "gallery_scene_options.hpp"
#include "particle_benchmark.hpp"
#include "probe_common.hpp"
#include "probe_modes.hpp"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>

// The gallery's translucent window page at 4K (step 11.5c of docs/refactoring-plan.md): each case (frosted, plain
// translucent, blur only) laid out in 1920 x 1080 CSS px and drawn at scale 2 into a 3840 x 2160 target, with from one
// to 32 of its windows stacked over its background, which moves every frame, so every frame copies and filters what
// lies behind each glass window again. At each count the page first runs 1 s of its clock, then each frame advances it
// 1/60 s: the owner's step (timers, rAF, the DOM snapshot and the scene update) is timed on the CPU, then the pass with
// the particle benchmarks' loops (particle_benchmark.hpp): CPU from the frame to its submitted pass, GPU by timestamps.
// D3D11 on the adapter asked for, and Vulkan on this machine's device where built.
namespace probes {
    namespace {
        using namespace particle_benchmark;
        constexpr float page_width = target_width / scale;
        constexpr float page_height = target_height / scale;
        constexpr double frame_ms = 1000.0 / 60;
        constexpr double settle_ms = 1000;
        constexpr unsigned warmup_frames = 30;
        constexpr unsigned sample_frames = 240;
        constexpr unsigned warp_warmup_frames = 1;
        constexpr unsigned warp_sample_frames = 4;
        constexpr const char* kinds[]{"frosted", "plain", "blur"};

        // A level: the case and how many windows it shows.
        struct Level {
            unsigned kind{};
            unsigned windows{};
        };

        // The page at one backend: a session for each case, and the owner's steps per level, sampled frames only.
        struct Page {
            std::array<std::unique_ptr<GallerySceneSession>, 3> sessions;
            std::vector<std::vector<double>> owner;
            std::vector<double> first_owner;
        };

        Source page_source(Page& page, const std::vector<Level>& levels, unsigned warmup) {
            page.owner.assign(levels.size(), {});
            page.first_owner.assign(levels.size(), 0);
            return [&page, levels, warmup](std::size_t level, unsigned index) -> const weft::SceneFrame& {
                auto& s = *page.sessions[levels[level].kind];
                if (index == 0) {
                    s.vm.eval("__ceGlassPage.setCount(" + std::to_string(levels[level].windows) + ")");
                    const double until = s.time + settle_ms;
                    while (s.time + frame_ms <= until) {
                        s.step(s.time + frame_ms);
                    }
                    require(s.vm.eval("String(__ceGlassPage.count())") == std::to_string(levels[level].windows),
                            "glass page windows");
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
                }
                return s.current;
            };
        }

        std::string report(const char* backend, const std::string& adapter, const std::vector<Level>& levels,
                           const Page& page, const std::vector<Timings>& timings) {
            std::ostringstream out;
            out << "{\"backend\":" << json_string(backend) << ",\"adapter\":" << json_string(adapter)
                << ",\"levels\":[";
            for (std::size_t i = 0; i < levels.size(); ++i) {
                const auto& l = levels[i];
                out << (i ? "," : "") << "{\"case\":" << json_string(kinds[l.kind]) << ",\"windows\":" << l.windows
                    << ",\"owner_ms\":" << stats(page.owner[i]) << ",\"render_cpu_ms\":" << stats(timings[i].cpu)
                    << ",\"gpu_ms\":" << stats(timings[i].gpu) << ",\"first_frame_ms\":{\"owner\":" << std::fixed
                    << std::setprecision(3) << page.first_owner[i] << ",\"render_cpu\":" << timings[i].first[1]
                    << ",\"gpu\":" << timings[i].first[0] << "}}";
                std::cout << backend << ' ' << std::setw(8) << kinds[l.kind] << ' ' << std::setw(2) << l.windows
                          << " windows: owner p50 " << std::fixed << std::setprecision(3)
                          << percentile(page.owner[i], .5) << " p99 " << percentile(page.owner[i], .99)
                          << " ms; render CPU p50 " << percentile(timings[i].cpu, .5) << " p99 "
                          << percentile(timings[i].cpu, .99) << " ms; GPU p50 " << percentile(timings[i].gpu, .5)
                          << " p99 " << percentile(timings[i].gpu, .99) << " max " << percentile(timings[i].gpu, 1)
                          << " ms; first frame owner " << page.first_owner[i] << " render " << timings[i].first[1]
                          << " GPU " << timings[i].first[0] << " ms\n";
            }
            return out.str() + "]}";
        }

        Page page() {
            Page p;
            for (unsigned kind = 0; kind < 3; ++kind) {
                p.sessions[kind] = std::make_unique<GallerySceneSession>("GLASS_TESTS", kind, page_width, page_height,
                                                                         gallery_glass_page_options());
                p.sessions[kind]->set_profiling(false);
            }
            return p;
        }
    } // namespace

    int run_glass_page_benchmark(bool warp, const std::filesystem::path& folder) {
        std::filesystem::create_directories(folder);
        // WARP checks that the benchmark runs; the hardware run measures.
        std::vector<Level> levels;
        for (unsigned kind = 0; kind < 3; ++kind) {
            for (const unsigned windows : warp ? std::vector<unsigned>{4} : std::vector<unsigned>{1, 4, 8, 16, 32}) {
                levels.push_back({kind, windows});
            }
        }
        const unsigned warmup = warp ? warp_warmup_frames : warmup_frames;
        const unsigned samples = warp ? warp_sample_frames : sample_frames;
        auto direct_page = page();
        std::string adapter;
        const auto direct = particle_benchmark::d3d11(warp, levels.size(), warmup, samples,
                                                      page_source(direct_page, levels, warmup), adapter);
        std::string out = "{\"format\":\"weft-glass-page-benchmark-v1\",\"page\":[1920,1080],\"target\":[3840,2160],"
                          "\"scale\":2,\"window\":[360,232],\"settle_ms\":1000,\"warmup_frames\":" +
                          std::to_string(warmup) + ",\"sample_frames\":" + std::to_string(samples) +
#ifdef NDEBUG
                          ",\"build\":\"Release\"" +
#else
                          ",\"build\":\"Debug\"" +
#endif
                          ",\"passes\":[" + report("D3D11", adapter, levels, direct_page, direct);
        for (const auto& s : direct_page.sessions) {
            require(s->vm.eval("__ceGallery.state().errors.length") == "0", "glass page errors");
        }
#ifdef WEFT_PROBE_VULKAN
        if (!warp) {
            auto vulkan_page = page();
            std::string vulkan_adapter;
            const auto timings = particle_benchmark::vulkan(levels.size(), warmup, samples,
                                                            page_source(vulkan_page, levels, warmup), vulkan_adapter);
            if (!timings.empty()) {
                out += "," + report("Vulkan", vulkan_adapter, levels, vulkan_page, timings);
            }
        }
#endif
        PROCESS_MEMORY_COUNTERS_EX memory{};
        require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                                     sizeof(memory)),
                "process memory");
        std::cout << "process peak working set " << memory.PeakWorkingSetSize / (1024 * 1024) << " MiB\n";
        write_text(folder / "glass-page.json",
                   out + "],\"process_peak_working_set_bytes\":" + std::to_string(memory.PeakWorkingSetSize) + "}");
        std::cout << "PASS glass page benchmark: " << levels.size() << " levels at 3840 x 2160, " << samples
                  << " frames each\n";
        return 0;
    }
} // namespace probes
