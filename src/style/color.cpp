#include "style/color.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

namespace weft::color {
    namespace {
        struct NamedColor {
            std::string_view name;
            std::uint32_t rgb;
        };

        constexpr NamedColor named_colors[] = {
#include "canvas/css_named_colors.inc"
        };

        using Vector = std::array<double, 3>;
        using Matrix = std::array<Vector, 3>;

        Vector multiply(const Matrix& m, const Vector& v) {
            return {m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2], m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
                    m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
        }

        constexpr double pi = 3.14159265358979323846;

        // The matrices and transfer functions of CSS Color 4 §18 (sample code).
        constexpr Matrix linear_srgb_to_xyz{{{0.41239079926595934, 0.357584339383878, 0.1804807884018343},
                                             {0.21263900587151027, 0.715168678767756, 0.07219231536073371},
                                             {0.01933081871559182, 0.11919477979462598, 0.9505321522496607}}};
        constexpr Matrix xyz_to_linear_srgb{{{3.2409699419045226, -1.537383177570094, -0.4986107602930034},
                                             {-0.9692436362808796, 1.8759675015077202, 0.04155505740717559},
                                             {0.05563007969699366, -0.20397695888897652, 1.0569715142428786}}};
        constexpr Matrix d65_to_d50{{{1.0479298208405488, 0.022946793341019088, -0.05019222954313557},
                                     {0.029627815688159344, 0.990434484573249, -0.01707382502938514},
                                     {-0.009243058152591178, 0.015055144896577895, 0.7518742899580008}}};
        constexpr Matrix d50_to_d65{{{0.9554734527042182, -0.023098536874261423, 0.0632593086610217},
                                     {-0.028369706963208136, 1.0099954580058226, 0.021041398966943008},
                                     {0.012314001688319899, -0.020507696433477912, 1.3303659366080753}}};
        constexpr Matrix linear_p3_to_xyz{{{0.4865709486482162, 0.26566769316909306, 0.1982172852343625},
                                           {0.2289745640697488, 0.6917385218365064, 0.079286914093745},
                                           {0.0, 0.04511338185890264, 1.043944368900976}}};
        constexpr Matrix xyz_to_linear_p3{{{2.493496911941425, -0.9313836179191239, -0.40271078445071684},
                                           {-0.8294889695615747, 1.7626640603183463, 0.023624685841943577},
                                           {0.03584583024378447, -0.07617238926804182, 0.9568845240076872}}};
        constexpr Matrix linear_a98_to_xyz{{{0.5766690429101305, 0.1855582379065463, 0.1882286462349947},
                                            {0.29734497525053605, 0.6273635662554661, 0.07529145849399788},
                                            {0.02703136138641234, 0.07068885253582723, 0.9913375368376388}}};
        constexpr Matrix xyz_to_linear_a98{{{2.0415879038107465, -0.5650069742788596, -0.34473135077832956},
                                            {-0.9692436362808795, 1.8759675015077202, 0.04155505740717557},
                                            {0.013444280632031142, -0.11836239223101838, 1.0151749943912054}}};
        // ProPhoto RGB is relative to D50.
        constexpr Matrix linear_prophoto_to_xyz_d50{{{0.7977604896723027, 0.13518583717574031, 0.0313493495815248},
                                                     {0.2880711282292934, 0.7118432178101014, 0.00008565396060525902},
                                                     {0.0, 0.0, 0.8251046025104601}}};
        constexpr Matrix xyz_d50_to_linear_prophoto{{{1.3457989731028281, -0.25558010007997534, -0.05110628506753401},
                                                     {-0.5446224939028347, 1.5082327413132781, 0.02053603239147973},
                                                     {0.0, 0.0, 1.2119675456389454}}};
        constexpr Matrix linear_rec2020_to_xyz{{{0.6369580483012914, 0.14461690358620832, 0.1688809751641721},
                                                {0.2627002120112671, 0.6779980715188708, 0.05930171646986196},
                                                {0.0, 0.028072693049087428, 1.060985057710791}}};
        constexpr Matrix xyz_to_linear_rec2020{{{1.716651187971268, -0.355670783776392, -0.253366281373660},
                                                {-0.666684351832489, 1.616481236634939, 0.0157685458139111},
                                                {0.017639857445311, -0.042770613257809, 0.942103121235474}}};
        constexpr Matrix xyz_to_lms{{{0.8190224379967030, 0.3619062600528904, -0.1288737815209879},
                                     {0.0329836539323885, 0.9292868615863434, 0.0361446663506424},
                                     {0.0481771893596242, 0.2642395317527308, 0.6335478284694309}}};
        constexpr Matrix lms_to_oklab{{{0.2104542683093140, 0.7936177747023054, -0.0040720430116193},
                                       {1.9779985324311684, -2.4285922420485799, 0.4505937096174110},
                                       {0.0259040424655478, 0.7827717124575296, -0.8086757549230774}}};
        constexpr Matrix oklab_to_lms{{{1.0, 0.3963377773761749, 0.2158037573099136},
                                       {1.0, -0.1055613458156586, -0.0638541728258133},
                                       {1.0, -0.0894841775298119, -1.2914855480194092}}};
        constexpr Matrix lms_to_xyz{{{1.2268798758459243, -0.5578149944602171, 0.2813910456659647},
                                     {-0.0405757452148008, 1.1122868032803170, -0.0717110580655164},
                                     {-0.0763729366746601, -0.4214933324022432, 1.5869240198367816}}};
        constexpr Vector d50_white{0.3457 / 0.3585, 1.0, (1.0 - 0.3457 - 0.3585) / 0.3585};

        // sRGB's transfer function (also Display P3's), extended to negative values by sign.
        double srgb_to_linear(double c) {
            const double a = std::abs(c);
            const double v = a <= 0.04045 ? a / 12.92 : std::pow((a + 0.055) / 1.055, 2.4);
            return std::copysign(v, c);
        }

        double linear_to_srgb(double c) {
            const double a = std::abs(c);
            const double v = a <= 0.0031308 ? 12.92 * a : 1.055 * std::pow(a, 1 / 2.4) - 0.055;
            return std::copysign(v, c);
        }

        double a98_to_linear(double c) {
            return std::copysign(std::pow(std::abs(c), 563.0 / 256.0), c);
        }

        double linear_to_a98(double c) {
            return std::copysign(std::pow(std::abs(c), 256.0 / 563.0), c);
        }

        double prophoto_to_linear(double c) {
            const double a = std::abs(c);
            return std::copysign(a <= 16.0 / 512.0 ? a / 16 : std::pow(a, 1.8), c);
        }

        double linear_to_prophoto(double c) {
            const double a = std::abs(c);
            return std::copysign(a >= 1.0 / 512.0 ? std::pow(a, 1 / 1.8) : 16 * a, c);
        }

        constexpr double rec2020_alpha = 1.09929682680944;
        constexpr double rec2020_beta = 0.018053968510807;

        double rec2020_to_linear(double c) {
            const double a = std::abs(c);
            return std::copysign(
                a < rec2020_beta * 4.5 ? a / 4.5 : std::pow((a + rec2020_alpha - 1) / rec2020_alpha, 1 / 0.45), c);
        }

        double linear_to_rec2020(double c) {
            const double a = std::abs(c);
            return std::copysign(a > rec2020_beta ? rec2020_alpha * std::pow(a, 0.45) - (rec2020_alpha - 1) : 4.5 * a,
                                 c);
        }

        template <class F>
        Vector each(const Vector& v, F f) {
            return {f(v[0]), f(v[1]), f(v[2])};
        }

        Vector srgb_to_xyz(const Vector& srgb) {
            return multiply(linear_srgb_to_xyz, each(srgb, srgb_to_linear));
        }

        Vector xyz_to_srgb(const Vector& xyz) {
            return each(multiply(xyz_to_linear_srgb, xyz), linear_to_srgb);
        }

        constexpr double lab_epsilon = 216.0 / 24389.0;
        constexpr double lab_kappa = 24389.0 / 27.0;

        Vector xyz_d50_to_lab(const Vector& xyz) {
            const auto f = [](double t) {
                return t > lab_epsilon ? std::cbrt(t) : (lab_kappa * t + 16) / 116;
            };
            const double fx = f(xyz[0] / d50_white[0]);
            const double fy = f(xyz[1] / d50_white[1]);
            const double fz = f(xyz[2] / d50_white[2]);
            return {116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)};
        }

        Vector lab_to_xyz_d50(const Vector& lab) {
            const double fy = (lab[0] + 16) / 116;
            const double fx = lab[1] / 500 + fy;
            const double fz = fy - lab[2] / 200;
            const double x = fx * fx * fx > lab_epsilon ? fx * fx * fx : (116 * fx - 16) / lab_kappa;
            const double y = lab[0] > lab_kappa * lab_epsilon ? fy * fy * fy : lab[0] / lab_kappa;
            const double z = fz * fz * fz > lab_epsilon ? fz * fz * fz : (116 * fz - 16) / lab_kappa;
            return {x * d50_white[0], y * d50_white[1], z * d50_white[2]};
        }

        Vector xyz_to_oklab(const Vector& xyz) {
            const auto lms = multiply(xyz_to_lms, xyz);
            return multiply(lms_to_oklab, each(lms, [](double c) {
                                return std::cbrt(c);
                            }));
        }

        Vector oklab_to_xyz(const Vector& lab) {
            const auto lms = multiply(oklab_to_lms, lab);
            return multiply(lms_to_xyz, each(lms, [](double c) {
                                return c * c * c;
                            }));
        }

        // Rectangular to polar (L, C, H in degrees, NaN when the color has no hue) and back.
        Vector to_polar(const Vector& lab, double achromatic) {
            const double c = std::sqrt(lab[1] * lab[1] + lab[2] * lab[2]);
            double h = std::atan2(lab[2], lab[1]) * 180 / pi;
            if (h < 0) {
                h += 360;
            }
            return {lab[0], c, c <= achromatic ? std::numeric_limits<double>::quiet_NaN() : h};
        }

        Vector from_polar(const Vector& lch) {
            const double h = std::isnan(lch[2]) ? 0 : lch[2] * pi / 180;
            return {lch[0], lch[1] * std::cos(h), lch[1] * std::sin(h)};
        }

        Vector hsl_to_srgb(Vector hsl) {
            double hue = std::isnan(hsl[0]) ? 0 : std::fmod(hsl[0], 360.0);
            if (hue < 0) {
                hue += 360;
            }
            const double s = hsl[1] / 100;
            const double l = hsl[2] / 100;
            const auto f = [&](double n) {
                const double k = std::fmod(n + hue / 30, 12.0);
                const double a = s * std::min(l, 1 - l);
                return l - a * std::max(-1.0, std::min({k - 3, 9 - k, 1.0}));
            };
            return {f(0), f(8), f(4)};
        }

        Vector srgb_to_hsl(const Vector& rgb) {
            const double max = std::max({rgb[0], rgb[1], rgb[2]});
            const double min = std::min({rgb[0], rgb[1], rgb[2]});
            double hue = std::numeric_limits<double>::quiet_NaN();
            double s = 0;
            const double l = (min + max) / 2;
            const double d = max - min;
            if (d != 0) {
                s = (l == 0 || l == 1) ? 0 : (max - l) / std::min(l, 1 - l);
                if (max == rgb[0]) {
                    hue = (rgb[1] - rgb[2]) / d + (rgb[1] < rgb[2] ? 6 : 0);
                } else if (max == rgb[1]) {
                    hue = (rgb[2] - rgb[0]) / d + 2;
                } else {
                    hue = (rgb[0] - rgb[1]) / d + 4;
                }
                hue *= 60;
            }
            if (s < 0) {
                hue += 180;
                s = std::abs(s);
            }
            if (hue >= 360) {
                hue -= 360;
            }
            return {hue, s * 100, l * 100};
        }

        Vector hwb_to_srgb(const Vector& hwb) {
            double w = hwb[1] / 100;
            double b = hwb[2] / 100;
            if (w + b >= 1) {
                const double gray = w / (w + b);
                return {gray, gray, gray};
            }
            auto rgb = hsl_to_srgb({hwb[0], 100, 50});
            for (auto& c : rgb) {
                c = c * (1 - w - b) + w;
            }
            return rgb;
        }

        Vector srgb_to_hwb(const Vector& rgb) {
            const auto hsl = srgb_to_hsl(rgb);
            const double white = std::min({rgb[0], rgb[1], rgb[2]});
            const double black = 1 - std::max({rgb[0], rgb[1], rgb[2]});
            return {hsl[0], white * 100, black * 100};
        }
    } // namespace

    std::optional<Rgba> named(std::string_view name) {
        if (name == "transparent") {
            return Rgba{0, 0, 0, 0};
        }
        for (const auto& c : named_colors) {
            if (c.name == name) {
                return Rgba{double((c.rgb >> 16) & 0xff) / 255, double((c.rgb >> 8) & 0xff) / 255,
                            double(c.rgb & 0xff) / 255, 1};
            }
        }
        if (name == "rebeccapurple") {
            return Rgba{0x66 / 255.0, 0x33 / 255.0, 0x99 / 255.0, 1};
        }
        return std::nullopt;
    }

    std::optional<Space> space(std::string_view name) {
        constexpr std::pair<std::string_view, Space> spaces[]{{"srgb", Space::Srgb},
                                                              {"srgb-linear", Space::SrgbLinear},
                                                              {"display-p3", Space::DisplayP3},
                                                              {"a98-rgb", Space::A98Rgb},
                                                              {"prophoto-rgb", Space::ProphotoRgb},
                                                              {"rec2020", Space::Rec2020},
                                                              {"xyz", Space::XyzD65},
                                                              {"xyz-d65", Space::XyzD65},
                                                              {"xyz-d50", Space::XyzD50},
                                                              {"lab", Space::Lab},
                                                              {"lch", Space::Lch},
                                                              {"oklab", Space::Oklab},
                                                              {"oklch", Space::Oklch},
                                                              {"hsl", Space::Hsl},
                                                              {"hwb", Space::Hwb}};
        for (const auto& [n, s] : spaces) {
            if (n == name) {
                return s;
            }
        }
        return std::nullopt;
    }

    bool polar(Space space) {
        return space == Space::Lch || space == Space::Oklch || space == Space::Hsl || space == Space::Hwb;
    }

    int hue_index(Space space) {
        return space == Space::Hsl || space == Space::Hwb ? 0 : 2;
    }

    std::array<double, 3> from_srgb(Space space, const std::array<double, 3>& srgb) {
        switch (space) {
            case Space::Srgb:
                return srgb;
            case Space::SrgbLinear:
                return each(srgb, srgb_to_linear);
            case Space::DisplayP3:
                return each(multiply(xyz_to_linear_p3, srgb_to_xyz(srgb)), linear_to_srgb);
            case Space::A98Rgb:
                return each(multiply(xyz_to_linear_a98, srgb_to_xyz(srgb)), linear_to_a98);
            case Space::ProphotoRgb:
                return each(multiply(xyz_d50_to_linear_prophoto, multiply(d65_to_d50, srgb_to_xyz(srgb))),
                            linear_to_prophoto);
            case Space::Rec2020:
                return each(multiply(xyz_to_linear_rec2020, srgb_to_xyz(srgb)), linear_to_rec2020);
            case Space::XyzD65:
                return srgb_to_xyz(srgb);
            case Space::XyzD50:
                return multiply(d65_to_d50, srgb_to_xyz(srgb));
            case Space::Lab:
                return xyz_d50_to_lab(multiply(d65_to_d50, srgb_to_xyz(srgb)));
            case Space::Lch:
                return to_polar(xyz_d50_to_lab(multiply(d65_to_d50, srgb_to_xyz(srgb))), 0.0015);
            case Space::Oklab:
                return xyz_to_oklab(srgb_to_xyz(srgb));
            case Space::Oklch:
                return to_polar(xyz_to_oklab(srgb_to_xyz(srgb)), 0.000004);
            case Space::Hsl:
                return srgb_to_hsl(srgb);
            case Space::Hwb:
                return srgb_to_hwb(srgb);
        }
        return srgb;
    }

    std::array<double, 3> to_srgb(Space space, const std::array<double, 3>& c) {
        switch (space) {
            case Space::Srgb:
                return c;
            case Space::SrgbLinear:
                return each(c, linear_to_srgb);
            case Space::DisplayP3:
                return xyz_to_srgb(multiply(linear_p3_to_xyz, each(c, srgb_to_linear)));
            case Space::A98Rgb:
                return xyz_to_srgb(multiply(linear_a98_to_xyz, each(c, a98_to_linear)));
            case Space::ProphotoRgb:
                return xyz_to_srgb(
                    multiply(d50_to_d65, multiply(linear_prophoto_to_xyz_d50, each(c, prophoto_to_linear))));
            case Space::Rec2020:
                return xyz_to_srgb(multiply(linear_rec2020_to_xyz, each(c, rec2020_to_linear)));
            case Space::XyzD65:
                return xyz_to_srgb(c);
            case Space::XyzD50:
                return xyz_to_srgb(multiply(d50_to_d65, c));
            case Space::Lab:
                return xyz_to_srgb(multiply(d50_to_d65, lab_to_xyz_d50(c)));
            case Space::Lch:
                return xyz_to_srgb(multiply(d50_to_d65, lab_to_xyz_d50(from_polar(c))));
            case Space::Oklab:
                return xyz_to_srgb(oklab_to_xyz(c));
            case Space::Oklch:
                return xyz_to_srgb(oklab_to_xyz(from_polar(c)));
            case Space::Hsl:
                return hsl_to_srgb(c);
            case Space::Hwb:
                return hwb_to_srgb(c);
        }
        return c;
    }

    Rgba mix(Space space, HueInterpolation interpolation, const Rgba& a, double weight_a, const Rgba& b,
             double weight_b) {
        auto x = from_srgb(space, {a[0], a[1], a[2]});
        auto y = from_srgb(space, {b[0], b[1], b[2]});
        const int hue = polar(space) ? hue_index(space) : -1;
        // A color with no hue takes the other's (CSS Color 4 §12.2, missing components); a fully transparent color's
        // other components are missing too, so its premultiplied values do not pull the mix.
        if (hue >= 0) {
            if (std::isnan(x[hue]) && std::isnan(y[hue])) {
                x[hue] = y[hue] = 0;
            } else if (std::isnan(x[hue])) {
                x[hue] = y[hue];
            } else if (std::isnan(y[hue])) {
                y[hue] = x[hue];
            }
            double& h1 = x[hue];
            double& h2 = y[hue];
            const double d = h2 - h1;
            switch (interpolation) {
                case HueInterpolation::Shorter:
                    if (d > 180) {
                        h1 += 360;
                    } else if (d < -180) {
                        h2 += 360;
                    }
                    break;
                case HueInterpolation::Longer:
                    if (d > 0 && d < 180) {
                        h1 += 360;
                    } else if (d > -180 && d <= 0) {
                        h2 += 360;
                    }
                    break;
                case HueInterpolation::Increasing:
                    if (d < 0) {
                        h2 += 360;
                    }
                    break;
                case HueInterpolation::Decreasing:
                    if (d > 0) {
                        h1 += 360;
                    }
                    break;
            }
        }
        const double alpha = a[3] * weight_a + b[3] * weight_b;
        std::array<double, 3> out{};
        for (int i = 0; i < 3; ++i) {
            if (i == hue) {
                out[std::size_t(i)] = x[std::size_t(i)] * weight_a + y[std::size_t(i)] * weight_b;
                out[std::size_t(i)] = std::fmod(out[std::size_t(i)], 360.0);
                if (out[std::size_t(i)] < 0) {
                    out[std::size_t(i)] += 360;
                }
            } else {
                const double premultiplied = x[std::size_t(i)] * a[3] * weight_a + y[std::size_t(i)] * b[3] * weight_b;
                out[std::size_t(i)] = alpha == 0 ? 0 : premultiplied / alpha;
            }
        }
        const auto rgb = to_srgb(space, out);
        return {rgb[0], rgb[1], rgb[2], alpha};
    }

    Rgba clip(Rgba color) {
        for (auto& c : color) {
            c = std::isfinite(c) ? std::clamp(c, 0.0, 1.0) : 0.0;
        }
        return color;
    }
} // namespace weft::color
