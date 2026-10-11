#pragma once

#include "paint/display_list.hpp"
#include "render/srgb.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

// Display lists the renderer tests draw on every backend, and the CPU expectations of their pixels.
namespace weft::testing {
    namespace paint = weft::paint;
    using Asset = paint::Asset;
    using Encoding = paint::Encoding;
    using Pixel = std::array<std::uint8_t, 4>;
    using Linear = std::array<float, 4>;

    // Every channel value at every alpha: R = x, G = 255 - x, B = 7x mod 256, A = y.
    inline constexpr unsigned conversion_size = 256;

    inline std::shared_ptr<const Asset> conversion_asset() {
        std::vector<std::uint8_t> bytes(std::size_t(conversion_size) * conversion_size * 4);
        for (unsigned y = 0; y < conversion_size; ++y) {
            for (unsigned x = 0; x < conversion_size; ++x) {
                auto* p = bytes.data() + (std::size_t(y) * conversion_size + x) * 4;
                p[0] = std::uint8_t(x);
                p[1] = std::uint8_t(255 - x);
                p[2] = std::uint8_t(x * 7);
                p[3] = std::uint8_t(y);
            }
        }
        return std::make_shared<const Asset>(
            Asset{conversion_size, conversion_size, Encoding::SrgbStraight, std::move(bytes)});
    }

    // The CPU conversion the renderer used before srgb_convert.hlsl: decode table times byte / 255.
    inline std::array<float, 4> reference_texel(const std::uint8_t* p) {
        const float alpha = p[3] / 255.0f;
        std::array<float, 4> out{};
        for (unsigned c = 0; c < 3; ++c) {
            out[c] = weft::render::srgb_to_linear(p[c] / 255.0f) * alpha;
        }
        out[3] = alpha;
        return out;
    }

    // Linear premultiplied light of a straight sRGB color.
    inline Linear linear(const paint::Color& c) {
        return {weft::render::srgb_to_linear(c[0]) * c[3], weft::render::srgb_to_linear(c[1]) * c[3],
                weft::render::srgb_to_linear(c[2]) * c[3], c[3]};
    }

    inline Linear scaled(Linear c, float k) {
        for (auto& v : c) {
            v *= k;
        }
        return c;
    }

    // Source-over of premultiplied colors.
    inline Linear over(const Linear& top, const Linear& under) {
        Linear out{};
        for (unsigned c = 0; c < 4; ++c) {
            out[c] = top[c] + under[c] * (1 - top[3]);
        }
        return out;
    }

    // What an sRGB RGBA8 target stores for a linear premultiplied color.
    inline Pixel encode(const Linear& c) {
        Pixel out{};
        for (unsigned i = 0; i < 4; ++i) {
            const float v = std::clamp(c[i], 0.f, 1.f);
            out[i] = std::uint8_t(std::lround((i == 3 ? v : weft::render::linear_to_srgb(v)) * 255));
        }
        return out;
    }

    inline bool close_to(const Pixel& actual, const Pixel& expected, int levels) {
        for (std::size_t c = 0; c < 4; ++c) {
            if (std::abs(int(actual[c]) - int(expected[c])) > levels) {
                std::cerr << "pixel " << int(actual[0]) << ',' << int(actual[1]) << ',' << int(actual[2]) << ','
                          << int(actual[3]) << " expected " << int(expected[0]) << ',' << int(expected[1]) << ','
                          << int(expected[2]) << ',' << int(expected[3]) << '\n';
                return false;
            }
        }
        return true;
    }

    // The bilinear premultiplied-linear sample of page_list's picture (conversion_asset over uv {.25, .25, .5, .5},
    // drawn 24 px square at (48, 4)) at a pixel center.
    inline Linear picture_sample(float x, float y) {
        const auto asset = conversion_asset();
        const float tx = (.25f + (x - 48) / 24 * .5f) * conversion_size - .5f;
        const float ty = (.25f + (y - 4) / 24 * .5f) * conversion_size - .5f;
        const int ix = int(std::floor(tx));
        const int iy = int(std::floor(ty));
        Linear out{};
        for (int yy = 0; yy < 2; ++yy) {
            for (int xx = 0; xx < 2; ++xx) {
                const auto texel =
                    reference_texel(asset->bytes.data() + (std::size_t(iy + yy) * conversion_size + ix + xx) * 4);
                const float weight = (xx ? tx - ix : 1 - (tx - ix)) * (yy ? ty - iy : 1 - (ty - iy));
                for (unsigned c = 0; c < 4; ++c) {
                    out[c] += texel[c] * weight;
                }
            }
        }
        return out;
    }

    // A page-like display list: an opaque backdrop, a translucent box, a tinted coverage mask, a translucent image
    // over part of its asset, signed distances with an outline, and a box and an image cut by a rounded clip.
    inline paint::DisplayList page_list() {
        paint::DisplayList list;
        auto box = [&](paint::Color color, float x, float y, float w, float h) {
            paint::Item item;
            item.kind = paint::Item::Kind::Box;
            item.payload = std::uint32_t(list.boxes.size());
            item.transform = {1, 0, 0, 1, x, y};
            item.width = w;
            item.height = h;
            item.clip = {0, 0, 96, 64};
            list.boxes.emplace_back().background = color;
            list.items.push_back(item);
            return list.items.size() - 1;
        };
        auto image = [&](paint::Image payload, float x, float y, float w, float h) {
            paint::Item item;
            item.kind = paint::Item::Kind::Image;
            item.payload = std::uint32_t(list.images.size());
            item.transform = {1, 0, 0, 1, x, y};
            item.width = w;
            item.height = h;
            item.clip = {0, 0, 96, 64};
            list.images.push_back(payload);
            list.items.push_back(item);
            return list.items.size() - 1;
        };
        std::vector<std::uint8_t> mask(64);
        for (unsigned i = 0; i < mask.size(); ++i) {
            mask[i] = std::uint8_t(i * 4);
        }
        list.assets.push_back(std::make_shared<const Asset>(Asset{8, 8, Encoding::Coverage, mask}));
        list.assets.push_back(conversion_asset());
        std::vector<std::uint8_t> distance(16 * 16);
        for (unsigned y = 0; y < 16; ++y) {
            for (unsigned x = 0; x < 16; ++x) {
                const float d = 5 - std::hypot(x + .5f - 8, y + .5f - 8);
                distance[y * 16 + x] = std::uint8_t(std::clamp(128 + d * 128 / 6, 0.f, 255.f));
            }
        }
        list.assets.push_back(std::make_shared<const Asset>(Asset{16, 16, Encoding::SignedDistance, distance, 6}));
        box({.1f, .12f, .15f, 1}, 0, 0, 96, 64);
        box({1, 1, 1, .5f}, 4, 4, 20, 12);
        paint::Image tinted;
        tinted.asset = 0;
        tinted.color = {.9f, .7f, .2f, 1};
        image(tinted, 28, 4, 16, 16);
        paint::Image picture;
        picture.asset = 1;
        picture.uv = {.25f, .25f, .5f, .5f};
        image(picture, 48, 4, 24, 24);
        paint::Image sdf;
        sdf.asset = 2;
        sdf.color = {1, 1, 1, 1};
        sdf.stroke = 2;
        sdf.outline = {.2f, .4f, 1, 1};
        image(sdf, 76, 4, 16, 16);
        // A rounded clip that cuts both of the next items.
        list.clips.push_back({{8, 30, 40, 30}, {10, 10, 10, 10}, {8, 8, 8, 8}});
        const auto cut_box = box({.8f, .2f, .3f, .9f}, 4, 28, 48, 34);
        paint::Image cut_picture = picture;
        cut_picture.uv = {0, 0, 1, 1};
        const auto cut_image = image(cut_picture, 30, 34, 30, 30);
        for (auto i : {cut_box, cut_image}) {
            list.items[i].clips_count = 1;
        }
        return list;
    }

    // Coverage images of many sizes at fractional positions and scales, interleaved with boxes, over a backdrop: a glyph
    // run as a page draws it.
    inline paint::DisplayList glyph_run() {
        paint::DisplayList list;
        std::uint32_t seed = 0x5eed103d;
        auto next = [&](std::uint32_t range) {
            seed = seed * 1664525u + 1013904223u;
            return (seed >> 8) % range;
        };
        auto add = [&](paint::Item::Kind kind, float x, float y, float w, float h, std::uint32_t payload) {
            auto& item = list.items.emplace_back();
            item.kind = kind;
            item.transform = {1, 0, 0, 1, x, y};
            item.width = w;
            item.height = h;
            item.clip = {0, 0, 96, 64};
            item.payload = payload;
        };
        list.boxes.emplace_back().background = {.1f, .12f, .15f, 1};
        add(paint::Item::Kind::Box, 0, 0, 96, 64, 0);
        for (std::uint32_t i = 0; i < 48; ++i) {
            const auto w = 1 + next(14);
            const auto h = 1 + next(14);
            std::vector<std::uint8_t> coverage(std::size_t(w) * h);
            for (auto& c : coverage) {
                c = std::uint8_t(next(256));
            }
            list.assets.push_back(std::make_shared<const Asset>(Asset{w, h, Encoding::Coverage, coverage}));
            auto& image = list.images.emplace_back();
            image.asset = std::uint32_t(list.assets.size() - 1);
            image.color = {.9f, .8f - i * .01f, .3f, 1};
            const float scale = i % 3 == 0 ? 1 : i % 3 == 1 ? 1.5f : .75f;
            add(paint::Item::Kind::Image, float(next(80)) + next(8) * .125f, float(next(50)) + next(8) * .125f,
                w * scale, h * scale, std::uint32_t(list.images.size() - 1));
            if (i % 8 == 7) {
                list.boxes.emplace_back().background = {.2f, .4f, .9f, .5f};
                add(paint::Item::Kind::Box, float(next(80)), float(next(50)), 8, 6,
                    std::uint32_t(list.boxes.size() - 1));
            }
        }
        return list;
    }

    // A 64 x 32 black list with an isolated group of a red box (when red) under an overlapping blue one at half
    // opacity, and a white box in a group at half opacity inside another at half opacity.
    inline paint::DisplayList groups(bool red) {
        paint::DisplayList list;
        auto add = [&](paint::Item::Kind kind, float opacity = 1) -> paint::Item& {
            auto& item = list.items.emplace_back();
            item.kind = kind;
            item.opacity = opacity;
            item.clip = {0, 0, 64, 32};
            return item;
        };
        auto box = [&](paint::Color color, float x, float y, float w, float h) {
            auto& item = add(paint::Item::Kind::Box);
            item.payload = std::uint32_t(list.boxes.size());
            item.transform = {1, 0, 0, 1, x, y};
            item.width = w;
            item.height = h;
            list.boxes.emplace_back().background = color;
        };
        box({0, 0, 0, 1}, 0, 0, 64, 32);
        add(paint::Item::Kind::BeginGroup);
        if (red) {
            box({1, 0, 0, 1}, 8, 4, 16, 16);
        }
        box({0, 0, 1, 1}, 16, 4, 16, 16);
        add(paint::Item::Kind::EndGroup, .5f);
        add(paint::Item::Kind::BeginGroup);
        add(paint::Item::Kind::BeginGroup);
        box({1, 1, 1, 1}, 40, 4, 16, 16);
        add(paint::Item::Kind::EndGroup, .5f);
        add(paint::Item::Kind::EndGroup, .5f);
        return list;
    }
} // namespace weft::testing
