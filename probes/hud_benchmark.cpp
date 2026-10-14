#include "gallery_scene_session.hpp"
#include "particle_benchmark.hpp"
#include "probe_common.hpp"
#include "probe_modes.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>

// World-anchored nameplates driven the way an engine's browser bridge drives a page (docs/refactoring-plan.md, step
// 13): N units, each a name over a health bar (examples/crpg-gallery/native/HUD_TESTS.js), follow points the host moves
// every frame, and about one in twenty loses health each frame. Each frame the host writes every unit's position and
// health as JSON and evaluates a script call with it; the page parses it and writes each nameplate's transform and
// changed bar widths; the owner then takes the DOM's changes into the scene. The page is 1920 x 1080 CSS px drawn at
// scale 2 into 3840 x 2160 with the particle benchmarks' loops (particle_benchmark.hpp). Timed per frame: the host's
// JSON, the script call, the owner's update (styles, layout and the scene), and the pass (CPU, GPU).
namespace probes {
    namespace {
        using namespace particle_benchmark;
        constexpr float page_width = target_width / scale;
        constexpr float page_height = target_height / scale;
        constexpr double frame_ms = 1000.0 / 60;
        constexpr unsigned warmup_frames = 30;
        constexpr unsigned sample_frames = 240;
        constexpr unsigned warp_warmup_frames = 1;
        constexpr unsigned warp_sample_frames = 4;

        struct Owner {
            std::vector<double> json;
            std::vector<double> script;
            std::vector<double> update;
            std::size_t json_bytes{};
        };

        // Where unit i is at t seconds: a circle of its own about its own centre, in page px.
        std::array<double, 2> place(unsigned i, double t) {
            const double cx = 80 + std::fmod(i * 197.0, page_width - 260);
            const double cy = 60 + std::fmod(i * 113.0, page_height - 160);
            const double r = 20 + (i % 7) * 9;
            const double a = t * (0.6 + (i % 5) * 0.25) + i * 0.7;
            return {cx + r * std::cos(a), cy + r * std::sin(a)};
        }

        Source source(GallerySceneSession& s, const std::vector<unsigned>& counts, std::vector<Owner>& owner,
                      unsigned warmup) {
            owner.assign(counts.size(), {});
            // Each unit's health, from 0 to 1.
            auto health = std::make_shared<std::vector<double>>();
            return [&s, counts, &owner, warmup, health](std::size_t level, unsigned index) -> const weft::SceneFrame& {
                const auto n = counts[level];
                auto& hp = *health;
                if (index == 0) {
                    s.vm.eval("__ceHud.setCount(" + std::to_string(n) + ")");
                    hp.assign(n, 1.0);
                }
                const double t = s.time / 1000;
                std::string json;
                const auto began = std::chrono::steady_clock::now();
                json.reserve(std::size_t(n) * 24 + 2);
                json += '[';
                char entry[64];
                for (unsigned i = 0; i < n; ++i) {
                    if ((i + index) % 20 == 0) {
                        hp[i] = hp[i] > 0.05 ? hp[i] - 0.05 : 1.0;
                    }
                    const auto p = place(i, t);
                    const int written =
                        std::snprintf(entry, sizeof entry, "%s[%.2f,%.2f,%.2f]", i ? "," : "", p[0], p[1], hp[i]);
                    json.append(entry, std::size_t(written));
                }
                json += ']';
                const auto written = std::chrono::steady_clock::now();
                s.vm.eval("__ceHud.apply('" + json + "')");
                const auto applied = std::chrono::steady_clock::now();
                s.step(s.time + frame_ms);
                const auto updated = std::chrono::steady_clock::now();
                require(s.vm.eval("__ceGallery.state().errors.length") == "0", "HUD page errors");
                if (index >= warmup) {
                    auto& o = owner[level];
                    using ms = std::chrono::duration<double, std::milli>;
                    o.json.push_back(ms(written - began).count());
                    o.script.push_back(ms(applied - written).count());
                    o.update.push_back(ms(updated - applied).count());
                    o.json_bytes = json.size();
                }
                return s.current;
            };
        }

        std::string report(const char* backend, const std::string& adapter, const std::vector<unsigned>& counts,
                           const std::vector<Owner>& owner, const std::vector<Timings>& timings) {
            std::ostringstream out;
            out << "{\"backend\":" << json_string(backend) << ",\"adapter\":" << json_string(adapter)
                << ",\"counts\":[";
            for (std::size_t i = 0; i < counts.size(); ++i) {
                const auto& o = owner[i];
                const auto& t = timings[i];
                out << (i ? "," : "") << "{\"units\":" << counts[i] << ",\"json_bytes\":" << o.json_bytes
                    << ",\"host_json_ms\":" << stats(o.json) << ",\"script_ms\":" << stats(o.script)
                    << ",\"owner_update_ms\":" << stats(o.update) << ",\"render_cpu_ms\":" << stats(t.cpu)
                    << ",\"gpu_ms\":" << stats(t.gpu) << ",\"per_frame\":{\"draws\":" << std::fixed
                    << std::setprecision(1) << t.draws << ",\"text_builds\":" << t.text_builds << "}}";
                std::cout << backend << ' ' << std::setw(5) << counts[i] << " units: host JSON p50 " << std::fixed
                          << std::setprecision(3) << percentile(o.json, .5) << " ms (" << o.json_bytes
                          << " bytes); script p50 " << percentile(o.script, .5) << " p99 " << percentile(o.script, .99)
                          << " ms; owner update p50 " << percentile(o.update, .5) << " p99 "
                          << percentile(o.update, .99) << " ms; render CPU p50 " << percentile(t.cpu, .5) << " p99 "
                          << percentile(t.cpu, .99) << " ms; GPU p50 " << percentile(t.gpu, .5) << " ms; "
                          << std::setprecision(1) << t.draws << " draws\n";
            }
            return out.str() + "]}";
        }

        std::unique_ptr<GallerySceneSession> session() {
            auto s = std::make_unique<GallerySceneSession>("HUD_TESTS", 0, page_width, page_height,
                                                           GallerySceneOptions{"hud-utilities.css", "HUD_TESTS.js"});
            s->set_profiling(false);
            return s;
        }
    } // namespace

    int run_hud_benchmark(bool warp, const std::filesystem::path& folder) {
        std::filesystem::create_directories(folder);
        // WARP checks that the benchmark runs; the hardware run measures.
        const std::vector<unsigned> counts = warp ? std::vector<unsigned>{100} : std::vector<unsigned>{100, 200, 400};
        const unsigned warmup = warp ? warp_warmup_frames : warmup_frames;
        const unsigned samples = warp ? warp_sample_frames : sample_frames;
        auto direct_page = session();
        std::vector<Owner> direct_owner;
        std::string adapter;
        const auto direct = particle_benchmark::d3d11(warp, counts.size(), warmup, samples,
                                                      source(*direct_page, counts, direct_owner, warmup), adapter);
        std::string out = "{\"format\":\"weft-hud-benchmark-v1\",\"page\":[1920,1080],\"target\":[3840,2160],"
                          "\"scale\":2,\"push\":\"json\",\"warmup_frames\":" +
                          std::to_string(warmup) + ",\"sample_frames\":" + std::to_string(samples) +
#ifdef NDEBUG
                          ",\"build\":\"Release\"" +
#else
                          ",\"build\":\"Debug\"" +
#endif
                          ",\"passes\":[" + report("D3D11", adapter, counts, direct_owner, direct);
        PROCESS_MEMORY_COUNTERS_EX memory{};
        require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                                     sizeof(memory)),
                "process memory");
        std::cout << "process peak working set " << memory.PeakWorkingSetSize / (1024 * 1024) << " MiB\n";
        write_text(folder / "hud.json",
                   out + "],\"process_peak_working_set_bytes\":" + std::to_string(memory.PeakWorkingSetSize) + "}");
        std::cout << "PASS HUD benchmark: " << counts.size() << " counts at 3840 x 2160, " << samples
                  << " frames each\n";
        return 0;
    }
} // namespace probes
