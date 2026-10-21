#include "render/shadow_image.hpp"

#include "constants/common.hpp"
#include "constants/render.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <tuple>
#include <utility>

#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#define WEFT_SHADOW_SSE2 1
#else
#define WEFT_SHADOW_SSE2 0
#endif

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

        // line[col] += in[col + shift] * weight for the columns [from, end); with SSE2 four at a time, each sum as it
        // is.
        void add_tap(float* line, const float* in, int shift, float weight, int from, int end) {
            int col = from;
#if WEFT_SHADOW_SSE2
            const __m128 w = _mm_set1_ps(weight);
            for (; col + 4 <= end; col += 4) {
                const __m128 tap = _mm_mul_ps(_mm_loadu_ps(in + (col + shift)), w);
                _mm_storeu_ps(line + col, _mm_add_ps(_mm_loadu_ps(line + col), tap));
            }
#endif
            for (; col < end; ++col) {
                line[col] += in[col + shift] * weight;
            }
        }

        // A separable Gaussian of standard deviation sigma (px) over a w x h mask, zero beyond it. Each cell sums its
        // taps in order, tap by tap over whole rows. A zero adds nothing to the sums, so the loops' bounds leave out
        // the taps past the mask's edges and the cells beyond the bounds of what it covers, and the result is the sum
        // of every tap's.
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
                    add_tap(line, in, k, kernel[k + radius], std::max(from, left - k), std::min(to, right - k + 1));
                }
            }
            const int rows_end = std::min(height, bottom + radius + 1);
            for (int row = std::max(0, top - radius); row < rows_end; ++row) {
                float* line = out.data() + std::size_t(row) * w;
                const int last = std::min(radius, bottom - row);
                for (int k = std::max(-radius, top - row); k <= last; ++k) {
                    add_tap(line, temporary.data() + std::size_t(row + k) * w, 0, kernel[k + radius], from, to);
                }
            }
            return out;
        }

        std::uint8_t channel(float v) {
            return std::uint8_t(std::clamp(v * constants::color_channel_max + .5f, 0.f,
                                           static_cast<float>(constants::color_channel_max)));
        }

        // The premultiplied BGRA pixels of a color at alphas, at the columns [from, to] of a row. With SSE2, four at a
        // time: for a number (not NaN) max and min are std::clamp's, and the conversion truncates as a cast does.
        void put(std::uint8_t* row, const paint::Color& color, const float* alpha, int from, int to) {
            int col = from;
#if WEFT_SHADOW_SSE2
            const __m128 most = _mm_set1_ps(float(constants::color_channel_max));
            const __m128 half = _mm_set1_ps(.5f);
            const __m128 zero = _mm_setzero_ps();
            const auto channels = [&](__m128 v) {
                return _mm_cvttps_epi32(_mm_min_ps(_mm_max_ps(_mm_add_ps(_mm_mul_ps(v, most), half), zero), most));
            };
            const __m128 red = _mm_set1_ps(color[0]);
            const __m128 green = _mm_set1_ps(color[1]);
            const __m128 blue = _mm_set1_ps(color[2]);
            for (; col + 3 <= to; col += 4) {
                const __m128 a = _mm_loadu_ps(alpha + col);
                const __m128i pixels = _mm_or_si128(
                    _mm_or_si128(channels(_mm_mul_ps(blue, a)), _mm_slli_epi32(channels(_mm_mul_ps(green, a)), 8)),
                    _mm_or_si128(_mm_slli_epi32(channels(_mm_mul_ps(red, a)), 16), _mm_slli_epi32(channels(a), 24)));
                _mm_storeu_si128(reinterpret_cast<__m128i*>(row + std::size_t(col) * 4), pixels);
            }
#endif
            for (; col <= to; ++col) {
                const float a = alpha[col];
                auto* p = row + std::size_t(col) * 4;
                p[0] = channel(color[2] * a);
                p[1] = channel(color[1] * a);
                p[2] = channel(color[0] * a);
                p[3] = channel(a);
            }
        }

        // How much of a rounded box (width x height, radii) covers the cells of a grid w cells wide, a row at a time:
        // the cell at (col, row) is centered at (x0 + col / k, y0 + row / k) in the box's px, and covers
        // clamp(.5 - distance * k, 0, 1). Cells more than half a cell inside the box's straight sides cover it whole
        // and cells as far outside it not at all, so the distance is taken only along its edges.
        class Coverage {
        public:
            // The cells of a row the box reaches, [near_first, near_last], and those it covers whole, [whole_first,
            // whole_last]; either is empty where its first is past its last.
            struct Row {
                float y{};
                int near_first = 1;
                int near_last = 0;
                int whole_first = 1;
                int whole_last = 0;
            };

            Coverage(unsigned w, float x0, float y0, float k, float width, float height,
                     const std::array<float, 4>& radii)
                : w_(w),
                  x0_(x0),
                  y0_(y0),
                  k_(k),
                  width_(width),
                  height_(height),
                  radii_(radii),
                  band_(.5f / k + 1e-3f),
                  half_(std::min(width, height) / 2) {
                for (const auto r : radii) {
                    corner_ = std::max(corner_, std::min(half_, r));
                }
            }

            Row row(unsigned row) const {
                Row r;
                r.y = y0_ + float(row) / k_;
                if (!(width_ > 0) || !(height_ > 0) || r.y < -band_ || r.y > height_ + band_) {
                    return r;
                }
                // The span every cell of which the box covers whole: its full width where the row passes the
                // corners' reach, else between the corners.
                float whole_from = 1;
                float whole_to = 0;
                if (r.y >= corner_ + band_ && r.y <= height_ - corner_ - band_) {
                    whole_from = band_;
                    whole_to = width_ - band_;
                } else if (r.y >= band_ && r.y <= height_ - band_) {
                    whole_from = corner_ + band_;
                    whole_to = width_ - corner_ - band_;
                }
                float near_from = -band_;
                float near_to = width_ + band_;
                // In a corner's rows, also the cells more than half a cell and a margin for rounding inside or outside
                // its arc, whose coverage is 1 or 0 all the same: those within the chords of the arc shrunk by that
                // much, and those past the chords of the arc grown by it.
                const bool top = r.y < height_ / 2;
                const float left = std::min(half_, radii_[top ? 0 : 3]);
                const float right = std::min(half_, radii_[top ? 1 : 2]);
                // How far into a corner's rows the row lies, and half an arc's chord there.
                const auto into = [&](float radius) {
                    return top ? radius - r.y : r.y - (height_ - radius);
                };
                const auto half_chord = [](float radius, float at) {
                    return std::sqrt(std::max(0.f, radius * radius - at * at));
                };
                const float margin = band_ + arc_margin;
                const float into_left = into(left);
                const float into_right = into(right);
                if (r.y >= margin && r.y <= height_ - margin) {
                    const float from = into_left > 0 ? left - half_chord(left - margin, into_left) : margin;
                    const float to =
                        into_right > 0 ? width_ - right + half_chord(right - margin, into_right) : width_ - margin;
                    const bool straight = whole_from <= whole_to;
                    whole_from = straight ? std::min(whole_from, from) : from;
                    whole_to = straight ? std::max(whole_to, to) : to;
                }
                if (into_left > 0) {
                    near_from = std::max(near_from, left - half_chord(left + margin, into_left));
                }
                if (into_right > 0) {
                    near_to = std::min(near_to, width_ - right + half_chord(right + margin, into_right));
                }
                std::tie(r.near_first, r.near_last) = columns(near_from, near_to);
                if (whole_from <= whole_to) {
                    std::tie(r.whole_first, r.whole_last) = columns(whole_from, whole_to);
                }
                return r;
            }

            // A cell of a row the box reaches.
            float at(const Row& r, int col) const {
                if (col >= r.whole_first && col <= r.whole_last) {
                    return 1;
                }
                const float x = x0_ + float(col) / k_;
                return std::clamp(.5f - distance(x, r.y, width_, height_, radii_) * k_, 0.f, 1.f);
            }

        private:
            // The columns whose centers lie in [a, b].
            std::pair<int, int> columns(float a, float b) const {
                const auto first = std::max(0.f, std::ceil((a - x0_) * k_));
                const auto last = std::min(float(w_) - 1, std::floor((b - x0_) * k_));
                return {int(first), int(last)};
            }

            // Box px: far more than a distance's rounding.
            static constexpr float arc_margin = .05f;

            unsigned w_{};
            float x0_{};
            float y0_{};
            float k_{};
            float width_{};
            float height_{};
            std::array<float, 4> radii_{};
            // Half a cell and a little: past it the clamp is 0 or 1.
            float band_{};
            // The largest radius a corner takes, and the largest corner.
            float half_{};
            float corner_{};
        };

        // A rounded box's coverage of each cell of a w x h grid, as Coverage takes it.
        std::vector<float> coverage(unsigned w, unsigned h, float x0, float y0, float k, float width, float height,
                                    const std::array<float, 4>& radii) {
            std::vector<float> out(std::size_t(w) * h);
            const Coverage box(w, x0, y0, k, width, height, radii);
            for (unsigned row = 0; row < h; ++row) {
                const auto r = box.row(row);
                auto* line = out.data() + std::size_t(row) * w;
                for (int col = r.near_first; col <= r.near_last; ++col) {
                    line[col] = box.at(r, col);
                }
            }
            return out;
        }

        // A row of a mask w cells wide between its cells x0 and x0 + 1, tx of the way, zero beyond it: bilinear
        // between two rows is a * (1 - ty) + b * ty of the two rows' values.
        float between(const float* row, unsigned w, int x0, float tx) {
            const auto at = [&](int cx) {
                return cx < 0 || cx >= int(w) ? 0.f : row[cx];
            };
            return at(x0) * (1 - tx) + at(x0 + 1) * tx;
        }

        // A box blurred by sigma device px over a w x h grid of the image's cells, the first centered at (x0, y0) in
        // the box's px, scale cells per px, read a row at a time: without a blur the box's coverage of each cell, to
        // shadow_coarse_sigma_px that coverage as `blurred` takes it on the grid, and past it `blurred` on a grid
        // that much coarser, its value at each cell bilinear between the coarse cells (a blur that wide is smooth):
        // each coarse row read across the grid's columns once, and each row between the two it lies between.
        class Blur {
        public:
            Blur(unsigned w, unsigned h, float x0, float y0, float scale, float sigma, float width, float height,
                 const std::array<float, 4>& radii)
                : shape_(w, x0, y0, scale, width, height, radii), w_(w) {
                const float coarse = constants::scene::shadow_coarse_sigma_px;
                if (!(sigma > 0)) {
                    return;
                }
                if (!(sigma > coarse)) {
                    kind_ = Kind::Fine;
                    values_ = blurred(coverage(w, h, x0, y0, scale, width, height, radii), w, h, sigma);
                    return;
                }
                // Cells of the coarse grid per image cell; its cells cover the image's and a cell beyond each side,
                // the image's cell (0, 0) at its cell (1, 1).
                kind_ = Kind::Coarse;
                step_ = coarse / sigma;
                const float k = scale * step_;
                const auto cw = unsigned(std::ceil(float(w) * step_)) + 3;
                rows_ = unsigned(std::ceil(float(h) * step_)) + 3;
                const auto soft =
                    blurred(coverage(cw, rows_, x0 - 1 / k, y0 - 1 / k, k, width, height, radii), cw, rows_, coarse);
                std::vector<int> cells(w);
                std::vector<float> ts(w);
                for (unsigned col = 0; col < w; ++col) {
                    const float x = origin + float(col) * step_;
                    const float fx = std::floor(x);
                    cells[col] = int(fx);
                    ts[col] = x - fx;
                }
                values_.resize(std::size_t(rows_) * w);
                for (unsigned row = 0; row < rows_; ++row) {
                    const float* in = soft.data() + std::size_t(row) * cw;
                    float* across = values_.data() + std::size_t(row) * w;
                    for (unsigned col = 0; col < w; ++col) {
                        across[col] = between(in, cw, cells[col], ts[col]);
                    }
                }
                zeros_.resize(w);
            }

            // Its values at the columns [first, last] of a row, into line at the same columns.
            void read(unsigned row, int first, int last, float* line) const {
                if (kind_ == Kind::Shape) {
                    const auto r = shape_.row(row);
                    for (int col = first; col <= last; ++col) {
                        line[col] = col >= r.near_first && col <= r.near_last ? shape_.at(r, col) : 0.f;
                    }
                } else if (kind_ == Kind::Fine) {
                    const float* in = values_.data() + std::size_t(row) * w_;
                    std::copy(in + first, in + last + 1, line + first);
                } else {
                    const float y = origin + float(row) * step_;
                    const float fy = std::floor(y);
                    const float ty = y - fy;
                    const float* above = across(int(fy));
                    const float* below = across(int(fy) + 1);
                    for (int col = first; col <= last; ++col) {
                        line[col] = above[col] * (1 - ty) + below[col] * ty;
                    }
                }
            }

        private:
            enum class Kind : std::uint8_t {
                Shape,
                Fine,
                Coarse
            };

            // The image's cell (0, 0) on the coarse grid.
            static constexpr float origin = 1;

            // A coarse row across the grid's columns, zeros past the coarse grid.
            const float* across(int row) const {
                return row < 0 || row >= int(rows_) ? zeros_.data() : values_.data() + std::size_t(row) * w_;
            }

            Kind kind_ = Kind::Shape;
            Coverage shape_;
            unsigned w_{};
            // Fine: the blurred grid. Coarse: the coarse rows across the grid's columns, and step_ coarse cells per
            // image cell.
            std::vector<float> values_;
            std::vector<float> zeros_;
            unsigned rows_{};
            float step_ = 1;
        };

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
        // Its blur reads the hole on a grid a kernel radius beyond the image on each side. Only the cells the box
        // reaches are made, and of them those the hole leaves shaded.
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
            const Blur clear(gw, gh, left + (.5f - float(reach)) / scale - s.x - s.spread,
                             top + (.5f - float(reach)) / scale - s.y - s.spread, scale, sigma, hole_w, hole_h, radii);
            const Coverage box(iw, left + .5f / scale, top + .5f / scale, scale, w, h, s.radii);
            out.bgra.resize(std::size_t(iw) * ih * 4);
            std::vector<float> line(gw);
            std::vector<float> alpha(iw);
            for (unsigned row = 0; row < ih; ++row) {
                const auto in = box.row(row);
                if (in.near_first > in.near_last) {
                    continue;
                }
                clear.read(row + reach, in.near_first + int(reach), in.near_last + int(reach), line.data());
                for (int col = in.near_first; col <= in.near_last; ++col) {
                    const auto shade = 1 - line[col + reach];
                    const float inside = shade > 0 ? box.at(in, col) : 0.f;
                    alpha[col] = inside > 0 ? std::clamp(shade, 0.f, 1.f) * inside * s.color[3] : 0.f;
                }
                put(out.bgra.data() + std::size_t(row) * iw * 4, s.color, alpha.data(), in.near_first, in.near_last);
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
        const Blur filtered(iw, ih, first, first, scale, sigma, w + 2 * s.spread, h + 2 * s.spread, radii);
        // An outer shadow shows only outside the box: what the box covers of each cell is cut away, and the cells it
        // covers whole are not made.
        const Coverage box(iw, left + .5f / scale, top + .5f / scale, scale, w, h, s.radii);
        out.bgra.resize(count * 4);
        std::vector<float> line(iw);
        std::vector<float> alpha(iw);
        for (unsigned row = 0; row < ih; ++row) {
            const auto cut = box.row(row);
            const bool whole = cut.whole_first <= cut.whole_last;
            for (const auto span : {std::pair{0, whole ? cut.whole_first - 1 : int(iw) - 1},
                                    std::pair{whole ? cut.whole_last + 1 : int(iw), int(iw) - 1}}) {
                const int from = span.first;
                const int to = span.second;
                if (from > to) {
                    continue;
                }
                filtered.read(row, from, to, line.data());
                // Whole past what the box reaches, and where it reaches less what it covers.
                for (int col = from; col <= to; ++col) {
                    alpha[col] = line[col] * s.color[3];
                }
                for (int col = std::max(from, cut.near_first); col <= std::min(to, cut.near_last); ++col) {
                    const float outside = 1 - box.at(cut, col);
                    alpha[col] = outside > 0 ? line[col] * outside * s.color[3] : 0.f;
                }
                put(out.bgra.data() + std::size_t(row) * iw * 4, s.color, alpha.data(), from, to);
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
            const float* in = mask.data() + std::size_t(row) * w;
            for (unsigned block = 0, col = 0; col < w; ++block) {
                for (const unsigned end = std::min(w, col + n); col < end; ++col) {
                    line[block] += in[col] * share;
                }
            }
        }
        const auto soft = blurred(blocks, cw, ch, sigma / float(n));
        // Read back bilinear, each column's and row's coordinates found once, and only the cells whose four reads can
        // hold more than zero: the coarse columns and rows that are not all zeros. Each such coarse row is read across
        // the columns once, and each row between the two it lies between.
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
        // The columns' cells grow with them.
        unsigned col_from = 0;
        while (col_from < w && columns[col_from].cell + 1 < soft_left) {
            ++col_from;
        }
        unsigned col_to = w;
        while (col_to > col_from && columns[col_to - 1].cell > soft_right) {
            --col_to;
        }
        std::vector<float> across(std::size_t(soft_bottom - soft_top + 1) * w);
        for (int row = soft_top; row <= soft_bottom; ++row) {
            const float* in = soft.data() + std::size_t(row) * cw;
            float* line = across.data() + std::size_t(row - soft_top) * w;
            for (unsigned col = col_from; col < col_to; ++col) {
                line[col] = between(in, cw, columns[col].cell, columns[col].t);
            }
        }
        const std::vector<float> zeros(w);
        const auto coarse_row = [&](int row) {
            return row < soft_top || row > soft_bottom ? zeros.data() : across.data() + std::size_t(row - soft_top) * w;
        };
        for (unsigned row = 0; row < h; ++row) {
            const auto [y0, ty] = rows[row];
            if (y0 + 1 < soft_top || y0 > soft_bottom) {
                continue;
            }
            const float* above = coarse_row(y0);
            const float* below = coarse_row(y0 + 1);
            float* line = out.data() + std::size_t(row) * w;
            for (unsigned col = col_from; col < col_to; ++col) {
                line[col] = above[col] * (1 - ty) + below[col] * ty;
            }
        }
        return out;
    }

    std::vector<std::uint8_t> coverage_bytes(const std::vector<float>& values) {
        std::vector<std::uint8_t> out(values.size());
        std::size_t i = 0;
#if WEFT_SHADOW_SSE2
        // Four at a time: for a number (not NaN) max and min are std::clamp's, and std::lround rounds what is not below
        // zero as the truncation does and one more where what it leaves (exactly) is at least a half.
        const __m128 zero = _mm_setzero_ps();
        const __m128 one = _mm_set1_ps(1);
        const __m128 most = _mm_set1_ps(float(constants::color_channel_max));
        const __m128 half = _mm_set1_ps(.5f);
        const __m128i up = _mm_set1_epi32(1);
        for (; i + 4 <= values.size(); i += 4) {
            const __m128 v = _mm_mul_ps(_mm_min_ps(_mm_max_ps(_mm_loadu_ps(values.data() + i), zero), one), most);
            const __m128i whole = _mm_cvttps_epi32(v);
            const __m128i rounded = _mm_add_epi32(
                whole, _mm_and_si128(_mm_castps_si128(_mm_cmpge_ps(_mm_sub_ps(v, _mm_cvtepi32_ps(whole)), half)), up));
            const __m128i words = _mm_packs_epi32(rounded, rounded);
            const int bytes = _mm_cvtsi128_si32(_mm_packus_epi16(words, words));
            std::memcpy(out.data() + i, &bytes, sizeof bytes);
        }
#endif
        for (; i < values.size(); ++i) {
            out[i] = std::uint8_t(std::lround(std::clamp(values[i], 0.f, 1.f) * float(constants::color_channel_max)));
        }
        return out;
    }

    void blur_coverage(std::vector<std::uint8_t>& coverage, unsigned w, unsigned h, float sigma) {
        std::vector<float> values(coverage.size());
        for (std::size_t i = 0; i < values.size(); ++i) {
            values[i] = coverage[i] / float(constants::color_channel_max);
        }
        coverage = coverage_bytes(gaussian_blur(values, w, h, sigma));
    }
} // namespace weft::render
