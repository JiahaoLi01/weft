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

        // A separable Gaussian of standard deviation sigma (px) over a w x h mask, zero beyond it.
        std::vector<float> blurred(const std::vector<float>& mask, unsigned w, unsigned h, float sigma) {
            if (!(sigma > 0)) {
                return mask;
            }
            const int radius = int(std::ceil(constants::scene::shadow_kernel_sigmas * sigma));
            std::vector<float> kernel(2 * radius + 1);
            float total{};
            for (int k = -radius; k <= radius; ++k) {
                total += kernel[k + radius] = std::exp(-float(k * k) / (2 * sigma * sigma));
            }
            for (auto& v : kernel) {
                v /= total;
            }
            std::vector<float> temporary(mask.size());
            std::vector<float> out(mask.size());
            for (unsigned row = 0; row < h; ++row) {
                for (unsigned col = 0; col < w; ++col) {
                    float v{};
                    for (int k = -radius; k <= radius; ++k) {
                        if (int(col) + k >= 0 && int(col) + k < int(w)) {
                            v += mask[std::size_t(row) * w + col + k] * kernel[k + radius];
                        }
                    }
                    temporary[std::size_t(row) * w + col] = v;
                }
            }
            for (unsigned row = 0; row < h; ++row) {
                for (unsigned col = 0; col < w; ++col) {
                    float v{};
                    for (int k = -radius; k <= radius; ++k) {
                        if (int(row) + k >= 0 && int(row) + k < int(h)) {
                            v += temporary[std::size_t(int(row) + k) * w + col] * kernel[k + radius];
                        }
                    }
                    out[std::size_t(row) * w + col] = v;
                }
            }
            return out;
        }

        std::uint8_t channel(float v) {
            return std::uint8_t(std::clamp(v * constants::color_channel_max + .5f, 0.f,
                                           static_cast<float>(constants::color_channel_max)));
        }

        // Where a shadow's pixels lie when they are made at a scale.
        ShadowGeometry geometry_at(const paint::Shadow& s, float width, float height, float scale) {
            ShadowGeometry g;
            if (s.inset) {
                // The box, and a guard for its antialiased edge.
                const float guard = constants::scene::shadow_guard_px;
                g.width = std::uint32_t(std::ceil(width * scale + 2 * guard));
                g.height = std::uint32_t(std::ceil(height * scale + 2 * guard));
                g.rect = {-guard / scale, -guard / scale, g.width / scale, g.height / scale};
                return g;
            }
            const auto [sigma, guard] = padding(s, scale);
            g.width = std::uint32_t(std::ceil((width + 2 * s.spread) * scale + 2 * guard));
            g.height = std::uint32_t(std::ceil((height + 2 * s.spread) * scale + 2 * guard));
            g.rect = {s.x - s.spread - guard / scale, s.y - s.spread - guard / scale, g.width / scale,
                      g.height / scale};
            return g;
        }

        // An inset shadow: inside the box, less what the blurred hole (the box shrunk by spread, offset) leaves clear.
        // Its blur reads the hole on a grid a kernel radius beyond the image on each side.
        Result<ShadowImage> inset_image(const paint::Shadow& s, float w, float h, float scale) {
            ShadowImage out;
            out.geometry = geometry_at(s, w, h, scale);
            const auto iw = out.geometry.width;
            const auto ih = out.geometry.height;
            const float left = out.geometry.rect[0];
            const float top = out.geometry.rect[1];
            const auto sigma = s.blur * scale / 2;
            const unsigned reach = sigma > 0 ? unsigned(std::ceil(constants::scene::shadow_kernel_sigmas * sigma)) : 0;
            const unsigned gw = iw + 2 * reach;
            const unsigned gh = ih + 2 * reach;
            const auto cells = std::size_t(gw) * gh;
            if (!iw || !ih || cells > std::size_t(constants::scene::max_shadow_bitmap_pixels)) {
                return make_diagnostic(Diag::PaintList, "shadow bitmap bound");
            }
            const float hole_w = w - 2 * s.spread;
            const float hole_h = h - 2 * s.spread;
            auto radii = s.radii;
            for (auto& r : radii) {
                r = std::max(0.f, r - s.spread);
            }
            std::vector<float> hole(cells);
            if (hole_w > 0 && hole_h > 0) {
                for (unsigned row = 0; row < gh; ++row) {
                    for (unsigned col = 0; col < gw; ++col) {
                        const float px = left + (float(col) - float(reach) + .5f) / scale - s.x - s.spread;
                        const float py = top + (float(row) - float(reach) + .5f) / scale - s.y - s.spread;
                        hole[std::size_t(row) * gw + col] =
                            std::clamp(.5f - distance(px, py, hole_w, hole_h, radii) * scale, 0.f, 1.f);
                    }
                }
            }
            const auto clear = blurred(hole, gw, gh, sigma);
            out.rgba.resize(std::size_t(iw) * ih * 4);
            for (unsigned row = 0; row < ih; ++row) {
                for (unsigned col = 0; col < iw; ++col) {
                    const auto inside = std::clamp(
                        .5f - distance(left + (col + .5f) / scale, top + (row + .5f) / scale, w, h, s.radii) * scale,
                        0.f, 1.f);
                    const auto shade = 1 - clear[std::size_t(row + reach) * gw + col + reach];
                    const auto alpha = std::clamp(shade, 0.f, 1.f) * inside * s.color[3];
                    auto* p = out.rgba.data() + (std::size_t(row) * iw + col) * 4;
                    p[0] = channel(s.color[0] * alpha);
                    p[1] = channel(s.color[1] * alpha);
                    p[2] = channel(s.color[2] * alpha);
                    p[3] = channel(alpha);
                }
            }
            return out;
        }

        // How many pixels making a shadow at a scale takes: its image, and for an inset shadow the grid its blur
        // reads, a kernel radius wider on each side.
        double cells(const paint::Shadow& s, float width, float height, float scale) {
            if (s.inset) {
                const double guard = constants::scene::shadow_guard_px;
                const auto sigma = s.blur * scale / 2;
                const double reach = sigma > 0 ? std::ceil(constants::scene::shadow_kernel_sigmas * sigma) : 0;
                return (std::ceil(width * scale + 2 * guard) + 2 * reach) *
                       (std::ceil(height * scale + 2 * guard) + 2 * reach);
            }
            const double guard = padding(s, scale).guard;
            return std::ceil((width + 2 * s.spread) * scale + 2 * guard) *
                   std::ceil((height + 2 * s.spread) * scale + 2 * guard);
        }

        // The scale a shadow's pixels are made at: the device scale, or a lower one where that would take more pixels
        // than the bitmap bound (a large panel at a high device scale). A shadow is a blur, and the pass stretches
        // the smaller image over the same rectangle.
        float bitmap_scale(const paint::Shadow& s, float width, float height, float scale) {
            constexpr double bound = constants::scene::max_shadow_bitmap_pixels;
            for (int i = 0; i < constants::scene::shadow_scale_tries; ++i) {
                const auto n = cells(s, width, height, scale);
                if (!(n > bound)) {
                    break;
                }
                scale = float(scale * std::sqrt(bound / n) * constants::scene::shadow_scale_margin);
            }
            return scale;
        }
    } // namespace

    ShadowGeometry shadow_geometry(const paint::Shadow& s, float width, float height, float scale) {
        return geometry_at(s, width, height, bitmap_scale(s, width, height, scale));
    }

    Result<ShadowImage> shadow_image(const paint::Shadow& s, float w, float h, float scale) {
        scale = bitmap_scale(s, w, h, scale);
        if (s.inset) {
            return inset_image(s, w, h, scale);
        }
        const auto [sigma, guard] = padding(s, scale);
        ShadowImage out;
        out.geometry = geometry_at(s, w, h, scale);
        const auto iw = out.geometry.width;
        const auto ih = out.geometry.height;
        const float left = out.geometry.rect[0];
        const float top = out.geometry.rect[1];
        const auto count = std::size_t(iw) * ih;
        if (!iw || !ih || count > std::size_t(constants::scene::max_shadow_bitmap_pixels)) {
            return make_diagnostic(Diag::PaintList, "shadow bitmap bound");
        }
        std::vector<float> mask(count);
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
        const auto filtered = blurred(mask, iw, ih, sigma);
        // An outer shadow shows only outside the box.
        out.rgba.resize(count * 4);
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

    std::vector<float> gaussian_blur(const std::vector<float>& mask, unsigned w, unsigned h, float sigma) {
        return blurred(mask, w, h, sigma);
    }
} // namespace weft::render
