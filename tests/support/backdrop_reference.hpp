#pragma once

#include "paint/display_list.hpp"
#include "render/scene_pass.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

// The scene passes' backdrop filter on the CPU, as the passes compute it (src/render/shaders/backdrop.hlsl and the
// scene pass's Step::Backdrop): encoded premultiplied RGBA in 8-bit images between passes, the color matrix before the
// blur, an area downsample across and then down to the step's size (each texel the mean of the source texels its span
// covers), a horizontal then a vertical Gaussian pass with every address past an image's edge mirrored back into it;
// the composite samples the result bilinearly, clamped, applies the matrix after the blur and draws source-over.
namespace weft::testing {
    using Rgba = std::array<float, 4>;

    struct BackdropImage {
        int width{};
        int height{};
        // Encoded premultiplied RGBA in [0, 1], quantized to 8 bits as a texture holds it.
        std::vector<Rgba> pixels;

        const Rgba& at(int x, int y) const {
            return pixels[std::size_t(y) * std::size_t(width) + std::size_t(x)];
        }
    };

    // Texel index of coordinate i in [0, n): mirrored past the edges as the mirror address modes repeat them, or
    // clamped.
    inline int address(int i, int n, bool mirror) {
        if (!mirror) {
            return std::clamp(i, 0, n - 1);
        }
        const int period = 2 * n;
        const int m = ((i % period) + period) % period;
        return m < n ? m : period - 1 - m;
    }

    inline Rgba quantized(Rgba c) {
        for (auto& v : c) {
            v = std::nearbyint(std::clamp(v, 0.f, 1.f) * 255) / 255;
        }
        return c;
    }

    // A bilinear sample at uv (0-1 over the image).
    inline Rgba bilinear(const BackdropImage& image, float u, float v, bool mirror) {
        const float x = u * float(image.width) - .5f;
        const float y = v * float(image.height) - .5f;
        const int x0 = int(std::floor(x));
        const int y0 = int(std::floor(y));
        const float fx = x - float(x0);
        const float fy = y - float(y0);
        Rgba out{};
        for (int dy = 0; dy < 2; ++dy) {
            for (int dx = 0; dx < 2; ++dx) {
                const float w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
                const auto& t = image.at(address(x0 + dx, image.width, mirror), address(y0 + dy, image.height, mirror));
                for (unsigned c = 0; c < 4; ++c) {
                    out[c] += t[c] * w;
                }
            }
        }
        return out;
    }

    // A color matrix on a premultiplied color: on its straight values, clamped, premultiplied again.
    inline Rgba matrix(const paint::ColorMatrix& m, const Rgba& c) {
        const Rgba s = c[3] > 0 ? Rgba{c[0] / c[3], c[1] / c[3], c[2] / c[3], c[3]} : Rgba{};
        Rgba out{};
        for (unsigned r = 0; r < 4; ++r) {
            float v = m[r * 5 + 4];
            for (unsigned k = 0; k < 4; ++k) {
                v += m[r * 5 + k] * s[k];
            }
            out[r] = std::clamp(v, 0.f, 1.f);
        }
        return {out[0] * out[3], out[1] * out[3], out[2] * out[3], out[3]};
    }

    // The region [x0, x1) x [y0, y1) of a target's encoded premultiplied BGRA rows.
    inline BackdropImage copied(const std::vector<std::uint8_t>& bgra, int target_width, std::array<int, 4> region) {
        BackdropImage out{region[2] - region[0], region[3] - region[1], {}};
        for (int y = region[1]; y < region[3]; ++y) {
            for (int x = region[0]; x < region[2]; ++x) {
                const auto* p = bgra.data() + (std::size_t(y) * std::size_t(target_width) + std::size_t(x)) * 4;
                out.pixels.push_back({p[2] / 255.f, p[1] / 255.f, p[0] / 255.f, p[3] / 255.f});
            }
        }
        return out;
    }

    // An area downsample of image across (or down) to size texels on that axis: each the mean of the source texels its
    // span covers, those at its ends by their share.
    inline BackdropImage area(const BackdropImage& image, int size, bool across) {
        const int from = across ? image.width : image.height;
        const double step = double(from) / double(size);
        BackdropImage out{across ? size : image.width, across ? image.height : size, {}};
        for (int y = 0; y < out.height; ++y) {
            for (int x = 0; x < out.width; ++x) {
                const double start = (across ? x : y) * step;
                const double end = start + step;
                Rgba sum{};
                // The last span ends at the source's end, give or take rounding.
                const int last = std::min(int(std::ceil(end)), from);
                for (int i = int(std::floor(start)); i < last; ++i) {
                    const double w = std::min(end, i + 1.0) - std::max(start, double(i));
                    const auto& t = across ? image.at(i, y) : image.at(x, i);
                    for (unsigned c = 0; c < 4; ++c) {
                        sum[c] += float(t[c] * w);
                    }
                }
                for (auto& v : sum) {
                    v = float(v / step);
                }
                out.pixels.push_back(quantized(sum));
            }
        }
        return out;
    }

    // What a Step::Backdrop draws from: the copy through pre, downsampled, blurred.
    inline BackdropImage filtered(BackdropImage image, const render::ScenePass::Backdrop& b) {
        if (b.pre != paint::identity_color_matrix) {
            for (auto& p : image.pixels) {
                p = quantized(matrix(b.pre, p));
            }
        }
        if (int(b.size[0]) < image.width) {
            image = area(image, int(b.size[0]), true);
        }
        if (int(b.size[1]) < image.height) {
            image = area(image, int(b.size[1]), false);
        }
        if (b.sigma[0] > 0) {
            for (const bool across : {true, false}) {
                const auto k = render::backdrop_blur_constants(b.sigma[across ? 0 : 1], across, unsigned(image.width),
                                                               unsigned(image.height));
                const int taps = int(k.extent[0]);
                BackdropImage out{image.width, image.height, {}};
                for (int y = 0; y < image.height; ++y) {
                    for (int x = 0; x < image.width; ++x) {
                        Rgba sum{};
                        for (int i = -taps; i <= taps; ++i) {
                            const float w = k.weights[std::size_t(std::abs(i)) / 4][std::size_t(std::abs(i)) % 4];
                            const auto& t = across ? image.at(address(x + i, image.width, true), y)
                                                   : image.at(x, address(y + i, image.height, true));
                            for (unsigned c = 0; c < 4; ++c) {
                                sum[c] += t[c] * w;
                            }
                        }
                        out.pixels.push_back(quantized(sum));
                    }
                }
                image = std::move(out);
            }
        }
        return image;
    }

    // The pixel (x, y) of the composite over below: the filtered image sampled at the pixel's centre in the region,
    // through post, source-over; encoded premultiplied RGBA in [0, 1].
    inline Rgba composite(const BackdropImage& filtered, std::array<int, 4> region, const paint::ColorMatrix& post,
                          int x, int y, const Rgba& below, float opacity = 1) {
        auto c = bilinear(filtered, (float(x) + .5f - float(region[0])) / float(region[2] - region[0]),
                          (float(y) + .5f - float(region[1])) / float(region[3] - region[1]), false);
        if (post != paint::identity_color_matrix) {
            c = matrix(post, c);
        }
        Rgba out{};
        for (unsigned k = 0; k < 4; ++k) {
            out[k] = c[k] * opacity + below[k] * (1 - c[3] * opacity);
        }
        return out;
    }
} // namespace weft::testing
