#include "render/rounded_clip.hpp"
#include "constants/render.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace weft {
    RoundedClip normalize_rounded(RoundedClip c) {
        for (float v : c.rect) {
            if (!std::isfinite(v) || std::abs(v) > constants::render::rounded_clip_max_magnitude) {
                throw std::invalid_argument("WEFT_ROUNDED_LIMIT: rectangle");
            }
        }
        if (c.rect[2] < 0 || c.rect[3] < 0) {
            throw std::invalid_argument("WEFT_ROUNDED_LIMIT: negative size");
        }
        for (float v : c.radius_x) {
            if (!std::isfinite(v) || v < 0 || v > constants::render::rounded_clip_max_magnitude) {
                throw std::invalid_argument("WEFT_ROUNDED_LIMIT: radius");
            }
        }
        for (float v : c.radius_y) {
            if (!std::isfinite(v) || v < 0 || v > constants::render::rounded_clip_max_magnitude) {
                throw std::invalid_argument("WEFT_ROUNDED_LIMIT: radius");
            }
        }
        float f = 1;
        auto scale = [&](float extent, float sum) {
            if (sum > 0) {
                f = std::min(f, extent / sum);
            }
        };
        scale(c.rect[2], c.radius_x[0] + c.radius_x[1]);
        scale(c.rect[2], c.radius_x[3] + c.radius_x[2]);
        scale(c.rect[3], c.radius_y[0] + c.radius_y[3]);
        scale(c.rect[3], c.radius_y[1] + c.radius_y[2]);
        for (auto& v : c.radius_x) {
            v *= f;
        }
        for (auto& v : c.radius_y) {
            v *= f;
        }
        return c;
    }

    bool contains_rounded(const RoundedClip& c, float x, float y) {
        const float l = c.rect[0];
        const float t = c.rect[1];
        const float r = l + c.rect[2];
        const float b = t + c.rect[3];
        if (x < l || x > r || y < t || y > b) {
            return false;
        }
        for (unsigned i = 0; i < 4; ++i) {
            const bool right = i == 1 || i == 2;
            const bool bottom = i >= 2;
            const float rx = c.radius_x[i];
            const float ry = c.radius_y[i];
            if (rx == 0 || ry == 0) {
                continue;
            }
            const float cx = right ? r - rx : l + rx;
            const float cy = bottom ? b - ry : t + ry;
            if ((right ? x > cx : x < cx) && (bottom ? y > cy : y < cy)) {
                const auto dx = (x - cx) / rx;
                const auto dy = (y - cy) / ry;
                return dx * dx + dy * dy <= 1;
            }
        }
        return true;
    }

    std::vector<std::uint8_t> rounded_coverage(const std::vector<RoundedClip>& clips, std::array<float, 4> r,
                                               std::uint32_t w, std::uint32_t h) {
        if (clips.size() > constants::render::rounded_mask_max_clips || !w || !h ||
            w > constants::render::rounded_mask_max_dimension || h > constants::render::rounded_mask_max_dimension ||
            std::size_t(w) * h > constants::render::rounded_mask_max_pixels || !std::isfinite(r[0]) ||
            !std::isfinite(r[1]) || !std::isfinite(r[2]) || !std::isfinite(r[3]) || r[2] <= 0 || r[3] <= 0) {
            throw std::invalid_argument("WEFT_ROUNDED_LIMIT: mask work bound");
        }
        std::vector<RoundedClip> normalized;
        for (const auto& c : clips) {
            normalized.push_back(normalize_rounded(c));
        }
        std::vector<std::uint8_t> mask(std::size_t(w) * h);
        constexpr unsigned samples = constants::render::rounded_mask_samples_per_axis;
        for (unsigned y = 0; y < h; ++y) {
            for (unsigned x = 0; x < w; ++x) {
                unsigned inside{};
                bool full = true;
                const float left = r[0] + x * r[2] / w;
                const float top = r[1] + y * r[3] / h;
                const float right = r[0] + (x + 1) * r[2] / w;
                const float bottom = r[1] + (y + 1) * r[3] / h;
                for (const auto& c : normalized) {
                    if (!contains_rounded(c, left, top) || !contains_rounded(c, right, top) ||
                        !contains_rounded(c, left, bottom) || !contains_rounded(c, right, bottom)) {
                        full = false;
                        break;
                    }
                }
                if (full) {
                    mask[std::size_t(y) * w + x] = constants::color_channel_max;
                    continue;
                }
                for (unsigned sy = 0; sy < samples; ++sy) {
                    for (unsigned sx = 0; sx < samples; ++sx) {
                        const float px = r[0] + (x + (sx + .5f) / samples) * r[2] / w;
                        const float py = r[1] + (y + (sy + .5f) / samples) * r[3] / h;
                        bool okay = true;
                        for (const auto& c : normalized) {
                            if (!contains_rounded(c, px, py)) {
                                okay = false;
                                break;
                            }
                        }
                        inside += okay;
                    }
                }
                mask[std::size_t(y) * w + x] =
                    std::uint8_t((inside * constants::color_channel_max + samples * samples / 2) / (samples * samples));
            }
        }
        return mask;
    }
} // namespace weft
