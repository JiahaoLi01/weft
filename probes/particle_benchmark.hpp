#pragma once

#include "scene/scene.hpp"
#include "scene/scene_anchor.hpp"

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

// The 4K particle passes the particle benchmarks time (particle_benchmark.cpp): each frame a source gives drawn at scale
// 2 into a 3840 x 2160 target, timed on the GPU by timestamps around the pass and on the CPU from the frame to its
// submitted pass (display list, plan, upload, draw calls), the GPU idle before each frame. D3D11 on the adapter asked
// for, and the Vulkan scene pass on this machine's device where built, without the validation layer, into a target it
// may read (backdrops at its level copy from it).
namespace probes::particle_benchmark {
    inline constexpr unsigned target_width = 3840;
    inline constexpr unsigned target_height = 2160;
    inline constexpr float scale = 2;

    // The sampled frames' times, and the first frame's: the start-up spike of a level's first pass. Per sampled frame,
    // the pass's draw calls and the text and path images it built (SceneNativeWork).
    struct Timings {
        std::vector<double> gpu;
        std::vector<double> cpu;
        std::array<double, 2> first{};
        double draws{};
        double text_builds{};
        double path_builds{};
        // Kept passes patched and patches refused by reason (SceneNativeWork), up to the level's last frame: warm-up
        // and earlier levels included.
        std::size_t patches{};
        std::map<std::string, std::size_t> refusals;
    };

    // The frame to draw at frame index of level (0, 1, ...); a frame lives until the next call.
    using Source = std::function<const weft::SceneFrame&(std::size_t level, unsigned index)>;
    // The anchors a pass places anchored elements with, taken as the pass begins (timed with it), as a render thread
    // takes the latest table of a View's anchors; none when empty.
    using Anchors = std::function<const weft::SceneAnchorTable*()>;

    double percentile(std::vector<double> v, double p);
    // {"p50", "p95", "p99", "max"} of v, in ms.
    std::string stats(const std::vector<double>& v);

    // Each level's warmup frames, then its sampled frames, timed.
    std::vector<Timings> d3d11(bool warp, std::size_t levels, unsigned warmup, unsigned samples, const Source& source,
                               std::string& adapter, const Anchors& anchors = {});
#ifdef WEFT_PROBE_VULKAN
    // Empty without a Vulkan device.
    std::vector<Timings> vulkan(std::size_t levels, unsigned warmup, unsigned samples, const Source& source,
                                std::string& adapter, const Anchors& anchors = {});
#endif
} // namespace probes::particle_benchmark
