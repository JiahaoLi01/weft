#pragma once

#include <filesystem>
#include <string>

// Probe modes of weft_d3d11_probe, one translation unit each; d3d11_probe.cpp dispatches on argv.
namespace probes {
    // react_window_host.cpp
    int run_react_window(bool warp, bool self_test, const std::filesystem::path& folder);
    // render_backend_probe.cpp
    int run_backend(bool warp, const std::filesystem::path& folder);
    // cheetah_probe.cpp
    int run_cheetah(bool warp, bool visible, const std::filesystem::path& folder);
    // cheetah_alert_probe.cpp
    int run_cheetah_alert(bool warp, const std::filesystem::path& folder, bool original_style = false,
                          bool automatic = false, bool flow = false);
    // flow_probe.cpp
    int run_flow(bool warp, const std::filesystem::path& folder);
    // cheetah_flow_window_probe.cpp
    int run_cheetah_flow_window(bool warp, bool self_test, const std::filesystem::path& folder,
                                bool high_resolution = false, bool performance = false, bool sequential = false,
                                bool motion_test = false);
    // vulkan_flow_window_probe.cpp, built with the Vulkan backend (WEFT_PROBE_VULKAN)
    int run_cheetah_flow_window_vulkan(bool self_test, const std::filesystem::path& folder, bool performance);
    // scene_vulkan_probe.cpp, built with the Vulkan backend (WEFT_PROBE_VULKAN)
    int run_scene_vulkan_test(const std::filesystem::path& folder, const std::string& adapter = {});
    // scene_vulkan_window.cpp, built with the Vulkan backend (WEFT_PROBE_VULKAN)
    int run_scene_vulkan_window(bool self_test, const std::filesystem::path& folder);
    // render_cache_probe.cpp
    int run_render_cache(bool warp, const std::filesystem::path& folder);
    // two_frame_probe.cpp
    int run_two_frame_probe(bool warp, const std::filesystem::path& folder);
    // gallery_canvas_probe.cpp
    int run_gallery_canvas(bool warp, const std::filesystem::path& folder);
    // gallery_canvas_probe.cpp
    int run_gallery_canvas_window(bool warp, const std::filesystem::path& folder, bool self_test);
    // scene_gpu_surface_probe.cpp
    int run_scene_gpu_test(bool warp, const std::filesystem::path& folder, bool optimized = false);
    // scene_surface_retirement_probe.cpp
    int run_scene_surface_retirement(bool warp, const std::filesystem::path& folder);
    // particle_benchmark.cpp
    int run_particle_benchmark(bool warp, const std::filesystem::path& folder);
    // gallery_cluster_gpu_probe.cpp
    int run_gallery_cluster_gpu(bool warp, const std::filesystem::path& folder);
    // gallery_interaction_profile.cpp
    // sample: also sample the owner thread's stacks during warm ticks into samples.txt (needs PDBs for names).
    int run_gallery_interaction_profile(const std::filesystem::path& folder, const std::string& filter,
                                        bool sample = false);
    // gallery_preview_probe.cpp
    int run_gallery_preview(bool warp, const std::filesystem::path& folder, bool self_test, std::string benchmark = {},
                            bool threaded = true, bool scroll_test = false, bool profile = false);
    // external_texture_probe.cpp
    int run_external_texture(bool warp, const std::filesystem::path& folder);
    // complex_css_probe.cpp
    int run_complex_css_test(bool warp, const std::filesystem::path& folder);
    // render_stages_probe.cpp
    int run_render_stages(bool warp, const std::filesystem::path& folder);
    // render_stages_probe.cpp
    int run_owner_async(bool warp, const std::filesystem::path& folder);
} // namespace probes
