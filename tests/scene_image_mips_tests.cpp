#include "scene/scene_native.hpp"
#include "support/check.hpp"

#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <vector>

// The smaller levels of a page's image (scene_image_mips), as Chrome's Skia builds an image's mipmaps: the rounded mean
// of the 2 x 2 texels a texel covers, weights 1, 2, 1 along an axis of odd length (which tests/image_scale_metal_tests.mm
// does not reach: its images halve evenly to the levels it draws at), each level half the one before, at least 1 texel,
// down to 1 x 1; each channel apart. Portable.
namespace {
    using weft::testing::require;

    // Texels of 4 equal channels.
    std::vector<std::uint8_t> texels(std::initializer_list<std::uint8_t> values) {
        std::vector<std::uint8_t> out;
        for (const auto v : values) {
            out.insert(out.end(), {v, v, v, v});
        }
        return out;
    }
} // namespace

int main() {
    try {
        const auto square = weft::scene_image_mips(texels({0, 255, 255, 1}), 2, 2);
        require(square.size() == 1 && square[0] == texels({128}), "a 2 x 2 image's rounded mean");

        const auto row = weft::scene_image_mips(texels({10, 20, 40}), 3, 1);
        require(row.size() == 1 && row[0] == texels({23}), "an odd row's 1-2-1 mean");

        // 5 x 3, its one lit texel in the middle column and row: 2 x 1, both texels' 3 x 3 taps sharing that column
        // (its weight 1 x 2 of 16), then 1 x 1.
        const auto odd = weft::scene_image_mips(texels({0, 0, 0, 0, 0, 0, 0, 160, 0, 0, 0, 0, 0, 0, 0}), 5, 3);
        require(odd.size() == 2 && odd[0] == texels({20, 20}) && odd[1] == texels({20}),
                "an odd image's levels down to 1 x 1");

        // Channels apart, and a 1-texel axis kept while the other halves.
        const std::vector<std::uint8_t> column{0, 10, 200, 255, 4, 30, 100, 255, 8, 50, 0, 255, 12, 70, 0, 255};
        const auto halves = weft::scene_image_mips(column, 1, 4);
        require(halves.size() == 2 && halves[0] == std::vector<std::uint8_t>{2, 20, 150, 255, 10, 60, 0, 255} &&
                    halves[1] == std::vector<std::uint8_t>{6, 40, 75, 255},
                "a column's levels, each channel apart");

        require(weft::scene_image_mips(texels({7}), 1, 1).empty(), "a 1 x 1 image has no smaller level");
        bool refused{};
        try {
            weft::scene_image_mips(texels({1, 2}), 3, 1);
        } catch (const std::invalid_argument&) {
            refused = true;
        }
        require(refused, "pixels that are not width x height texels are refused");
        std::cout << "PASS scene image mips\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
