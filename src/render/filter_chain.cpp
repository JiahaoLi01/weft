#include "render/filter_chain.hpp"

#include "constants/common.hpp"
#include "constants/render.hpp"
#include "render/srgb.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace weft::render {
    namespace {
        using Rgba = std::array<float, 4>;

        // The matrix that keeps straight colors as they are.
        constexpr paint::ColorMatrix identity = paint::identity_color_matrix;
    } // namespace

    FilterChain plan_filter_chain(const std::vector<paint::FilterOp>& ops, std::uint32_t width, std::uint32_t height,
                                  float resolution) {
        FilterChain chain;
        chain.images.push_back({width, height});
        std::uint32_t current{};
        // A new image of the given size, and a pass into it.
        const auto image = [&](std::array<std::uint32_t, 2> size) {
            chain.images.push_back(size);
            return std::uint32_t(chain.images.size() - 1);
        };
        const auto pass = [&](FilterStep::Shader shader, std::uint32_t source, std::uint32_t target, bool over,
                              const BackdropConstants& constants) {
            chain.steps.push_back({shader, source, target, over, constants});
        };
        // The image from blurred by a Gaussian of standard deviation sigma (device px of the content): as a backdrop
        // blurs, downsampled first by area to the whole texels that bring it nearest max_backdrop_sigma (at least one).
        const auto blurred = [&](std::uint32_t from, float sigma) {
            const auto size = chain.images[from];
            const float sx = sigma * float(size[0]) / float(width);
            const float sy = sigma * float(size[1]) / float(height);
            std::array<std::uint32_t, 2> to = size;
            std::array<float, 2> deviation{sx, sy};
            constexpr float most = constants::render::scene::max_backdrop_sigma;
            if (sx > most) {
                to[0] = std::uint32_t(std::max(1.f, std::floor(float(size[0]) * most / sx)));
                deviation[0] = sx * float(to[0]) / float(size[0]);
            }
            if (sy > most) {
                to[1] = std::uint32_t(std::max(1.f, std::floor(float(size[1]) * most / sy)));
                deviation[1] = sy * float(to[1]) / float(size[1]);
            }
            auto at = from;
            if (to[0] < size[0]) {
                const auto next = image({to[0], size[1]});
                pass(FilterStep::Shader::Area, at, next, false, backdrop_area_constants(size[0], to[0], true));
                at = next;
            }
            if (to[1] < size[1]) {
                const auto next = image(to);
                pass(FilterStep::Shader::Area, at, next, false, backdrop_area_constants(size[1], to[1], false));
                at = next;
            }
            const auto across = image(to);
            pass(FilterStep::Shader::Blur, at, across, false,
                 backdrop_blur_constants(deviation[0], true, to[0], to[1]));
            const auto down = image(to);
            pass(FilterStep::Shader::Blur, across, down, false,
                 backdrop_blur_constants(deviation[1], false, to[0], to[1]));
            return down;
        };
        for (const auto& op : ops) {
            switch (op.kind) {
                case paint::FilterOp::Kind::Blur:
                    if (op.blur > 0) {
                        current = blurred(current, op.blur * resolution);
                    }
                    break;
                case paint::FilterOp::Kind::Matrix: {
                    const auto next = image(chain.images[current]);
                    pass(FilterStep::Shader::Matrix, current, next, false,
                         backdrop_matrix_constants(op.matrix, op.linear));
                    current = next;
                    break;
                }
                case paint::FilterOp::Kind::DropShadow: {
                    // The alpha blurred, tinted with the shadow's color and offset; the image over it.
                    const auto shadow = op.blur > 0 ? blurred(current, op.blur * resolution) : current;
                    paint::ColorMatrix tint{};
                    tint[4] = op.color[0];
                    tint[9] = op.color[1];
                    tint[14] = op.color[2];
                    tint[18] = op.color[3];
                    const auto next = image(chain.images[current]);
                    pass(FilterStep::Shader::Matrix, shadow, next, false,
                         backdrop_matrix_constants(
                             tint, false, {op.x * resolution / float(width), op.y * resolution / float(height)}));
                    pass(FilterStep::Shader::Matrix, current, next, true, backdrop_matrix_constants(identity));
                    current = next;
                    break;
                }
            }
        }
        chain.result = current;
        return chain;
    }

    namespace {
        // Texel index of coordinate i in [0, n), mirrored past the edges as the mirror address modes repeat them.
        int mirrored(int i, int n) {
            const int period = 2 * n;
            const int m = ((i % period) + period) % period;
            return m < n ? m : period - 1 - m;
        }

        const Rgba& at(const FilterPixels& image, int x, int y) {
            return image.rgba[std::size_t(y) * image.width + std::size_t(x)];
        }

        // A bilinear sample at (u, v), 0 to 1 over the image, addresses mirrored.
        Rgba bilinear(const FilterPixels& image, float u, float v) {
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
                    if (w == 0) {
                        continue;
                    }
                    const auto& t =
                        at(image, mirrored(x0 + dx, int(image.width)), mirrored(y0 + dy, int(image.height)));
                    for (unsigned c = 0; c < 4; ++c) {
                        out[c] += t[c] * w;
                    }
                }
            }
            return out;
        }

        // A value as an 8-bit UNORM target stores it.
        float stored(float v) {
            return std::nearbyint(std::clamp(v, 0.f, 1.f) * float(constants::color_channel_max)) /
                   float(constants::color_channel_max);
        }

        // A color matrix on a premultiplied color, as BACKDROP_MATRIX maps it.
        Rgba matrix(const BackdropConstants& k, const Rgba& c) {
            Rgba s = c[3] > 0 ? Rgba{c[0] / c[3], c[1] / c[3], c[2] / c[3], c[3]} : Rgba{};
            const bool linear = k.texel[0] > .5f;
            if (linear) {
                for (unsigned i = 0; i < 3; ++i) {
                    s[i] = srgb_to_linear(s[i]);
                }
            }
            Rgba m{};
            for (unsigned r = 0; r < 4; ++r) {
                float v = k.offsets[r];
                for (unsigned i = 0; i < 4; ++i) {
                    v += k.rows[r][i] * s[i];
                }
                m[r] = std::clamp(v, 0.f, 1.f);
            }
            if (linear) {
                for (unsigned i = 0; i < 3; ++i) {
                    m[i] = linear_to_srgb(m[i]);
                }
            }
            return {m[0] * m[3], m[1] * m[3], m[2] * m[3], m[3]};
        }
    } // namespace

    FilterPixels run_filter_chain(const FilterChain& chain, FilterPixels content) {
        std::vector<FilterPixels> images(chain.images.size());
        images[0] = std::move(content);
        for (const auto& step : chain.steps) {
            const auto& from = images[step.source];
            auto& to = images[step.target];
            const auto size = chain.images[step.target];
            if (!step.over) {
                to = {size[0], size[1], std::vector<Rgba>(std::size_t(size[0]) * size[1])};
            }
            const auto& k = step.constants;
            for (std::uint32_t y = 0; y < size[1]; ++y) {
                for (std::uint32_t x = 0; x < size[0]; ++x) {
                    Rgba out{};
                    if (step.shader == FilterStep::Shader::Area) {
                        // The span of source texels the texel covers on the axis, each by its share.
                        const bool across = k.texel[1] > .5f;
                        const double step_size = k.texel[0];
                        const double start = (across ? x : y) * step_size;
                        const double end = start + step_size;
                        const int last = std::min(int(std::ceil(end)), int(k.texel[2]));
                        for (int i = int(std::floor(start)); i < last; ++i) {
                            const double w = std::min(end, i + 1.0) - std::max(start, double(i));
                            const auto& t = across ? at(from, i, int(y)) : at(from, int(x), i);
                            for (unsigned c = 0; c < 4; ++c) {
                                out[c] += float(t[c] * w);
                            }
                        }
                        for (auto& v : out) {
                            v = float(v / step_size);
                        }
                    } else if (step.shader == FilterStep::Shader::Blur) {
                        // Taps at texel centers, mirrored past the edges.
                        const bool across = k.texel[2] != 0;
                        const int taps = int(k.extent[0]);
                        for (int i = -taps; i <= taps; ++i) {
                            const auto a = std::size_t(std::abs(i));
                            const float w = k.weights[a / 4][a % 4];
                            const auto& t = across ? at(from, mirrored(int(x) + i, int(from.width)), int(y))
                                                   : at(from, int(x), mirrored(int(y) + i, int(from.height)));
                            for (unsigned c = 0; c < 4; ++c) {
                                out[c] += t[c] * w;
                            }
                        }
                    } else {
                        const float u = (float(x) + .5f) / float(size[0]) - k.texel[2];
                        const float v = (float(y) + .5f) / float(size[1]) - k.texel[3];
                        out = matrix(k, bilinear(from, u, v));
                    }
                    auto& pixel = to.rgba[std::size_t(y) * size[0] + x];
                    if (step.over) {
                        for (unsigned c = 0; c < 4; ++c) {
                            out[c] += pixel[c] * (1 - out[3]);
                        }
                    }
                    for (unsigned c = 0; c < 4; ++c) {
                        pixel[c] = stored(out[c]);
                    }
                }
            }
        }
        return std::move(images[chain.result]);
    }
} // namespace weft::render
