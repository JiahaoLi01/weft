#include "particle_benchmark.hpp"
#include "probe_common.hpp"
#include "probe_modes.hpp"
#include "render/backends/d3d11/scene_renderer.hpp"
#include "../tests/support/scene_fonts.hpp"
#ifdef WEFT_PROBE_VULKAN
#include "render/backends/vulkan/scene_renderer.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>

// The cost of UI particles at 4K (step 11.4 of docs/refactoring-plan.md): one scene of 1920 x 1080 CSS px drawn at
// scale 2 into a 3840 x 2160 target, holding N soft discs 10-18 px across (20-36 device px) at half alpha that keep
// moving (each frame 1/60 s later), over the gallery's backdrop. The passes are timed as particle_benchmark.hpp
// describes; after warm-up frames, percentiles and the slowest frame per count.
namespace probes {
    namespace particle_benchmark {
        double percentile(std::vector<double> v, double p) {
            std::sort(v.begin(), v.end());
            return v.empty() ? 0 : v[std::min(v.size() - 1, std::size_t(std::ceil(p * v.size())) - 1)];
        }

        std::string stats(const std::vector<double>& v) {
            std::ostringstream out;
            out << std::fixed << std::setprecision(3) << "{\"p50\":" << percentile(v, .5)
                << ",\"p95\":" << percentile(v, .95) << ",\"p99\":" << percentile(v, .99)
                << ",\"max\":" << percentile(v, 1) << '}';
            return out.str();
        }

        std::vector<Timings> d3d11(bool warp, std::size_t levels, unsigned warmup, unsigned samples,
                                   const Source& source, std::string& adapter, const Anchors& anchors) {
            using Microsoft::WRL::ComPtr;
            weft::render::d3d11::Renderer gpu(warp, target_width, target_height, false);
            adapter = gpu.adapter;
            weft::render::d3d11::SceneRenderer scene(gpu, weft::testing::thread_scene_text());
            weft::render::d3d11::SceneRenderer::Slot slot;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = target_width;
            desc.Height = target_height;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            ComPtr<ID3D11Texture2D> target;
            ComPtr<ID3D11RenderTargetView> rtv;
            require(SUCCEEDED(gpu.device->CreateTexture2D(&desc, nullptr, &target)) &&
                        SUCCEEDED(gpu.device->CreateRenderTargetView(target.Get(), nullptr, &rtv)),
                    "particle benchmark target");
            ComPtr<ID3D11Query> disjoint;
            ComPtr<ID3D11Query> start;
            ComPtr<ID3D11Query> end;
            D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            require(SUCCEEDED(gpu.device->CreateQuery(&q, &disjoint)), "timestamp disjoint query");
            q.Query = D3D11_QUERY_TIMESTAMP;
            require(SUCCEEDED(gpu.device->CreateQuery(&q, &start)) && SUCCEEDED(gpu.device->CreateQuery(&q, &end)),
                    "timestamp queries");
            std::vector<Timings> out;
            for (std::size_t level = 0; level < levels; ++level) {
                Timings t;
                // The work counters as the sampled frames begin.
                std::array<std::size_t, 3> before{};
                for (unsigned k = 0; k < warmup + samples; ++k) {
                    if (k == warmup) {
                        before = {scene.draws, scene.text_builds, scene.path_builds};
                    }
                    const auto& f = source(level, k);
                    const auto began = std::chrono::steady_clock::now();
                    gpu.immediate->Begin(disjoint.Get());
                    gpu.immediate->End(start.Get());
                    if (!scene.paint(slot, f, rtv.Get(), scale, target_width, target_height, std::nullopt, 0, {},
                                     anchors ? anchors() : nullptr)) {
                        std::string why;
                        for (const auto& [reason, count] : scene.reasons) {
                            why += " " + reason;
                        }
                        require(false, "native particle pass:" + why);
                    }
                    gpu.immediate->End(end.Get());
                    gpu.immediate->End(disjoint.Get());
                    const auto cpu =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began);
                    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT timing{};
                    while (gpu.immediate->GetData(disjoint.Get(), &timing, sizeof(timing), 0) != S_OK) {
                    }
                    UINT64 a{};
                    UINT64 b{};
                    require(gpu.immediate->GetData(start.Get(), &a, sizeof(a), 0) == S_OK &&
                                gpu.immediate->GetData(end.Get(), &b, sizeof(b), 0) == S_OK && !timing.Disjoint &&
                                timing.Frequency,
                            "particle pass timestamps");
                    const double gpu_ms = double(b - a) * 1000 / double(timing.Frequency);
                    if (k == 0) {
                        t.first = {gpu_ms, cpu.count()};
                    }
                    if (k >= warmup) {
                        t.gpu.push_back(gpu_ms);
                        t.cpu.push_back(cpu.count());
                    }
                }
                t.draws = double(scene.draws - before[0]) / samples;
                t.text_builds = double(scene.text_builds - before[1]) / samples;
                t.path_builds = double(scene.path_builds - before[2]) / samples;
                t.patches = scene.kept_patches;
                t.refusals = scene.patch_refusals;
                out.push_back(std::move(t));
            }
            gpu.check_debug_messages();
            return out;
        }

#ifdef WEFT_PROBE_VULKAN
        std::vector<Timings> vulkan(std::size_t levels, unsigned warmup, unsigned samples, const Source& source,
                                    std::string& adapter, const Anchors& anchors) {
            namespace vk = weft::render::vulkan;
            std::string unavailable;
            // Without the validation layer, whose state tracking (synchronization validation above all) would be
            // timed with the pass; the tests run the same passes with it.
            auto device = vk::Device::open({.validation = false}, unavailable);
            if (!device) {
                std::cout << "Vulkan skipped: " << unavailable << '\n';
                return {};
            }
            auto& d = *device;
            adapter = d.name;
            vk::SceneRenderer scene(d, weft::testing::thread_scene_text());
            vk::SceneRenderer::Slot slot;
            // Readable, as a host's target with backdrops at its level must be.
            auto image = d.image(VK_FORMAT_B8G8R8A8_UNORM, target_width, target_height,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            VkCommandPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool_info.queueFamilyIndex = d.queue_family;
            VkCommandPool pool{};
            vk::check(d.vkd.vkCreateCommandPool(d.device, &pool_info, nullptr, &pool), "benchmark command pool");
            VkCommandBufferAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocate.commandPool = pool;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            VkCommandBuffer command{};
            vk::check(d.vkd.vkAllocateCommandBuffers(d.device, &allocate, &command), "benchmark command buffer");
            VkFenceCreateInfo fence_info{};
            fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            VkFence fence{};
            vk::check(d.vkd.vkCreateFence(d.device, &fence_info, nullptr, &fence), "benchmark fence");
            VkQueryPoolCreateInfo query{};
            query.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            query.queryType = VK_QUERY_TYPE_TIMESTAMP;
            query.queryCount = 2;
            VkQueryPool timestamps{};
            vk::check(d.vkd.vkCreateQueryPool(d.device, &query, nullptr, &timestamps), "benchmark timestamps");
            VkPhysicalDeviceProperties properties{};
            d.vki.vkGetPhysicalDeviceProperties(d.physical, &properties);
            std::vector<Timings> out;
            for (std::size_t level = 0; level < levels; ++level) {
                Timings t;
                // The work counters as the sampled frames begin.
                std::array<std::size_t, 3> before{};
                for (unsigned k = 0; k < warmup + samples; ++k) {
                    if (k == warmup) {
                        before = {scene.draws, scene.text_builds, scene.path_builds};
                    }
                    const auto& f = source(level, k);
                    const auto began = std::chrono::steady_clock::now();
                    VkCommandBufferBeginInfo begin{};
                    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                    vk::check(d.vkd.vkBeginCommandBuffer(command, &begin), "benchmark begin");
                    d.vkd.vkCmdResetQueryPool(command, timestamps, 0, 2);
                    d.vkd.vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, 0);
                    require(scene.encode(slot, f, command, {image.view, target_width, target_height, image.image},
                                         scale, std::nullopt, 0, {}, anchors ? anchors() : nullptr),
                            "native Vulkan particle pass");
                    d.vkd.vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, 1);
                    vk::check(d.vkd.vkEndCommandBuffer(command), "benchmark end");
                    VkSubmitInfo submit{};
                    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                    submit.commandBufferCount = 1;
                    submit.pCommandBuffers = &command;
                    vk::check(d.vkd.vkQueueSubmit(d.queue, 1, &submit, fence), "benchmark submit");
                    scene.submitted(slot);
                    const auto cpu =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began);
                    d.wait(fence);
                    vk::check(d.vkd.vkResetFences(d.device, 1, &fence), "benchmark fence reset");
                    std::array<std::uint64_t, 2> ticks{};
                    vk::check(d.vkd.vkGetQueryPoolResults(d.device, timestamps, 0, 2, sizeof(ticks), ticks.data(),
                                                          sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT),
                              "benchmark timestamps");
                    const double gpu_ms = double(ticks[1] - ticks[0]) * properties.limits.timestampPeriod / 1e6;
                    if (k == 0) {
                        t.first = {gpu_ms, cpu.count()};
                    }
                    if (k >= warmup) {
                        t.gpu.push_back(gpu_ms);
                        t.cpu.push_back(cpu.count());
                    }
                }
                t.draws = double(scene.draws - before[0]) / samples;
                t.text_builds = double(scene.text_builds - before[1]) / samples;
                t.path_builds = double(scene.path_builds - before[2]) / samples;
                out.push_back(std::move(t));
            }
            d.vkd.vkDeviceWaitIdle(d.device);
            d.vkd.vkDestroyQueryPool(d.device, timestamps, nullptr);
            d.vkd.vkDestroyFence(d.device, fence, nullptr);
            d.vkd.vkDestroyCommandPool(d.device, pool, nullptr);
            d.destroy(image);
            require(scene.reasons.empty(), "the Vulkan pass drew everything");
            return out;
        }
#endif
    } // namespace particle_benchmark

    namespace {
        namespace paint = weft::paint;
        using namespace particle_benchmark;
        // Frames per count on hardware; WARP only checks that the benchmark runs.
        constexpr unsigned warmup_frames = 30;
        constexpr unsigned sample_frames = 240;
        constexpr unsigned warp_warmup_frames = 1;
        constexpr unsigned warp_sample_frames = 4;
        // Emissions hold at most this many particles each, spread over a grid of the scene.
        constexpr unsigned per_emission = 50000;

        weft::SceneNode node(std::uint32_t slot, std::array<float, 4> box) {
            weft::SceneNode n;
            n.identity = {1, 1, slot};
            n.tag = "div";
            n.box = box;
            return n;
        }

        weft::SceneFrame frame(unsigned count, unsigned index) {
            weft::SceneFrame f;
            f.time_ms = 1000 + index * 1000.0 / 60;
            f.root = node(1, {0, 0, target_width / scale, target_height / scale});
            f.root.style["background-color"] = "#050403";
            paint::ParticleEmitter e;
            e.shape = paint::ParticleEmitter::Shape::Rect;
            e.area = {0, 0, 1, 1};
            e.life = {1000, 1000};
            e.speed = {10, 40};
            e.spread = 360;
            e.size = {10, 14, 18, 10};
            e.color = {paint::Color{1, .6f, .2f, .5f}, paint::Color{.9f, .4f, .2f, .5f},
                       paint::Color{.6f, .2f, .5f, .5f}, paint::Color{.3f, .1f, .6f, .5f}};
            e.softness = .5f;
            const unsigned emissions = (count + per_emission - 1) / per_emission;
            const unsigned columns = unsigned(std::ceil(std::sqrt(double(emissions))));
            const unsigned rows = (emissions + columns - 1) / columns;
            const float w = target_width / scale / columns;
            const float h = target_height / scale / rows;
            for (unsigned i = 0; i < emissions; ++i) {
                auto n = node(2 + i, {w * (i % columns), h * (i / columns), w, h});
                paint::ParticleEmission at;
                at.kind = paint::ParticleEmission::Kind::Burst;
                at.count = std::min(per_emission, count - i * per_emission);
                at.elapsed = 1 + index / 60.0;
                at.seed = 1000 + i;
                n.particles = {{std::make_shared<const weft::SceneEmitter>(weft::SceneEmitter{e, {}}), at}};
                f.root.children.push_back(n);
            }
            return f;
        }

        std::string report(const char* backend, const std::string& adapter, const std::vector<unsigned>& counts,
                           const std::vector<Timings>& timings) {
            std::ostringstream out;
            out << "{\"backend\":" << json_string(backend) << ",\"adapter\":" << json_string(adapter)
                << ",\"counts\":[";
            for (std::size_t i = 0; i < counts.size(); ++i) {
                out << (i ? "," : "") << "{\"particles\":" << counts[i] << ",\"gpu_ms\":" << stats(timings[i].gpu)
                    << ",\"cpu_ms\":" << stats(timings[i].cpu) << ",\"first_frame_ms\":{\"gpu\":" << timings[i].first[0]
                    << ",\"cpu\":" << timings[i].first[1] << "}}";
                std::cout << backend << ' ' << std::setw(7) << counts[i] << " particles: GPU p50 " << std::fixed
                          << std::setprecision(3) << percentile(timings[i].gpu, .5) << " p99 "
                          << percentile(timings[i].gpu, .99) << " max " << percentile(timings[i].gpu, 1)
                          << " ms; CPU p50 " << percentile(timings[i].cpu, .5) << " p99 "
                          << percentile(timings[i].cpu, .99) << " max " << percentile(timings[i].cpu, 1)
                          << " ms; first frame GPU " << timings[i].first[0] << " CPU " << timings[i].first[1]
                          << " ms\n";
            }
            return out.str() + "]}";
        }
    } // namespace

    int run_particle_benchmark(bool warp, const std::filesystem::path& folder) {
        std::filesystem::create_directories(folder);
        // WARP checks that the benchmark runs; the hardware run measures.
        const std::vector<unsigned> counts = warp ? std::vector<unsigned>{1000, 10000}
                                                  : std::vector<unsigned>{1000, 10000, 50000, 100000, 200000, 400000};
        const unsigned warmup = warp ? warp_warmup_frames : warmup_frames;
        const unsigned samples = warp ? warp_sample_frames : sample_frames;
        weft::SceneFrame current;
        const Source source = [&](std::size_t level, unsigned index) -> const weft::SceneFrame& {
            current = frame(counts[level], index);
            return current;
        };
        std::string adapter;
        const auto direct = particle_benchmark::d3d11(warp, counts.size(), warmup, samples, source, adapter);
        std::string out =
            "{\"format\":\"weft-particle-benchmark-v1\",\"target\":[3840,2160],\"scale\":2,\"warmup_frames\":" +
            std::to_string(warmup) + ",\"sample_frames\":" + std::to_string(samples) +
#ifdef NDEBUG
            ",\"build\":\"Release\"" +
#else
            ",\"build\":\"Debug\"" +
#endif
            ",\"particle\":\"soft disc 10-18 css px, alpha .5, moving\",\"passes\":[" +
            report("D3D11", adapter, counts, direct);
#ifdef WEFT_PROBE_VULKAN
        if (!warp) {
            std::string vulkan_adapter;
            const auto timings = particle_benchmark::vulkan(counts.size(), warmup, samples, source, vulkan_adapter);
            if (!timings.empty()) {
                out += "," + report("Vulkan", vulkan_adapter, counts, timings);
            }
        }
#endif
        PROCESS_MEMORY_COUNTERS_EX memory{};
        require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                                     sizeof(memory)),
                "process memory");
        std::cout << "process peak working set " << memory.PeakWorkingSetSize / (1024 * 1024) << " MiB\n";
        write_text(folder / "particles.json",
                   out + "],\"process_peak_working_set_bytes\":" + std::to_string(memory.PeakWorkingSetSize) + "}");
        std::cout << "PASS particle benchmark: " << counts.size() << " counts at 3840 x 2160, " << samples
                  << " frames each\n";
        return 0;
    }
} // namespace probes
