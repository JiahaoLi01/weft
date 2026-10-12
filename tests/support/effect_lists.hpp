#pragma once

#include "effects/effect_color.hpp"
#include "effects/effect_parameters.hpp"
#include "effects/effect_repeat.hpp"
#include "effects/effect_textures.hpp"
#include "effects/effect_uv.hpp"
#include "render/srgb.hpp"
#include "render_lists.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

// Display lists of the test effects (tests/effects/*.hlsl) that every renderer draws on a transparent target, and their
// CPU expectations: where in its box an effect runs, the record field of each parameter and the time, textures read as
// data and as color and repeated, a box drawn after effects, and an effect beside the box it reproduces under one
// group, rounded clip, rotation and corners.
namespace weft::testing {
    inline constexpr unsigned effect_width = 64;
    inline constexpr unsigned effect_height = 48;

    inline paint::Item item_at(paint::Item::Kind kind, std::uint32_t payload, float x, float y, float w, float h) {
        paint::Item item;
        item.kind = kind;
        item.transform = {1, 0, 0, 1, x, y};
        item.width = w;
        item.height = h;
        item.clip = {0, 0, float(effect_width), float(effect_height)};
        item.payload = payload;
        return item;
    }

    inline paint::Item effect_item(std::uint32_t payload, float x, float y, float w, float h) {
        return item_at(paint::Item::Kind::Effect, payload, x, y, w, h);
    }

    // The uv effect over a 40 x 24 box at (12.25, 8.75).
    inline paint::DisplayList uv_list() {
        paint::DisplayList list;
        list.programs.push_back(effects::effect_uv());
        list.effects.push_back({0, 0, {}, {}, {}});
        list.items.push_back(effect_item(0, 12.25f, 8.75f, 40, 24));
        return list;
    }

    // Opaque (u, v, 0) at the pixel centers inside the box, nothing outside: a linear pass's box without corners or
    // rotation covers the pixels whose centers it contains, wherever its edges fall.
    inline Pixel uv_expected(unsigned x, unsigned y) {
        const float u = (x + .5f - 12.25f) / 40;
        const float v = (y + .5f - 8.75f) / 24;
        return u >= 0 && u < 1 && v >= 0 && v < 1 ? encode({u, v, 0, 1}) : Pixel{};
    }

    // The parameters the parameters effect takes: a different opaque linear color for each record field.
    inline std::array<float, 4> effect_parameter_value(unsigned i) {
        return {i / 15.f, 1 - i / 15.f, float((i * 7) % 16) / 15, 1};
    }

    // Sixteen 8 x 8 boxes in two rows, box i at time i with every parameter: each shows parameter i.
    inline paint::DisplayList parameters_list() {
        paint::DisplayList list;
        list.programs.push_back(effects::effect_parameters());
        for (unsigned i = 0; i < 16; ++i) {
            paint::Effect effect{0, float(i) + .5f, {}, {}, {}};
            for (unsigned p = 0; p < 16; ++p) {
                effect.parameters.push_back(effect_parameter_value(p));
            }
            list.effects.push_back(effect);
            list.items.push_back(effect_item(i, float(i % 8) * 8, float(i / 8) * 8, 8, 8));
        }
        return list;
    }

    inline Pixel parameters_expected(unsigned x, unsigned y) {
        if (y >= 16) {
            return {};
        }
        const auto v = effect_parameter_value((y / 8) * 8 + x / 8);
        return encode({v[0], v[1], v[2], v[3]});
    }

    // A 16 x 16 data texture and a 16 x 16 opaque sRGB image. Every list shares the same two assets, so that a caching
    // renderer meets them again in later frames.
    inline std::uint8_t effect_data(unsigned x, unsigned y) {
        return std::uint8_t(x * 16 + y);
    }

    inline std::array<std::uint8_t, 4> effect_color(unsigned x, unsigned y) {
        return {std::uint8_t(x * 15), std::uint8_t(255 - y * 15), 128, 255};
    }

    inline const std::shared_ptr<const Asset>& effect_data_asset() {
        static const auto asset = [] {
            std::vector<std::uint8_t> data(16 * 16);
            for (unsigned y = 0; y < 16; ++y) {
                for (unsigned x = 0; x < 16; ++x) {
                    data[y * 16 + x] = effect_data(x, y);
                }
            }
            return std::make_shared<const Asset>(Asset{16, 16, Encoding::Coverage, std::move(data)});
        }();
        return asset;
    }

    inline const std::shared_ptr<const Asset>& effect_color_asset() {
        static const auto asset = [] {
            std::vector<std::uint8_t> color(16 * 16 * 4);
            for (unsigned y = 0; y < 16; ++y) {
                for (unsigned x = 0; x < 16; ++x) {
                    const auto c = effect_color(x, y);
                    std::copy(c.begin(), c.end(), color.begin() + (y * 16 + x) * 4);
                }
            }
            return std::make_shared<const Asset>(Asset{16, 16, Encoding::SrgbStraight, std::move(color)});
        }();
        return asset;
    }

    // The data texture as a coverage image: a caching renderer places it on its atlas.
    inline paint::DisplayList atlas_list() {
        paint::DisplayList list;
        list.assets.push_back(effect_data_asset());
        list.images.emplace_back().asset = 0;
        list.items.push_back(item_at(paint::Item::Kind::Image, 0, 8, 8, 16, 16));
        return list;
    }

    // The textures effect under a 16 x 16 box at (8, 8), texel per CSS px; the repeat effect over the color texture
    // under a 32 x 32 box at (28, 8); then a blue box from (4, 42) to (60, 46), drawn as a box after the effects.
    inline paint::DisplayList textures_list() {
        paint::DisplayList list;
        list.assets = {effect_data_asset(), effect_color_asset()};
        list.programs = {effects::effect_textures(), effects::effect_repeat()};
        list.effects.push_back({0, 0, {}, {0, 1}, {}});
        list.effects.push_back({1, 0, {}, {1}, {}});
        list.items.push_back(effect_item(0, 8, 8, 16, 16));
        list.items.push_back(effect_item(1, 28, 8, 32, 32));
        list.boxes.emplace_back().background = {0, 0, 1, 1};
        list.items.push_back(item_at(paint::Item::Kind::Box, 0, 4, 42, 56, 4));
        return list;
    }

    // In the first box the color texel scaled by the data texel; in the second the mean of the two color texels beside
    // the sample, the last texel's neighbor the first; linear premultiplied. Then the blue box.
    inline Pixel textures_expected(unsigned x, unsigned y) {
        const auto light = [](const std::array<std::uint8_t, 4>& c, unsigned channel) {
            return render::srgb_to_linear(c[channel] / 255.f);
        };
        if (x >= 8 && x < 24 && y >= 8 && y < 24) {
            const auto d = effect_data(x - 8, y - 8) / 255.f;
            const auto c = effect_color(x - 8, y - 8);
            return encode({light(c, 0) * d, light(c, 1) * d, light(c, 2) * d, d});
        }
        if (x >= 28 && x < 60 && y >= 8 && y < 40) {
            const auto a = effect_color((x - 28) % 16, (y - 8) % 16);
            const auto b = effect_color((x - 27) % 16, (y - 8) % 16);
            return encode(
                {(light(a, 0) + light(b, 0)) / 2, (light(a, 1) + light(b, 1)) / 2, (light(a, 2) + light(b, 2)) / 2, 1});
        }
        if (x >= 4 && x < 60 && y >= 42 && y < 46) {
            return encode({0, 0, 1, 1});
        }
        return {};
    }

    // In a group at half opacity, under a rounded clip, a rotated box with corners at .8 opacity: the color effect, or
    // the box with the same color as its background. Both draw alike.
    inline paint::DisplayList composed_list(bool effect) {
        constexpr paint::Color color{.8f, .3f, .2f, .7f};
        paint::DisplayList list;
        weft::RoundedClip clip;
        clip.rect = {6, 4, 52, 40};
        clip.radius_x = {10, 4, 12, 6};
        clip.radius_y = {10, 4, 12, 6};
        list.clips.push_back(clip);
        auto& begin = list.items.emplace_back();
        begin.kind = paint::Item::Kind::BeginGroup;
        paint::Item item;
        const float angle = .35f;
        item.transform = {std::cos(angle), std::sin(angle), -std::sin(angle), std::cos(angle), 22, 2};
        item.width = 36;
        item.height = 26;
        item.clip = {0, 0, float(effect_width), float(effect_height)};
        item.opacity = .8f;
        item.clips_count = 1;
        const std::array<float, 4> radii{6, 3, 8, 0};
        if (effect) {
            item.kind = paint::Item::Kind::Effect;
            list.programs.push_back(effects::effect_color());
            list.effects.push_back({0,
                                    0,
                                    {{render::srgb_to_linear(color[0]), render::srgb_to_linear(color[1]),
                                      render::srgb_to_linear(color[2]), color[3]}},
                                    {},
                                    radii});
        } else {
            item.kind = paint::Item::Kind::Box;
            auto& box = list.boxes.emplace_back();
            box.background = color;
            box.radii = radii;
        }
        list.items.push_back(item);
        auto& end = list.items.emplace_back();
        end.kind = paint::Item::Kind::EndGroup;
        end.opacity = .5f;
        end.width = float(effect_width);
        end.height = float(effect_height);
        end.clip = {0, 0, float(effect_width), float(effect_height)};
        return list;
    }

    // Whether every pixel of an effect list's target is within levels of expected(x, y); reports the first that is not.
    template <class Expected>
    bool effect_matches(const char* what, const std::vector<Pixel>& pixels, Expected&& expected, int levels) {
        for (unsigned y = 0; y < effect_height; ++y) {
            for (unsigned x = 0; x < effect_width; ++x) {
                const auto& actual = pixels[std::size_t(y) * effect_width + x];
                const auto want = expected(x, y);
                if (!close_to(actual, want, levels)) {
                    std::cerr << what << ": pixel (" << x << ", " << y << ") is " << int(actual[0]) << ' '
                              << int(actual[1]) << ' ' << int(actual[2]) << ' ' << int(actual[3]) << ", expected "
                              << int(want[0]) << ' ' << int(want[1]) << ' ' << int(want[2]) << ' ' << int(want[3])
                              << '\n';
                    return false;
                }
            }
        }
        return true;
    }

    // The largest channel difference of two effect list targets, and how many pixels the first covers.
    inline std::array<int, 2> effect_difference(const std::vector<Pixel>& a, const std::vector<Pixel>& b) {
        int worst{};
        int covered{};
        for (std::size_t i = 0; i < a.size(); ++i) {
            for (unsigned c = 0; c < 4; ++c) {
                worst = std::max(worst, std::abs(int(a[i][c]) - int(b[i][c])));
            }
            covered += a[i][3] != 0;
        }
        return {worst, covered};
    }
} // namespace weft::testing
