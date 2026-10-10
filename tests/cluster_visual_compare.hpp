#pragma once
#include "scene_visual_compare.hpp"

// Sparse fading points are sensitive to the >20 mask cutoff in the normal
// shape oracle. Their additional oracle uses RGB energy and 1 physical pixel
// nearest-neighbour coverage, in both directions. Dense scenes keep the
// unchanged mean <=3 / mask IoU >=.75 test.
inline bool cluster_sparse_match(const caseelephant::ProbeSceneBitmap& a, const caseelephant::ProbeSceneBitmap& b) {
    require(a.width == b.width && a.height == b.height, "sparse extent");
    double ae{};
    double be{};
    double ab{};
    double ba{};
    constexpr int background[]{caseelephant::test_constants::scene::backdrop_red,
                               caseelephant::test_constants::scene::backdrop_green,
                               caseelephant::test_constants::scene::backdrop_blue};
    for (unsigned y = 0; y < a.height; ++y) {
        for (unsigned x = 0; x < a.width; ++x) {
            for (unsigned c = 0; c < 3; ++c) {
                const auto i = (std::size_t(y) * a.width + x) * 4 + c;
                const auto av = std::abs(int(a.rgba[i]) - background[c]);
                const auto bv = std::abs(int(b.rgba[i]) - background[c]);
                int near_a{};
                int near_b{};
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        auto xx = int(x) + dx;
                        auto yy = int(y) + dy;
                        if (xx < 0 || yy < 0 || xx >= int(a.width) || yy >= int(a.height)) {
                            continue;
                        }
                        auto j = (std::size_t(yy) * a.width + xx) * 4 + c;
                        near_a = std::max(near_a, std::abs(int(a.rgba[j]) - background[c]));
                        near_b = std::max(near_b, std::abs(int(b.rgba[j]) - background[c]));
                    }
                }
                ae += av;
                be += bv;
                ab += std::min(av, near_b);
                ba += std::min(bv, near_a);
            }
        }
    }
    return ae > 0 && be > 0 &&
           std::min(ae, be) / std::max(ae, be) >= caseelephant::test_constants::scene::sparse_min_energy_ratio &&
           ab / ae >= caseelephant::test_constants::scene::sparse_min_coverage &&
           ba / be >= caseelephant::test_constants::scene::sparse_min_coverage;
}

inline bool cluster_image_match(const caseelephant::ProbeSceneBitmap& a, const caseelephant::ProbeSceneBitmap& b) {
    const auto d = scene_difference(a, b);
    const auto mean = *std::max_element(d.mean.begin(), d.mean.end());
    return mean <= caseelephant::test_constants::scene::max_channel_mean_difference &&
           (d.foreground < caseelephant::test_constants::scene::min_foreground_pixels ||
            d.soft_iou >= caseelephant::test_constants::scene::min_soft_iou ||
            (d.foreground < caseelephant::test_constants::scene::sparse_max_foreground_pixels &&
             mean < caseelephant::test_constants::scene::sparse_max_channel_mean && cluster_sparse_match(a, b)));
}
