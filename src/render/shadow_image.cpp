#include "render/shadow_image.hpp"

#include "constants/common.hpp"
#include "constants/render.hpp"

#include <algorithm>
#include <cmath>

namespace weft::render {
    namespace {
        // Signed distance from (x, y) to a width x height box with circular corners, each no larger than half the
        // shorter side; negative inside.
        float distance(float x, float y, float width, float height, const std::array<float, 4>& radii) {
            const auto r = std::min(std::min(width, height) / 2,
                                    radii[y < height / 2 ? (x < width / 2 ? 0 : 1) : (x < width / 2 ? 3 : 2)]);
            const auto qx = std::abs(x - width / 2) - (width / 2 - r);
            const auto qy = std::abs(y - height / 2) - (height / 2 - r);
            return std::hypot(std::max(qx, 0.f), std::max(qy, 0.f)) + std::min(std::max(qx, qy), 0.f) - r;
        }

        // Device pixels the blurred shape is padded by on each side, and the blur's standard deviation in them.
        struct Padding {
            float sigma{};
            float guard{};
        };

        Padding padding(const paint::Shadow& s, float scale) {
            const auto sigma = s.blur * scale / 2;
            return {sigma,
                    std::ceil(constants::scene::shadow_kernel_sigmas * sigma) + constants::scene::shadow_guard_px};
        }
    } // namespace

    ShadowGeometry shadow_geometry(const paint::Shadow& s, float width, float height, float scale) {
        const auto [sigma, guard] = padding(s, scale);
        ShadowGeometry g;
        g.width = std::uint32_t(std::ceil((width + 2 * s.spread) * scale + 2 * guard));
        g.height = std::uint32_t(std::ceil((height + 2 * s.spread) * scale + 2 * guard));
        g.rect = {s.x - s.spread - guard / scale, s.y - s.spread - guard / scale, g.width / scale, g.height / scale};
        return g;
    }

    Result<ShadowImage> shadow_image(const paint::Shadow& s, float w, float h, float scale) {
        const auto [sigma, guard] = padding(s, scale);
        ShadowImage out;
        out.geometry = shadow_geometry(s, w, h, scale);
        const auto iw = out.geometry.width;
        const auto ih = out.geometry.height;
        const float left = out.geometry.rect[0];
        const float top = out.geometry.rect[1];
        const auto count = std::size_t(iw) * ih;
        if (!iw || !ih || count > std::size_t(constants::scene::max_shadow_bitmap_pixels)) {
            return make_diagnostic(Diag::PaintList, "shadow bitmap bound");
        }
        std::vector<float> mask(count);
        std::vector<float> temporary(count);
        std::vector<float> filtered(count);
        // The spread shape: the box grown by spread on each side, its radii by spread.
        auto radii = s.radii;
        for (auto& r : radii) {
            r = std::max(0.f, r + s.spread);
        }
        for (unsigned row = 0; row < ih; ++row) {
            for (unsigned col = 0; col < iw; ++col) {
                const float px = (col + .5f) / scale - guard / scale;
                const float py = (row + .5f) / scale - guard / scale;
                mask[std::size_t(row) * iw + col] =
                    std::clamp(.5f - distance(px, py, w + 2 * s.spread, h + 2 * s.spread, radii) * scale, 0.f, 1.f);
            }
        }
        if (sigma > 0) {
            const int radius = int(std::ceil(constants::scene::shadow_kernel_sigmas * sigma));
            std::vector<float> kernel(2 * radius + 1);
            float total{};
            for (int k = -radius; k <= radius; ++k) {
                total += kernel[k + radius] = std::exp(-float(k * k) / (2 * sigma * sigma));
            }
            for (auto& v : kernel) {
                v /= total;
            }
            for (unsigned row = 0; row < ih; ++row) {
                for (unsigned col = 0; col < iw; ++col) {
                    float v{};
                    for (int k = -radius; k <= radius; ++k) {
                        if (int(col) + k >= 0 && int(col) + k < int(iw)) {
                            v += mask[std::size_t(row) * iw + col + k] * kernel[k + radius];
                        }
                    }
                    temporary[std::size_t(row) * iw + col] = v;
                }
            }
            for (unsigned row = 0; row < ih; ++row) {
                for (unsigned col = 0; col < iw; ++col) {
                    float v{};
                    for (int k = -radius; k <= radius; ++k) {
                        if (int(row) + k >= 0 && int(row) + k < int(ih)) {
                            v += temporary[std::size_t(int(row) + k) * iw + col] * kernel[k + radius];
                        }
                    }
                    filtered[std::size_t(row) * iw + col] = v;
                }
            }
        } else {
            filtered = mask;
        }
        // An outer shadow shows only outside the box.
        out.rgba.resize(count * 4);
        const auto channel = [](float v) {
            return std::uint8_t(std::clamp(v * constants::color_channel_max + .5f, 0.f,
                                           static_cast<float>(constants::color_channel_max)));
        };
        for (unsigned row = 0; row < ih; ++row) {
            for (unsigned col = 0; col < iw; ++col) {
                const auto index = std::size_t(row) * iw + col;
                const auto outside = std::clamp(
                    .5f + distance(left + (col + .5f) / scale, top + (row + .5f) / scale, w, h, s.radii) * scale, 0.f,
                    1.f);
                const auto alpha = filtered[index] * outside * s.color[3];
                auto* p = out.rgba.data() + index * 4;
                p[0] = channel(s.color[0] * alpha);
                p[1] = channel(s.color[1] * alpha);
                p[2] = channel(s.color[2] * alpha);
                p[3] = channel(alpha);
            }
        }
        return out;
    }
} // namespace weft::render
