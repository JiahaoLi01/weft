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

        // A separable Gaussian of standard deviation sigma (px) over a w x h mask, zero beyond it. Each cell sums its
        // taps in order, tap by tap over whole rows (loops the compiler vectorizes). A zero adds nothing to the sums,
        // so the loops' bounds leave out the taps past the mask's edges and the cells beyond the bounds of what it
        // covers, and the result is the sum of every tap's.
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
            const int width = int(w);
            const int height = int(h);
            // The rows and columns of the cells that are not zero.
            int top = height;
            int bottom = -1;
            int left = width;
            int right = -1;
            for (int row = 0; row < height; ++row) {
                const float* in = mask.data() + std::size_t(row) * w;
                for (int col = 0; col < width; ++col) {
                    if (in[col] != 0) {
                        top = std::min(top, row);
                        bottom = row;
                        left = std::min(left, col);
                        right = std::max(right, col);
                    }
                }
            }
            std::vector<float> out(mask.size());
            if (bottom < 0) {
                return out;
            }
            // The columns the blur reaches.
            const int from = std::max(0, left - radius);
            const int to = std::min(width, right + radius + 1);
            std::vector<float> temporary(mask.size());
            for (int row = top; row <= bottom; ++row) {
                const float* in = mask.data() + std::size_t(row) * w;
                float* line = temporary.data() + std::size_t(row) * w;
                for (int k = -radius; k <= radius; ++k) {
                    const float weight = kernel[k + radius];
                    const int end = std::min(to, right - k + 1);
                    for (int col = std::max(from, left - k); col < end; ++col) {
                        line[col] += in[col + k] * weight;
                    }
                }
            }
            const int rows_end = std::min(height, bottom + radius + 1);
            for (int row = std::max(0, top - radius); row < rows_end; ++row) {
                float* line = out.data() + std::size_t(row) * w;
                const int last = std::min(radius, bottom - row);
                for (int k = std::max(-radius, top - row); k <= last; ++k) {
                    const float weight = kernel[k + radius];
                    const float* in = temporary.data() + std::size_t(row + k) * w;
                    for (int col = from; col < to; ++col) {
                        line[col] += in[col] * weight;
                    }
                }
            }
            return out;
        }

        std::uint8_t channel(float v) {
            return std::uint8_t(std::clamp(v * constants::color_channel_max + .5f, 0.f,
                                           static_cast<float>(constants::color_channel_max)));
        }

        // How much of a rounded box (width x height, radii) covers each cell of a w x h grid: the cell at (col, row)
        // is centered at (x0 + col / k, y0 + row / k) in the box's px, and covers clamp(.5 - distance * k, 0, 1). Cells
        // more than half a cell inside the box's straight sides cover it whole and cells as far outside it not at all,
        // so the distance is taken only along its edges.
        std::vector<float> coverage(unsigned w, unsigned h, float x0, float y0, float k, float width, float height,
                                    const std::array<float, 4>& radii) {
            std::vector<float> out(std::size_t(w) * h);
            if (!(width > 0) || !(height > 0)) {
                return out;
            }
            float corner{};
            for (const auto r : radii) {
                corner = std::max(corner, std::min(std::min(width, height) / 2, r));
            }
            // Half a cell and a little: past it the clamp is 0 or 1.
            const float band = .5f / k + 1e-3f;
            // The columns whose centers lie in [a, b].
            const auto columns = [&](float a, float b) {
                const auto first = std::max(0.f, std::ceil((a - x0) * k));
                const auto last = std::min(float(w) - 1, std::floor((b - x0) * k));
                return std::pair{int(first), int(last)};
            };
            for (unsigned row = 0; row < h; ++row) {
                const float y = y0 + float(row) / k;
                if (y < -band || y > height + band) {
                    continue;
                }
                auto* line = out.data() + std::size_t(row) * w;
                // The span every cell of which the box covers whole: its full width where the row passes the
                // corners' reach, else between the corners.
                float whole_from = 1;
                float whole_to = 0;
                if (y >= corner + band && y <= height - corner - band) {
                    whole_from = band;
                    whole_to = width - band;
                } else if (y >= band && y <= height - band) {
                    whole_from = corner + band;
                    whole_to = width - corner - band;
                }
                const auto [near_first, near_last] = columns(-band, width + band);
                const auto [whole_first, whole_last] =
                    whole_from <= whole_to ? columns(whole_from, whole_to) : std::pair{1, 0};
                for (int col = near_first; col <= near_last; ++col) {
                    if (col >= whole_first && col <= whole_last) {
                        line[col] = 1;
                        continue;
                    }
                    const float x = x0 + float(col) / k;
                    line[col] = std::clamp(.5f - distance(x, y, width, height, radii) * k, 0.f, 1.f);
                }
            }
            return out;
        }

        // The mask between the cells (x0, y0) and (x0 + 1, y0 + 1), tx and ty of the way, zero beyond it.
        float bilinear(const std::vector<float>& mask, unsigned w, unsigned h, int x0, float tx, int y0, float ty) {
            const auto at = [&](int cx, int cy) {
                return cx < 0 || cy < 0 || cx >= int(w) || cy >= int(h) ? 0.f : mask[std::size_t(cy) * w + cx];
            };
            return (at(x0, y0) * (1 - tx) + at(x0 + 1, y0) * tx) * (1 - ty) +
                   (at(x0, y0 + 1) * (1 - tx) + at(x0 + 1, y0 + 1) * tx) * ty;
        }

        // A mask's value at a point of its grid's px (its first cell's center at 0), bilinear, zero beyond it.
        float sample(const std::vector<float>& mask, unsigned w, unsigned h, float x, float y) {
            const float fx = std::floor(x);
            const float fy = std::floor(y);
            return bilinear(mask, w, h, int(fx), x - fx, int(fy), y - fy);
        }

        // A box blurred by sigma device px over a grid of the image's cells, as `blurred` takes it on that grid; past
        // shadow_coarse_sigma_px, on a grid that much coarser, its value at each cell bilinear between the coarse
        // cells (a blur that wide is smooth). The grid: w x h cells, the first centered at (x0, y0) in the box's px,
        // scale cells per px; reach cells around it read as the box's too.
        struct Blur {
            std::vector<float> values;
            unsigned width{};
            unsigned height{};
            // The coarse grid's cells per image cell, and the image cell (0, 0) on it.
            float step = 1;
            float origin_x{};
            float origin_y{};

            float at(unsigned col, unsigned row) const {
                if (step == 1) {
                    return values[std::size_t(row) * width + col];
                }
                return sample(values, width, height, origin_x + float(col) * step, origin_y + float(row) * step);
            }
        };

        Blur blur_box(unsigned w, unsigned h, float x0, float y0, float scale, float sigma, float width, float height,
                      const std::array<float, 4>& radii) {
            Blur out;
            const float coarse = constants::scene::shadow_coarse_sigma_px;
            if (!(sigma > coarse)) {
                out.values = blurred(coverage(w, h, x0, y0, scale, width, height, radii), w, h, sigma);
                out.width = w;
                out.height = h;
                return out;
            }
            // Cells of the coarse grid per image cell; its cells cover the image's and a cell beyond each side.
            out.step = coarse / sigma;
            const float k = scale * out.step;
            out.width = unsigned(std::ceil(float(w) * out.step)) + 3;
            out.height = unsigned(std::ceil(float(h) * out.step)) + 3;
            const float cx0 = x0 - 1 / k;
            const float cy0 = y0 - 1 / k;
            out.values = blurred(coverage(out.width, out.height, cx0, cy0, k, width, height, radii), out.width,
                                 out.height, coarse);
            out.origin_x = 1;
            out.origin_y = 1;
            return out;
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
            // The hole on its grid (a kernel radius beyond the image), blurred; the box's own coverage of the image.
            const auto clear = blur_box(gw, gh, left + (.5f - float(reach)) / scale - s.x - s.spread,
                                        top + (.5f - float(reach)) / scale - s.y - s.spread, scale, sigma, hole_w,
                                        hole_h, radii);
            const auto inside = coverage(iw, ih, left + .5f / scale, top + .5f / scale, scale, w, h, s.radii);
            out.rgba.resize(std::size_t(iw) * ih * 4);
            for (unsigned row = 0; row < ih; ++row) {
                for (unsigned col = 0; col < iw; ++col) {
                    const auto index = std::size_t(row) * iw + col;
                    if (!(inside[index] > 0)) {
                        continue;
                    }
                    const auto shade = 1 - clear.at(col + reach, row + reach);
                    const auto alpha = std::clamp(shade, 0.f, 1.f) * inside[index] * s.color[3];
                    auto* p = out.rgba.data() + index * 4;
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
        // The spread shape: the box grown by spread on each side, its radii by spread; its top-left lies at the
        // image's guard.
        auto radii = s.radii;
        for (auto& r : radii) {
            r = std::max(0.f, r + s.spread);
        }
        const float first = (.5f - guard) / scale;
        const auto filtered = blur_box(iw, ih, first, first, scale, sigma, w + 2 * s.spread, h + 2 * s.spread, radii);
        // An outer shadow shows only outside the box: what the box covers of each cell is cut away.
        const auto inside = coverage(iw, ih, left + .5f / scale, top + .5f / scale, scale, w, h, s.radii);
        out.rgba.resize(count * 4);
        for (unsigned row = 0; row < ih; ++row) {
            for (unsigned col = 0; col < iw; ++col) {
                const auto index = std::size_t(row) * iw + col;
                const auto outside = 1 - inside[index];
                if (!(outside > 0)) {
                    continue;
                }
                const auto alpha = filtered.at(col, row) * outside * s.color[3];
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
        // Twice shadow_coarse_sigma_px wide and more: the mask's averages over blocks of n x n cells (n as many times
        // the coarse width as the blur is, a block's own spread adding a part in a few hundred), blurred there and read
        // back bilinear at each cell's center, a block of zeros beyond each side.
        const float coarse = constants::scene::shadow_coarse_sigma_px;
        const unsigned n = sigma >= 2 * coarse ? unsigned(sigma / coarse) : 1;
        if (n < 2 || !w || !h) {
            return blurred(mask, w, h, sigma);
        }
        const unsigned cw = (w + n - 1) / n + 2;
        const unsigned ch = (h + n - 1) / n + 2;
        std::vector<float> blocks(std::size_t(cw) * ch);
        const float share = 1 / float(n * n);
        for (unsigned row = 0; row < h; ++row) {
            auto* line = blocks.data() + std::size_t(row / n + 1) * cw + 1;
            for (unsigned col = 0; col < w; ++col) {
                line[col / n] += mask[std::size_t(row) * w + col] * share;
            }
        }
        const auto soft = blurred(blocks, cw, ch, sigma / float(n));
        // Bilinear as sample() reads it, each column's and row's coordinates found once, and only the cells whose
        // four reads can hold more than zero: the coarse columns and rows that are not all zeros.
        int soft_top = int(ch);
        int soft_bottom = -1;
        int soft_left = int(cw);
        int soft_right = -1;
        for (int row = 0; row < int(ch); ++row) {
            for (int col = 0; col < int(cw); ++col) {
                if (soft[std::size_t(row) * cw + col] != 0) {
                    soft_top = std::min(soft_top, row);
                    soft_bottom = row;
                    soft_left = std::min(soft_left, col);
                    soft_right = std::max(soft_right, col);
                }
            }
        }
        std::vector<float> out(mask.size());
        if (soft_bottom < 0) {
            return out;
        }

        struct Tap {
            int cell{};
            float t{};
        };

        const auto taps = [&](unsigned count) {
            std::vector<Tap> out_taps(count);
            for (unsigned i = 0; i < count; ++i) {
                const float p = (float(i) + .5f) / float(n) + .5f;
                const float f = std::floor(p);
                out_taps[i] = {int(f), p - f};
            }
            return out_taps;
        };
        const auto columns = taps(w);
        const auto rows = taps(h);
        for (unsigned row = 0; row < h; ++row) {
            const auto [y0, ty] = rows[row];
            if (y0 + 1 < soft_top || y0 > soft_bottom) {
                continue;
            }
            float* line = out.data() + std::size_t(row) * w;
            for (unsigned col = 0; col < w; ++col) {
                const auto [x0, tx] = columns[col];
                if (x0 + 1 < soft_left || x0 > soft_right) {
                    continue;
                }
                line[col] = bilinear(soft, cw, ch, x0, tx, y0, ty);
            }
        }
        return out;
    }
} // namespace weft::render
