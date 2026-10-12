#pragma once

#include "paint/display_list.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

// The CRPG UI's SmokeBg tick (src/components/SmokeBg.tsx of the captured UI) on the CPU, in doubles as JavaScript runs
// it: the alpha byte of each canvas pixel at time t. The color of every pixel is the rarity color; the page stretches
// the canvas over its box.
namespace weft::testing {
    struct SmokeParameters {
        double cloud_scale = 7.2;
        double noise_scale = 2.8;
        double cloud_speed_x = 1;
        double cloud_speed_y = 1;
        double noise_speed_x = -6;
        double noise_speed_y = -10;
        double gamma = .1;
        double noise_strength = .3;
        double fade_power = 2;
        bool fade_right{};
        bool rotate90{};
        unsigned width = 48;
        unsigned height = 64;
    };

    inline std::vector<std::uint8_t> smoke_canvas(const paint::Asset& cloud, const paint::Asset& noise,
                                                  const SmokeParameters& p, double t) {
        const auto w = double(p.width);
        const auto h = double(p.height);
        std::array<std::uint8_t, 256> gamma{};
        for (unsigned i = 0; i < 256; ++i) {
            gamma[i] = std::uint8_t(int(std::pow(i / 255.0, p.gamma) * 255));
        }
        // A Float32Array of the fade along its axis.
        std::vector<float> fade(std::max(p.width, p.height));
        for (unsigned i = 0; i < (p.fade_right ? p.width : p.height); ++i) {
            fade[i] = float(p.fade_right ? std::pow(i / w, p.fade_power) : std::pow(1 - i / h, p.fade_power));
        }
        const double cw = cloud.width;
        const double ch = cloud.height;
        const double nw = noise.width;
        const double nh = noise.height;
        const double cloud_per_unit = cw / p.cloud_scale;
        const double noise_per_unit = nw / p.noise_scale;
        const double cox = t * p.cloud_speed_x * (cloud_per_unit / w);
        const double coy = t * p.cloud_speed_y * (cloud_per_unit / h);
        const double nox = t * p.noise_speed_x * (noise_per_unit / w);
        const double noy = t * p.noise_speed_y * (noise_per_unit / h);
        const bool use_noise = p.noise_strength > .01;
        const double floor_noise = 1 - p.noise_strength;
        // ((x % size) + size) % size, as JavaScript's % on doubles.
        const auto wrap = [](double x, double size) {
            return std::fmod(std::fmod(x, size) + size, size);
        };
        std::vector<std::uint8_t> alpha(std::size_t(p.width) * p.height);
        for (unsigned py = 0; py < p.height; ++py) {
            const double cloud_row = (py / h) * cloud_per_unit + (p.rotate90 ? cox : coy);
            const double noise_row = (py / h) * noise_per_unit + (p.rotate90 ? nox : noy);
            for (unsigned px = 0; px < p.width; ++px) {
                const double cloud_column = (px / w) * cloud_per_unit + (p.rotate90 ? coy : cox);
                const double cu = wrap(p.rotate90 ? cloud_row : cloud_column, cw);
                const double cv = wrap(p.rotate90 ? cloud_column : cloud_row, ch);
                int lum = gamma[cloud.bytes[std::size_t(int(cv)) * cloud.width + std::size_t(int(cu))]];
                if (use_noise) {
                    const double noise_column = (px / w) * noise_per_unit + (p.rotate90 ? noy : nox);
                    const double nu = wrap(p.rotate90 ? noise_row : noise_column, nw);
                    const double nv = wrap(p.rotate90 ? noise_column : noise_row, nh);
                    const double n = noise.bytes[std::size_t(int(nv)) * noise.width + std::size_t(int(nu))] / 255.0;
                    lum = int(lum * (floor_noise + p.noise_strength * n));
                }
                const double f = p.fade_right ? fade[px] : fade[py];
                alpha[std::size_t(py) * p.width + px] = std::uint8_t(int(lum * f));
            }
        }
        return alpha;
    }
} // namespace weft::testing
