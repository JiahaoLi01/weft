#include "render/quad_lowering.hpp"

#include "constants/common.hpp"
#include "constants/render.hpp"
#include "render/rounded_clip.hpp"
#include "render/srgb.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <string>

namespace weft::render {
    namespace {
        using Kind = paint::Item::Kind;
        using Color = std::array<float, 4>;

        void require(bool condition, const char* message) {
            if (!condition) {
                throw std::runtime_error(message);
            }
        }

        // Narrows draw to clip (left, top, right, bottom), moving its uv with it.
        void crop(Draw& draw, const paint::ClipRect& clip) {
            const auto old = draw.rect;
            const float x = std::max(old.x, clip[0]);
            const float y = std::max(old.y, clip[1]);
            // Large device-space origins can make (x+w)-x slightly larger than w.
            // An intersected quad cannot exceed its source extent or UV interval.
            const float w = std::min(old.w, std::max(0.f, std::min(old.x + old.w, clip[2]) - x));
            const float h = std::min(old.h, std::max(0.f, std::min(old.y + old.h, clip[3]) - y));
            if (old.w <= 0 || old.h <= 0) {
                draw.rect.w = draw.rect.h = 0;
                return;
            }
            draw.uv = {draw.uv[0] + draw.uv[2] * (x - old.x) / old.w, draw.uv[1] + draw.uv[3] * (y - old.y) / old.h,
                       draw.uv[2] * w / old.w, draw.uv[3] * h / old.h};
            draw.rect = {x, y, w, h};
        }

        // The item's translated rectangle.
        geometry::Rect rectangle(const paint::Item& item) {
            const auto& m = item.transform;
            require(m[0] == 1 && m[1] == 0 && m[2] == 0 && m[3] == 1, "quad items must be translated rectangles");
            return {m[4], m[5], item.width, item.height};
        }
    } // namespace

    bool QuadMasks::Key::operator<(const Key& other) const {
        const std::less<const Asset*> less;
        if (less(input.get(), other.input.get())) {
            return true;
        }
        if (less(other.input.get(), input.get())) {
            return false;
        }
        return coordinates < other.coordinates;
    }

    FramePacket lower_to_quads(const paint::DisplayList& list, QuadMasks* persistent) {
        FramePacket packet;
        QuadMasks local;
        auto& cache = persistent ? *persistent : local;
        std::map<const Asset*, int> asset_indices;
        auto add_asset = [&](const std::shared_ptr<const Asset>& asset) {
            const auto [i, inserted] = asset_indices.emplace(asset.get(), int(packet.assets.size()));
            if (inserted) {
                packet.assets.push_back(asset);
            }
            return i->second;
        };
        std::size_t mask_bytes{};
        auto append = [&](Draw d, const paint::Item& item) {
            crop(d, item.clip);
            if (d.rect.w <= 0 || d.rect.h <= 0) {
                return;
            }
            const auto masks = std::vector<RoundedClip>(list.clips.begin() + item.clips_begin,
                                                        list.clips.begin() + item.clips_begin + item.clips_count);
            bool full = true;
            for (const auto& c : masks) {
                if (!contains_rounded(c, d.rect.x, d.rect.y) || !contains_rounded(c, d.rect.x + d.rect.w, d.rect.y) ||
                    !contains_rounded(c, d.rect.x, d.rect.y + d.rect.h) ||
                    !contains_rounded(c, d.rect.x + d.rect.w, d.rect.y + d.rect.h)) {
                    full = false;
                    break;
                }
            }
            if (!full) {
                require(d.rect.w <= constants::render::rounded_mask_max_dimension &&
                            d.rect.h <= constants::render::rounded_mask_max_dimension,
                        "rounded quad size bound");
                const auto w = std::uint32_t(std::ceil(d.rect.w));
                const auto h = std::uint32_t(std::ceil(d.rect.h));
                QuadMasks::Key key;
                if (persistent) {
                    if (d.asset >= 0) {
                        key.input = packet.assets[d.asset];
                    }
                    key.coordinates = {d.rect.x, d.rect.y, d.rect.w, d.rect.h};
                    key.coordinates.insert(key.coordinates.end(), d.uv.begin(), d.uv.end());
                    for (const auto& c : masks) {
                        key.coordinates.insert(key.coordinates.end(), c.rect.begin(), c.rect.end());
                        key.coordinates.insert(key.coordinates.end(), c.radius_x.begin(), c.radius_x.end());
                        key.coordinates.insert(key.coordinates.end(), c.radius_y.begin(), c.radius_y.end());
                    }
                    if (const auto i = cache.masks.find(key); i != cache.masks.end()) {
                        ++cache.hits;
                        d.asset = add_asset(i->second);
                        d.uv = {0, 0, 1, 1};
                        packet.draws.push_back(d);
                        return;
                    }
                    ++cache.misses;
                }
                auto coverage = rounded_coverage(masks, {d.rect.x, d.rect.y, d.rect.w, d.rect.h}, w, h);
                const Asset* old = d.asset < 0 ? nullptr : packet.assets[d.asset].get();
                const bool image = old && old->encoding == Encoding::SrgbStraight;
                require(!old || old->encoding != Encoding::SignedDistance, "rounded SDF preparation pending");
                const auto bytes = std::size_t(w) * h * (image ? constants::rgba8_bytes_per_pixel : 1);
                require(bytes <= constants::render::quad_mask_packet_max_bytes - mask_bytes,
                        "rounded preparation byte bound");
                mask_bytes += bytes;
                auto combined = std::make_shared<Asset>(
                    Asset{w, h, image ? Encoding::SrgbStraight : Encoding::Coverage, std::vector<std::uint8_t>(bytes)});
                for (std::uint32_t y = 0; y < h; ++y) {
                    for (std::uint32_t x = 0; x < w; ++x) {
                        auto index = std::size_t(y) * w + x;
                        float alpha = coverage[index] / constants::render::color_channel_max_float;
                        if (!old) {
                            combined->bytes[index] = coverage[index];
                            continue;
                        }
                        const float tx = (d.uv[0] + (x + .5f) / w * d.uv[2]) * old->width - .5f;
                        const float ty = (d.uv[1] + (y + .5f) / h * d.uv[3]) * old->height - .5f;
                        const int ix = int(std::floor(tx));
                        const int iy = int(std::floor(ty));
                        const float fx = tx - ix;
                        const float fy = ty - iy;
                        Color sampled{};
                        for (int yy = 0; yy < 2; ++yy) {
                            for (int xx = 0; xx < 2; ++xx) {
                                const auto px = std::clamp(ix + xx, 0, int(old->width) - 1);
                                const auto py = std::clamp(iy + yy, 0, int(old->height) - 1);
                                const auto j = std::size_t(py) * old->width + px;
                                float weight = (xx ? fx : 1 - fx) * (yy ? fy : 1 - fy);
                                if (!image) {
                                    sampled[3] += old->bytes[j] / constants::render::color_channel_max_float * weight;
                                } else {
                                    float a = old->bytes[j * 4 + 3] / constants::render::color_channel_max_float;
                                    for (int c = 0; c < 3; ++c) {
                                        sampled[c] += srgb_to_linear(old->bytes[j * 4 + c] /
                                                                     constants::render::color_channel_max_float) *
                                                      a * weight;
                                    }
                                    sampled[3] += a * weight;
                                }
                            }
                        }
                        if (!image) {
                            combined->bytes[index] =
                                std::uint8_t(std::lround(alpha * sampled[3] * constants::color_channel_max));
                        } else {
                            for (int c = 0; c < 3; ++c) {
                                float linear = sampled[3] > 0 ? std::clamp(sampled[c] / sampled[3], 0.f, 1.f) : 0;
                                float srgb = linear <= constants::render::srgb_encode_threshold
                                                 ? linear * constants::render::srgb_linear_slope
                                                 : constants::render::srgb_scale *
                                                           std::pow(linear, 1 / constants::render::srgb_gamma) -
                                                       constants::render::srgb_offset;
                                combined->bytes[index * 4 + c] =
                                    std::uint8_t(std::lround(srgb * constants::color_channel_max));
                            }
                            combined->bytes[index * 4 + 3] =
                                std::uint8_t(std::lround(sampled[3] * alpha * constants::color_channel_max));
                        }
                    }
                }
                if (persistent && bytes <= constants::render::quad_mask_cache_max_bytes) {
                    if (cache.masks.size() >= constants::render::quad_mask_cache_max_entries ||
                        bytes > constants::render::quad_mask_cache_max_bytes - cache.bytes) {
                        cache.masks.clear();
                        cache.bytes = 0;
                    }
                    cache.bytes += bytes;
                    cache.masks.emplace(std::move(key), combined);
                }
                d.asset = add_asset(combined);
                d.uv = {0, 0, 1, 1};
            }
            packet.draws.push_back(d);
        };
        for (const auto& item : list.items) {
            switch (item.kind) {
                case Kind::Image: {
                    const auto& image = list.images[item.payload];
                    Draw d{rectangle(item), image.color, add_asset(list.assets[image.asset])};
                    d.uv = image.uv;
                    d.stroke = image.stroke;
                    d.outline = image.outline;
                    append(d, item);
                    break;
                }
                case Kind::Box: {
                    const auto& box = list.boxes[item.payload];
                    const auto zero = [](float v) {
                        return v == 0;
                    };
                    require(box.gradient.kind == paint::GradientKind::None &&
                                std::all_of(box.radii.begin(), box.radii.end(), zero) &&
                                std::all_of(box.widths.begin(), box.widths.end(), zero) && item.opacity == 1,
                            "quad boxes must be plain opaque-composited rectangles");
                    append({rectangle(item), box.background}, item);
                    break;
                }
                default:
                    require(false, "quads draw boxes and images only");
            }
        }
        return packet;
    }
} // namespace weft::render
