#pragma once

#include "caseelephant/constants/common.hpp"

// Scene, gallery and cluster test values (see docs/coding-style.md).
//
// Each constant has the type of the literals it replaced (a plain integer literal is int), so
// arithmetic, comparisons and generated code at every use site stay exactly as before.
namespace caseelephant::test_constants::scene {
    // Gallery backdrop #050403. Blank oracle negatives are filled with it.
    inline constexpr int backdrop_red = 5;
    inline constexpr int backdrop_green = 4;
    inline constexpr int backdrop_blue = 3;

    // Visual oracle against the Chromium reference images.
    // Largest mean absolute difference of any RGBA channel.
    inline constexpr int max_channel_mean_difference = 3;
    // A pixel is foreground when a color channel differs from the backdrop by more than this.
    inline constexpr int foreground_channel_threshold = 20;
    // A 1-2-1 blurred foreground mask value above this stays foreground.
    inline constexpr int blurred_foreground_threshold = 48;
    inline constexpr double min_soft_iou = .75;
    // Reference images with fewer foreground pixels are too sparse for the IoU test.
    inline constexpr int min_foreground_pixels = 200;
    // Sparse particle oracle: at most this many foreground pixels and this channel mean, then
    // RGB energy ratio and nearest-neighbour coverage in both directions.
    inline constexpr int sparse_max_foreground_pixels = 1024;
    inline constexpr double sparse_max_channel_mean = .1;
    inline constexpr double sparse_min_energy_ratio = .9;
    inline constexpr double sparse_min_coverage = .95;

    // Gallery capture timeline.
    inline constexpr int frames_per_second = 60;
    inline constexpr double frame_interval_ms =
        static_cast<double>(constants::milliseconds_per_second) / frames_per_second;
    // Last captured frame of a scene gallery case and of a cluster case.
    inline constexpr int gallery_last_frame = 120;
    inline constexpr int cluster_last_frame = 300;
    // Rounded 60 Hz step of the drag input script.
    inline constexpr double drag_frame_interval_ms = 16.6666667;
    // Device scale of gallery bitmaps compared with the 2x references.
    inline constexpr int gallery_raster_scale = 2;

    // Live nodes of an empty document after unmount and collection.
    inline constexpr int empty_document_live_nodes = 3;

    // Scene contract tests.
    inline constexpr int timeline_view_size = 100;
    inline constexpr int contract_view_size = 80;
    // Tolerance of sampled color channels and opacity.
    inline constexpr double paint_sample_tolerance = .001;
    // Random mutations compared against a fresh full scene, with a collection every interval.
    inline constexpr int mutation_iterations = 120;
    inline constexpr int collect_garbage_interval = 16;
    // Random mutations compared against the full path in the cluster contract test.
    inline constexpr int cluster_mutation_iterations = 64;
} // namespace caseelephant::test_constants::scene
