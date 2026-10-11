#pragma once

#include "probe_common.hpp"
#include "probe_constants.hpp"
#include "layout/page_display_list.hpp"
#include "render/quad_lowering.hpp"

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

    inline void crop_draw(Draw& draw, const std::array<float, 4>& clip) {
        const auto old = draw.rect;
        const float x = std::max(old.x, clip[0]);
        const float y = std::max(old.y, clip[1]);
        // Large device-space origins can make (x+w)-x slightly larger than w.
        // An intersected quad cannot exceed its source extent or UV interval.
        const float w = std::min(old.w, std::max(0.f, std::min(old.x + old.w, clip[0] + clip[2]) - x));
        const float h = std::min(old.h, std::max(0.f, std::min(old.y + old.h, clip[1] + clip[3]) - y));
        if (old.w <= 0 || old.h <= 0) {
            draw.rect.w = draw.rect.h = 0;
            return;
        }
        draw.uv = {draw.uv[0] + draw.uv[2] * (x - old.x) / old.w, draw.uv[1] + draw.uv[3] * (y - old.y) / old.h,
                   draw.uv[2] * w / old.w, draw.uv[3] * h / old.h};
        draw.rect = {x, y, w, h};
    }

    // Glyph images and rounded-clip masks kept across packets of one font.
    struct CheetahPacketCache {
        weft::GlyphImages glyphs;
        weft::render::QuadMasks masks;
    };

    // The page's display list as quads (src/layout/page_display_list.hpp, src/render/quad_lowering.hpp).
    template <class FontT>
    FramePacket cheetah_packet(const weft::PositionedPage& page, FontT& font,
                               const std::map<std::string, std::shared_ptr<const Asset>>& images,
                               CheetahPacketCache* persistent = nullptr) {
        // Masks combine the previous font's glyph images; another font starts both over.
        if (persistent && persistent->glyphs.bind(&font)) {
            persistent->masks.masks.clear();
            persistent->masks.bytes = 0;
        }
        const auto list = weft::page_display_list(page, font, images, persistent ? &persistent->glyphs : nullptr);
        auto packet = weft::render::lower_to_quads(list, persistent ? &persistent->masks : nullptr);
        require(packet.assets.size() < weft::probe_constants::max_frame_assets,
                "bounded game image/glyph/mask resources");
        return packet;
    }

    // Independent CPU sample oracle for bilinear premultiplied-linear image data.
    inline Pixel cheetah_sample(const FramePacket& packet, int x, int y) {
        Color destination{0, 0, 0, 0};
        for (const auto& d : packet.draws) {
            if (x + .5f < d.rect.x || y + .5f < d.rect.y || x + .5f >= d.rect.x + d.rect.w ||
                y + .5f >= d.rect.y + d.rect.h) {
                continue;
            }
            Color source{decode(d.srgb[0]) * d.srgb[3], decode(d.srgb[1]) * d.srgb[3], decode(d.srgb[2]) * d.srgb[3],
                         d.srgb[3]};
            if (d.asset >= 0) {
                const auto& a = *packet.assets[d.asset];
                const float tx = (d.uv[0] + (x + .5f - d.rect.x) / d.rect.w * d.uv[2]) * a.width - .5f;
                const float ty = (d.uv[1] + (y + .5f - d.rect.y) / d.rect.h * d.uv[3]) * a.height - .5f;
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
                    for (auto& c : source) {
                        c *= sample[3];
                    }
                } else {
                    for (int c = 0; c < 4; ++c) {
                        source[c] = sample[c];
                    }
                }
            }
            for (auto& component : source) {
                component *= d.opacity;
            }
            for (int c = 0; c < 4; ++c) {
                destination[c] = source[c] + destination[c] * (1 - source[3]);
            }
        }
        Pixel result{};
        for (int c = 0; c < 4; ++c) {
            const float v = std::clamp(destination[c], 0.f, 1.f);
            const float e =
                c == 3 ? v
                : v <= weft::probe_constants::srgb_encode_threshold
                    ? v * weft::probe_constants::srgb_linear_slope
                    : weft::probe_constants::srgb_scale * std::pow(v, 1 / weft::probe_constants::srgb_gamma) -
                          weft::probe_constants::srgb_offset;
            result[c] = std::uint8_t(std::lround(e * weft::constants::color_channel_max));
        }
        return result;
    }
} // namespace probes
