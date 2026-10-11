#pragma once

#include "probe_common.hpp"
#include "probe_constants.hpp"
#include "layout/page_display_list.hpp"
#include "render/quad_list.hpp"
#include "render/rounded_clip.hpp"

namespace probes {
    // Real captured TSX -> native DOM -> positioned CSS values -> backend C ABI.

    inline std::map<std::string, std::shared_ptr<const Asset>> load_cheetah_images() {
        std::map<std::string, std::shared_ptr<const Asset>> result;
        for (const auto* name : {"ink-panel-v2.png", "ink-dock-left-v1.png", "ink-dock-right-v1.png"}) {
            // Explicit host resource allowlist, no DOM-driven filesystem access.
            const auto path =
                std::filesystem::path(WEFT_SOURCE_DIR) / "tests/fixtures/casecheetah/UI/public/art" / name;
            auto image = weft::testing::read_png(path);
            require(image.width == weft::probe_constants::cheetah_art_width &&
                        image.height == weft::probe_constants::cheetah_art_height,
                    "fixed source art dimensions");
            result.emplace(std::string("art/") + name,
                           std::make_shared<Asset>(
                               Asset{image.width, image.height, Encoding::SrgbStraight, std::move(image.rgba)}));
        }
        return result;
    }

    // Glyph images kept across packets of one font.
    struct CheetahPacketCache {
        weft::GlyphImages glyphs;
    };

    // The page's display list (src/layout/page_display_list.hpp) as a list packet.
    template <class FontT>
    FramePacket cheetah_packet(const weft::PositionedPage& page, FontT& font,
                               const std::map<std::string, std::shared_ptr<const Asset>>& images,
                               CheetahPacketCache* persistent = nullptr) {
        FramePacket packet;
        packet.list = std::make_shared<const weft::paint::DisplayList>(
            weft::page_display_list(page, font, images, persistent ? &persistent->glyphs : nullptr));
        require(packet.list->assets.size() < weft::probe_constants::max_frame_assets,
                "bounded game image/glyph resources");
        return packet;
    }

    // An expected pixel, and whether a rounded clip edge passes within a pixel of its center: there the GPU's analytic
    // antialiasing and the oracle's area coverage differ by design.
    struct OracleSample {
        Pixel pixel{};
        bool rounded_edge{};
    };

    // Independent CPU oracle of a list of translated boxes and images at pixel (x, y): pixel-center inclusion in each
    // item and its clip, bilinear premultiplied-linear sampling of its asset, and the area of its rounded clips over
    // subpixel samples, composited in linear premultiplied color.
    inline OracleSample cheetah_sample(const weft::paint::DisplayList& list, int x, int y) {
        constexpr int subpixels = weft::probe_constants::oracle_clip_subpixels;
        const float cx = x + .5f;
        const float cy = y + .5f;
        OracleSample result;
        // Fraction of the samples of the square of half-extent `half` around the center inside every rounded clip of
        // item.
        auto clip_area = [&](const weft::paint::Item& item, float half) {
            int inside{};
            for (int sy = 0; sy < subpixels; ++sy) {
                for (int sx = 0; sx < subpixels; ++sx) {
                    const float px = cx - half + 2 * half * (sx + .5f) / subpixels;
                    const float py = cy - half + 2 * half * (sy + .5f) / subpixels;
                    bool all = true;
                    for (std::uint32_t c = 0; all && c < item.clips_count; ++c) {
                        all = weft::contains_rounded(weft::normalize_rounded(list.clips[item.clips_begin + c]), px, py);
                    }
                    inside += all;
                }
            }
            return float(inside) / (subpixels * subpixels);
        };
        Color destination{0, 0, 0, 0};
        for (const auto& item : list.items) {
            const auto& m = item.transform;
            require(m[0] == 1 && m[1] == 0 && m[2] == 0 && m[3] == 1, "document items are translated");
            const auto& clip = item.clip;
            if (cx < m[4] || cy < m[5] || cx >= m[4] + item.width || cy >= m[5] + item.height || cx < clip[0] ||
                cy < clip[1] || cx >= clip[2] || cy >= clip[3]) {
                continue;
            }
            float coverage = 1;
            if (item.clips_count) {
                coverage = clip_area(item, .5f);
                const float neighborhood = clip_area(item, 1);
                result.rounded_edge = result.rounded_edge || (neighborhood > 0 && neighborhood < 1);
            }
            Color source{};
            if (item.kind == weft::paint::Item::Kind::Box) {
                const auto& c = list.boxes[item.payload].background;
                source = {decode(c[0]) * c[3], decode(c[1]) * c[3], decode(c[2]) * c[3], c[3]};
            } else {
                require(item.kind == weft::paint::Item::Kind::Image, "document lists hold boxes and images");
                const auto& image = list.images[item.payload];
                const auto& a = *list.assets[image.asset];
                const float tx = (image.uv[0] + (cx - m[4]) / item.width * image.uv[2]) * a.width - .5f;
                const float ty = (image.uv[1] + (cy - m[5]) / item.height * image.uv[3]) * a.height - .5f;
                const int ix = int(std::floor(tx));
                const int iy = int(std::floor(ty));
                const float fx = tx - ix;
                const float fy = ty - iy;
                Color sample{};
                for (int yy = 0; yy < 2; ++yy) {
                    for (int xx = 0; xx < 2; ++xx) {
                        const auto px = std::clamp(ix + xx, 0, int(a.width) - 1);
                        const auto py = std::clamp(iy + yy, 0, int(a.height) - 1);
                        const auto index = std::size_t(py) * a.width + px;
                        const float weight = (xx ? fx : 1 - fx) * (yy ? fy : 1 - fy);
                        if (a.encoding == Encoding::Coverage) {
                            sample[3] += a.bytes[index] / weft::probe_constants::color_channel_max_float * weight;
                        } else {
                            const float alpha = a.bytes[index * 4 + 3] / weft::probe_constants::color_channel_max_float;
                            for (int c = 0; c < 3; ++c) {
                                sample[c] +=
                                    decode(a.bytes[index * 4 + c] / weft::probe_constants::color_channel_max_float) *
                                    alpha * weight;
                            }
                            sample[3] += alpha * weight;
                        }
                    }
                }
                if (a.encoding == Encoding::Coverage) {
                    const auto& c = image.color;
                    source = {decode(c[0]) * c[3] * sample[3], decode(c[1]) * c[3] * sample[3],
                              decode(c[2]) * c[3] * sample[3], c[3] * sample[3]};
                } else {
                    source = sample;
                }
            }
            for (auto& component : source) {
                component *= item.opacity * coverage;
            }
            for (int c = 0; c < 4; ++c) {
                destination[c] = source[c] + destination[c] * (1 - source[3]);
            }
        }
        for (int c = 0; c < 4; ++c) {
            const float v = std::clamp(destination[c], 0.f, 1.f);
            const float e =
                c == 3 ? v
                : v <= weft::probe_constants::srgb_encode_threshold
                    ? v * weft::probe_constants::srgb_linear_slope
                    : weft::probe_constants::srgb_scale * std::pow(v, 1 / weft::probe_constants::srgb_gamma) -
                          weft::probe_constants::srgb_offset;
            result.pixel[c] = std::uint8_t(std::lround(e * weft::constants::color_channel_max));
        }
        return result;
    }

    // The oracle of a list packet, or of a quad packet's list.
    inline OracleSample cheetah_sample(const FramePacket& packet, int x, int y) {
        if (packet.list) {
            return cheetah_sample(*packet.list, x, y);
        }
        constexpr unsigned unbounded = weft::probe_constants::max_surface_dimension;
        return cheetah_sample(weft::render::quad_list(packet, 0, unbounded, unbounded), x, y);
    }

    // The largest channel difference in linear light: color channels decoded from sRGB, alpha as stored.
    inline float linear_difference(const Pixel& a, const Pixel& b) {
        float difference = std::abs(int(a[3]) - int(b[3])) / weft::probe_constants::color_channel_max_float;
        for (int c = 0; c < 3; ++c) {
            difference = std::max(difference, std::abs(decode(a[c] / weft::probe_constants::color_channel_max_float) -
                                                       decode(b[c] / weft::probe_constants::color_channel_max_float)));
        }
        return difference;
    }

    // Folds the difference of actual from expected into the interior maximum in 8-bit levels, or at a rounded clip
    // edge into the maximum in linear light.
    inline void compare_oracle(const OracleSample& expected, const Pixel& actual, int& interior, float& rounded_edge) {
        if (expected.rounded_edge) {
            rounded_edge = std::max(rounded_edge, linear_difference(actual, expected.pixel));
            return;
        }
        for (int c = 0; c < 4; ++c) {
            interior = std::max(interior, std::abs(int(actual[c]) - int(expected.pixel[c])));
        }
    }
} // namespace probes
