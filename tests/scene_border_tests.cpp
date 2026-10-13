#include "scene/scene_border.hpp"
#include "support/check.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

// Dotted and dashed border sides against Chrome 155's pixels: each row is a bottom border of one style, width and
// length at device scale 1, white over black, as tools/browser_border_patterns.mjs records it ('#' a pixel of at least
// 200, '+' above 30, '.' the rest). The shapes of scene_dashed_side, filled with their exact coverage (rectangles, and
// round dots), give the same row. The rows cover Blink's end dots for thin dots (EnforceDotsAtEndpoints), the gap that
// fits a whole number of dashes, proportionally sized pairs, lines too short for a pattern, and round dots over 3 px.
namespace {
    using weft::testing::require;

    struct Row {
        const char* style;
        int width;
        int length;
        const char* pixels;
    };

    constexpr Row rows[] = {
        // Chrome/155.0.8059.40
        {"dotted", 1, 7, "#.#.#.#"},
        {"dotted", 1, 8, "##.#.#.#"},
        {"dotted", 1, 9, "#.#.#.#.#"},
        {"dotted", 1, 10, "##.#.#.#.#"},
        {"dotted", 1, 11, "#.#.#.#.#.#"},
        {"dotted", 1, 12, "##.#.#.#.#.#"},
        {"dotted", 1, 13, "#.#.#.#.#.#.#"},
        {"dotted", 1, 20, "##.#.#.#.#.#.#.#.#.#"},
        {"dotted", 1, 21, "#.#.#.#.#.#.#.#.#.#.#"},
        {"dotted", 1, 22, "##.#.#.#.#.#.#.#.#.#.#"},
        {"dotted", 1, 23, "#.#.#.#.#.#.#.#.#.#.#.#"},
        {"dotted", 1, 24, "##.#.#.#.#.#.#.#.#.#.#.#"},
        {"dotted", 1, 25, "#.#.#.#.#.#.#.#.#.#.#.#.#"},
        {"dotted", 1, 37, "#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#"},
        {"dotted", 1, 38, "##.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#"},
        {"dotted", 1, 39, "#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#"},
        {"dotted", 1, 40, "##.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#"},
        {"dotted", 2, 7, "##...##"},
        {"dotted", 2, 8, "##.##.##"},
        {"dotted", 2, 9, "##.##..##"},
        {"dotted", 2, 10, "##..##..##"},
        {"dotted", 2, 11, "##...##..##"},
        {"dotted", 2, 12, "##.##..##.##"},
        {"dotted", 2, 13, "##.##..##..##"},
        {"dotted", 2, 20, "##.##..##..##..##.##"},
        {"dotted", 2, 21, "##.##..##..##..##..##"},
        {"dotted", 2, 22, "##..##..##..##..##..##"},
        {"dotted", 2, 23, "##...##..##..##..##..##"},
        {"dotted", 2, 24, "##.##..##..##..##..##.##"},
        {"dotted", 2, 25, "##.##..##..##..##..##..##"},
        {"dotted", 2, 37, "##.##..##..##..##..##..##..##..##..##"},
        {"dotted", 2, 38, "##..##..##..##..##..##..##..##..##..##"},
        {"dotted", 2, 39, "##...##..##..##..##..##..##..##..##..##"},
        {"dotted", 2, 40, "##.##..##..##..##..##..##..##..##..##.##"},
        {"dotted", 3, 7, "#######"},
        {"dotted", 3, 8, "###..##+"},
        {"dotted", 3, 9, "###...###"},
        {"dotted", 3, 10, "###....###"},
        {"dotted", 3, 11, "###.....###"},
        {"dotted", 3, 12, "####....####"},
        {"dotted", 3, 13, "###..###..###"},
        {"dotted", 3, 20, "###..###...###...###"},
        {"dotted", 3, 21, "###...###...###...###"},
        {"dotted", 3, 22, "###....###...###...###"},
        {"dotted", 3, 23, "###....###...###....###"},
        {"dotted", 3, 24, "####...###...###....####"},
        {"dotted", 3, 25, "###..###...###...###..###"},
        {"dotted", 3, 37, "###..###...###...###...###...###..###"},
        {"dotted", 3, 38, "###..###...###...###...###...###...###"},
        {"dotted", 3, 39, "###...###...###...###...###...###...###"},
        {"dotted", 3, 40, "###....###...###...###...###...###...###"},
        {"dotted", 4, 7, "####..."},
        {"dotted", 4, 8, "########"},
        {"dotted", 4, 9, "####.####"},
        {"dotted", 4, 10, "####..####"},
        {"dotted", 4, 11, "####...####"},
        {"dotted", 4, 12, "####....####"},
        {"dotted", 4, 13, "####.....####"},
        {"dotted", 4, 20, "####....####....####"},
        {"dotted", 4, 21, "####....+###+....####"},
        {"dotted", 4, 22, "####.....####.....####"},
        {"dotted", 4, 23, "####.....+###+.....####"},
        {"dotted", 4, 24, "####..+###+..+###+..####"},
        {"dotted", 4, 25, "####...####...####...####"},
        {"dotted", 4, 37, "####....+###+...+###+...+###+....####"},
        {"dotted", 4, 38, "####....+###+....####....+###+....####"},
        {"dotted", 4, 39, "####....+###+....+###+....+###+....####"},
        {"dotted", 4, 40, "####...+###+..+###+..+###+..+###+...####"},
        {"dashed", 1, 7, "##+.+##"},
        {"dashed", 1, 8, "###..###"},
        {"dashed", 1, 9, "###...###"},
        {"dashed", 1, 10, "###+##+###"},
        {"dashed", 1, 11, "###.###.###"},
        {"dashed", 1, 12, "###.+##+.###"},
        {"dashed", 1, 13, "###..###..###"},
        {"dashed", 1, 20, "###..+##+..+##+..###"},
        {"dashed", 1, 21, "###.+##+.###.+##+.###"},
        {"dashed", 1, 22, "###.+##+.+##+.+##+.###"},
        {"dashed", 1, 23, "###..###..###..###..###"},
        {"dashed", 1, 24, "###..+##+.+##+.+##+..###"},
        {"dashed", 1, 25, "###..+##+..###..+##+..###"},
        {"dashed", 1, 37, "###.+###.+##+.+##+.+##+.+##+.###+.###"},
        {"dashed", 1, 38, "###..###..###..###..###..###..###..###"},
        {"dashed", 1, 39, "###..###+.+##+.+##+.+##+.+##+.+###..###"},
        {"dashed", 1, 40, "###..+##+.+##+.+###..###+.+##+.+##+..###"},
        {"dashed", 2, 7, "#######"},
        {"dashed", 2, 8, "########"},
        {"dashed", 2, 9, "#########"},
        {"dashed", 2, 10, "##########"},
        {"dashed", 2, 11, "###########"},
        {"dashed", 2, 12, "############"},
        {"dashed", 2, 13, "#####...#####"},
        {"dashed", 2, 20, "######.######.######"},
        {"dashed", 2, 21, "######.+#####+.######"},
        {"dashed", 2, 22, "######..######..######"},
        {"dashed", 2, 23, "######..+#####+..######"},
        {"dashed", 2, 24, "######...######...######"},
        {"dashed", 2, 25, "######...+#####+...######"},
        {"dashed", 2, 37, "######....+#####+...+#####+....######"},
        {"dashed", 2, 38, "######....+#####+....+#####+....######"},
        {"dashed", 2, 39, "######.....######.....######.....######"},
        {"dashed", 2, 40, "######.....+#####+....+#####+.....######"},
        {"dashed", 3, 7, "#######"},
        {"dashed", 3, 8, "########"},
        {"dashed", 3, 9, "#########"},
        {"dashed", 3, 10, "##########"},
        {"dashed", 3, 11, "###########"},
        {"dashed", 3, 12, "############"},
        {"dashed", 3, 13, "#####+.+#####"},
        {"dashed", 3, 20, "######.######.######"},
        {"dashed", 3, 21, "######.+#####+.######"},
        {"dashed", 3, 22, "######..######..######"},
        {"dashed", 3, 23, "######..+#####+..######"},
        {"dashed", 3, 24, "######...######...######"},
        {"dashed", 3, 25, "######...+#####+...######"},
        {"dashed", 3, 37, "######.+#####+.+#####+.+#####+.######"},
        {"dashed", 3, 38, "######..######..######..######..######"},
        {"dashed", 3, 39, "######..+#####+.+#####+.+#####+..######"},
        {"dashed", 3, 40, "######..+#####+..######..+#####+..######"},
    };

    // How much of the device pixel square [x, x + 1) x [y, y + 1) the shapes cover: 16 x 16 samples.
    double coverage(const std::vector<weft::SceneDash>& shapes, int x, int y) {
        constexpr int n = 16;
        int inside{};
        for (int sy = 0; sy < n; ++sy) {
            for (int sx = 0; sx < n; ++sx) {
                const double px = x + (sx + .5) / n;
                const double py = y + (sy + .5) / n;
                for (const auto& s : shapes) {
                    const auto& r = s.rect;
                    bool hit{};
                    if (s.round) {
                        const double cx = r[0] + r[2] / 2;
                        const double cy = r[1] + r[3] / 2;
                        const double radius = r[2] / 2;
                        hit = (px - cx) * (px - cx) + (py - cy) * (py - cy) <= radius * radius;
                    } else {
                        hit = px >= r[0] && px < r[0] + r[2] && py >= r[1] && py < r[1] + r[3];
                    }
                    if (hit) {
                        ++inside;
                        break;
                    }
                }
            }
        }
        return double(inside) / (n * n);
    }
} // namespace

int main() {
    try {
        std::size_t failures{};
        for (const auto& row : rows) {
            const auto style =
                std::string(row.style) == "dotted" ? weft::SceneBorderStyle::Dotted : weft::SceneBorderStyle::Dashed;
            // The box of the probe: the row's length wide, four px above its bottom border.
            const float height = float(row.width + 4);
            const auto shapes =
                weft::scene_dashed_side({float(row.length), height}, 2, float(row.width), style, {1, 1, 0, 0});
            std::string got;
            for (int x = 0; x < row.length; ++x) {
                double value{};
                for (int y = 4; y < 4 + row.width; ++y) {
                    value = std::max(value, coverage(shapes, x, y));
                }
                const double level = value * 255;
                got += level >= 200 ? '#' : level > 30 ? '+' : '.';
            }
            // Round dots: Skia's coverage of a dot's edge pixels differs from the exact circle's by a few levels, so
            // only the pixels a dot fills must match; their centres agree to a thousandth of a pixel.
            const bool round = style == weft::SceneBorderStyle::Dotted && row.width > 3;
            const auto filled = [](std::string s) {
                for (auto& ch : s) {
                    ch = ch == '#' ? '#' : '.';
                }
                return s;
            };
            if (round ? filled(got) != filled(row.pixels) : got != row.pixels) {
                ++failures;
                std::cout << row.style << ' ' << row.width << " px, " << row.length << ": " << got << ", Chrome "
                          << row.pixels << '\n';
            }
        }
        std::cout << std::size(rows) << " dotted and dashed sides against Chrome: " << failures << " differ\n";
        require(failures == 0, "dotted and dashed sides as Chrome draws them");
        std::cout << "PASS scene dotted and dashed border sides\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
