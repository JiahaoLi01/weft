#include "gallery_scene_session.hpp"
#include "particle_benchmark.hpp"
#include "probe_common.hpp"
#include "probe_modes.hpp"
#include "sampling_profiler.hpp"

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
// changed bar widths; the owner then takes the DOM's changes into the scene. A second run hands the page no data at
// all: it computes the same values itself, the floor of any way to hand JS the data (shared memory included); with 400
// units it also changes only every 10th, 100th or 400th, which shows what a frame costs when little changes. The page
// is 1920 x 1080 CSS px drawn at scale 2 into 3840 x 2160 with the particle benchmarks' loops (particle_benchmark.hpp).
// A third run anchors the nameplates (data-anchor, docs/engine-communication.md): each frame the host writes every
// unit's anchor, the pass places the nameplates with the latest anchors, and the page changes only the health bars
// the host pushes as JSON (none at a level without health changes): their width, which lays the page out again, or at
// one level their scale, which does not. Timed per frame: the host's JSON (and anchors), the
// script call, the owner's update (styles, layout and the scene), and the pass (CPU, GPU). Sampled, the page computes
// its values with 400 units, changing one or all each frame, and the owner's updates and the passes of the sampled frames
// are sampled apart (owner-samples-<changed>.txt, render-samples-<changed>.txt; names need the windows-profile PDBs).
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

        // How the page gets each frame's values: pushed as JSON, computed by the page, or anchored (the host writes
        // anchors and pushes the health that changed).
        enum class Push {
            Json,
            Page,
            Anchor
        };

        // How many units there are, and which change each frame: every `every`-th (the page computes only those);
        // anchored, whether health changes, and whether bars scale instead of changing width.
        struct Level {
            unsigned units{};
            unsigned every = 1;
            bool health = true;
            bool scaled{};
        };

        // Samplers, when given, sample the owner's updates and the passes of the sampled frames (a page that computes
        // its values).
        struct Samplers {
            SamplingProfiler* owner{};
            SamplingProfiler* render{};
        };

        Source source(GallerySceneSession& s, const std::vector<Level>& levels, std::vector<Owner>& owner,
                      unsigned warmup, Push push, weft::SceneAnchorChannel* anchors = nullptr, Samplers samplers = {}) {
            owner.assign(levels.size(), {});
            // Each unit's health, from 0 to 1.
            auto health = std::make_shared<std::vector<double>>();
            auto places = std::make_shared<std::vector<weft::SceneAnchor>>();
            if (push == Push::Anchor) {
                s.vm.eval("__ceHud.anchor(true)");
            }
            return [&s, levels, &owner, warmup, health, push, anchors, places,
                    samplers](std::size_t level, unsigned index) -> const weft::SceneFrame& {
                if (samplers.render) {
                    samplers.render->active(false);
                }
                const auto n = levels[level].units;
                auto& hp = *health;
                if (index == 0) {
                    // Anchored, each level starts with new nameplates, at full health as the host's values do.
                    s.vm.eval(std::string(push == Push::Anchor ? "__ceHud.setCount(0);" : "") + "__ceHud.setCount(" +
                              std::to_string(n) + ")");
                    hp.assign(n, 1.0);
                }
                const double t = s.time / 1000;
                std::string json;
                const auto began = std::chrono::steady_clock::now();
                if (push == Push::Anchor) {
                    // Every unit's anchor, its key the unit's index + 1; then the health that changed, as JSON.
                    auto& a = *places;
                    a.resize(n);
                    for (unsigned i = 0; i < n; ++i) {
                        const auto p = place(i, t);
                        a[i] = {i + 1, float(p[0]), float(p[1])};
                    }
                    require(anchors->write(a) == weft::SceneAnchorWrite::Written, "HUD anchors");
                    json += '[';
                    char entry[48];
                    for (unsigned i = 0; levels[level].health && i < n; ++i) {
                        if ((i + index) % 20 == 0) {
                            hp[i] = hp[i] > 0.05 ? hp[i] - 0.05 : 1.0;
                            const int length =
                                std::snprintf(entry, sizeof entry, "%s[%u,%.2f]", json.size() > 1 ? "," : "", i, hp[i]);
                            json.append(entry, std::size_t(length));
                        }
                    }
                    json += ']';
                    const auto pushed = std::chrono::steady_clock::now();
                    if (json.size() > 2) {
                        s.vm.eval("__ceHud.health('" + json + "'," + (levels[level].scaled ? "true" : "false") + ")");
                    }
                    const auto applied = std::chrono::steady_clock::now();
                    s.step(s.time + frame_ms);
                    const auto updated = std::chrono::steady_clock::now();
                    require(s.vm.eval("__ceGallery.state().errors.length") == "0", "HUD page errors");
                    if (index >= warmup) {
                        auto& o = owner[level];
                        using ms = std::chrono::duration<double, std::milli>;
                        o.json.push_back(ms(pushed - began).count());
                        o.script.push_back(ms(applied - pushed).count());
                        o.update.push_back(ms(updated - applied).count());
                        o.json_bytes = json.size();
                    }
                    return s.current;
                }
                if (push == Push::Page) {
                    const auto call = "__ceHud.simulate(" + weft::testing::format_host_time(t) + "," +
                                      std::to_string(index) + "," + std::to_string(levels[level].every) + ")";
                    const auto written = std::chrono::steady_clock::now();
                    s.vm.eval(call);
                    const auto applied = std::chrono::steady_clock::now();
                    if (samplers.owner) {
                        samplers.owner->active(index >= warmup);
                    }
                    s.step(s.time + frame_ms);
                    if (samplers.owner) {
                        samplers.owner->active(false);
                    }
                    const auto updated = std::chrono::steady_clock::now();
                    require(s.vm.eval("__ceGallery.state().errors.length") == "0", "HUD page errors");
                    if (index >= warmup) {
                        auto& o = owner[level];
                        using ms = std::chrono::duration<double, std::milli>;
                        o.json.push_back(ms(written - began).count());
                        o.script.push_back(ms(applied - written).count());
                        o.update.push_back(ms(updated - applied).count());
                    }
                    if (samplers.render) {
                        samplers.render->active(index >= warmup);
                    }
                    return s.current;
                }
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

        std::string report(const char* backend, const char* push, const std::string& adapter,
                           const std::vector<Level>& levels, const std::vector<Owner>& owner,
                           const std::vector<Timings>& timings) {
            std::ostringstream out;
            out << "{\"backend\":" << json_string(backend) << ",\"push\":" << json_string(push)
                << ",\"adapter\":" << json_string(adapter) << ",\"counts\":[";
            for (std::size_t i = 0; i < levels.size(); ++i) {
                const auto& o = owner[i];
                const auto& t = timings[i];
                const auto changed = std::string(push) == "anchor"
                                         ? (levels[i].health ? (levels[i].units + 19) / 20 : 0)
                                         : (levels[i].units + levels[i].every - 1) / levels[i].every;
                out << (i ? "," : "") << "{\"units\":" << levels[i].units << ",\"changed\":" << changed
                    << ",\"scaled_bars\":" << (levels[i].scaled ? "true" : "false")
                    << ",\"json_bytes\":" << o.json_bytes << ",\"host_json_ms\":" << stats(o.json)
                    << ",\"script_ms\":" << stats(o.script) << ",\"owner_update_ms\":" << stats(o.update)
                    << ",\"render_cpu_ms\":" << stats(t.cpu) << ",\"gpu_ms\":" << stats(t.gpu)
                    << ",\"per_frame\":{\"draws\":" << std::fixed << std::setprecision(1) << t.draws
                    << ",\"text_builds\":" << t.text_builds << "}}";
                std::cout << backend << ' ' << push << ' ' << std::setw(5) << levels[i].units << " units, "
                          << std::setw(3) << changed << (levels[i].scaled ? " scaled" : "")
                          << " changing: host JSON p50 " << std::fixed << std::setprecision(3) << percentile(o.json, .5)
                          << " ms (" << o.json_bytes << " bytes); script p50 " << percentile(o.script, .5) << " p99 "
                          << percentile(o.script, .99) << " ms; owner update p50 " << percentile(o.update, .5)
                          << " p99 " << percentile(o.update, .99) << " ms; render CPU p50 " << percentile(t.cpu, .5)
                          << " p99 " << percentile(t.cpu, .99) << " ms; GPU p50 " << percentile(t.gpu, .5) << " ms; "
                          << std::setprecision(1) << t.draws << " draws\n";
            }
            return out.str() + "]}";
        }

        std::unique_ptr<GallerySceneSession> session() {
            auto s = std::make_unique<GallerySceneSession>("HUD_TESTS", 0, page_width, page_height,
                                                           GallerySceneOptions{"hud-utilities.css", "HUD_TESTS.js"});
            s->set_profiling(false);
            // As a View: element geometry only for geometry reads, which the page makes none of.
            s->eager_geometry = false;
            return s;
        }
    } // namespace

    int run_hud_benchmark(bool warp, const std::filesystem::path& folder) {
        std::filesystem::create_directories(folder);
        // WARP checks that the benchmark runs; the hardware run measures.
        const std::vector<Level> counts = warp ? std::vector<Level>{{100}} : std::vector<Level>{{100}, {200}, {400}};
        const std::vector<Level> partial =
            warp ? std::vector<Level>{{100}, {100, 100}}
                 : std::vector<Level>{{100}, {200}, {400}, {400, 10}, {400, 100}, {400, 400}};
        const std::vector<Level> anchored =
            warp ? std::vector<Level>{{100}, {100, 1, false}, {100, 1, true, true}}
                 : std::vector<Level>{{100}, {200}, {400}, {400, 1, false}, {400, 1, true, true}};
        const unsigned warmup = warp ? warp_warmup_frames : warmup_frames;
        const unsigned samples = warp ? warp_sample_frames : sample_frames;
        auto json_page = session();
        std::vector<Owner> json_owner;
        std::string adapter;
        const auto json = particle_benchmark::d3d11(
            warp, counts.size(), warmup, samples, source(*json_page, counts, json_owner, warmup, Push::Json), adapter);
        auto own_page = session();
        std::vector<Owner> own_owner;
        const auto own = particle_benchmark::d3d11(warp, partial.size(), warmup, samples,
                                                   source(*own_page, partial, own_owner, warmup, Push::Page), adapter);
        auto anchor_page = session();
        std::vector<Owner> anchor_owner;
        weft::SceneAnchorChannel channel;
        anchor_page->anchors = &channel;
        const auto anchor = particle_benchmark::d3d11(
            warp, anchored.size(), warmup, samples,
            source(*anchor_page, anchored, anchor_owner, warmup, Push::Anchor, &channel), adapter, [&channel] {
                return &channel.take(weft::SceneAnchorChannel::Reader::Render);
            });
        std::string out = "{\"format\":\"weft-hud-benchmark-v1\",\"page\":[1920,1080],\"target\":[3840,2160],"
                          "\"scale\":2,\"warmup_frames\":" +
                          std::to_string(warmup) + ",\"sample_frames\":" + std::to_string(samples) +
#ifdef NDEBUG
                          ",\"build\":\"Release\"" +
#else
                          ",\"build\":\"Debug\"" +
#endif
                          ",\"passes\":[" + report("D3D11", "json", adapter, counts, json_owner, json) + "," +
                          report("D3D11", "page", adapter, partial, own_owner, own) + "," +
                          report("D3D11", "anchor", adapter, anchored, anchor_owner, anchor);
#ifdef WEFT_PROBE_VULKAN
        // The anchored levels on Vulkan too.
        if (!warp) {
            auto vulkan_page = session();
            std::vector<Owner> vulkan_owner;
            weft::SceneAnchorChannel vulkan_channel;
            vulkan_page->anchors = &vulkan_channel;
            std::string vulkan_adapter;
            const auto timings = particle_benchmark::vulkan(
                anchored.size(), warmup, samples,
                source(*vulkan_page, anchored, vulkan_owner, warmup, Push::Anchor, &vulkan_channel), vulkan_adapter,
                [&vulkan_channel] {
                    return &vulkan_channel.take(weft::SceneAnchorChannel::Reader::Render);
                });
            if (!timings.empty()) {
                out += "," + report("Vulkan", "anchor", vulkan_adapter, anchored, vulkan_owner, timings);
            }
        }
#endif
        PROCESS_MEMORY_COUNTERS_EX memory{};
        require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                                     sizeof(memory)),
                "process memory");
        std::cout << "process peak working set " << memory.PeakWorkingSetSize / (1024 * 1024) << " MiB\n";
        write_text(folder / "hud.json",
                   out + "],\"process_peak_working_set_bytes\":" + std::to_string(memory.PeakWorkingSetSize) + "}");
        std::cout << "PASS HUD benchmark: " << counts.size() + partial.size() + anchored.size()
                  << " levels at 3840 x 2160, " << samples << " frames each\n";
        return 0;
    }

    int run_hud_benchmark_sampled(const std::filesystem::path& folder) {
        std::filesystem::create_directories(folder);
        const auto period = std::chrono::microseconds(weft::probe_constants::gallery::owner_sample_period_us);
        for (const unsigned every : {400u, 1u}) {
            const std::vector<Level> levels{{400, every}};
            SamplingProfiler owner_sampler(period);
            SamplingProfiler render_sampler(period);
            auto page = session();
            std::vector<Owner> owner;
            std::string adapter;
            const auto timings = particle_benchmark::d3d11(
                false, levels.size(), warmup_frames, sample_frames,
                source(*page, levels, owner, warmup_frames, Push::Page, nullptr, {&owner_sampler, &render_sampler}),
                adapter);
            const auto changed = std::to_string(400 / every);
            const auto owner_samples = owner_sampler.write(folder / ("owner-samples-" + changed + ".txt"));
            const auto render_samples = render_sampler.write(folder / ("render-samples-" + changed + ".txt"));
            std::cout << "400 units, " << changed << " changing: owner update p50 " << std::fixed
                      << std::setprecision(3) << percentile(owner[0].update, .5) << " ms (" << owner_samples
                      << " samples); render CPU p50 " << percentile(timings[0].cpu, .5) << " ms (" << render_samples
                      << " samples)\n";
        }
        std::cout << "PASS HUD benchmark sampled\n";
        return 0;
    }
} // namespace probes
