#include "scene/scene_filter.hpp"

#include "base/css_scalar.hpp"
#include "base/diagnostic.hpp"
#include "constants/common.hpp"
#include "constants/scene.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>
#include <string>

namespace weft {
    namespace {
        using Matrix = paint::ColorMatrix;

        // a, then b: b times a, both 4 x 5 maps of straight RGBA with their offsets in the fifth column.
        Matrix then(const Matrix& a, const Matrix& b) {
            Matrix out{};
            for (unsigned r = 0; r < 4; ++r) {
                for (unsigned c = 0; c < 5; ++c) {
                    float v = c == 4 ? b[r * 5 + 4] : 0;
                    for (unsigned k = 0; k < 4; ++k) {
                        v += b[r * 5 + k] * a[k * 5 + c];
                    }
                    out[r * 5 + c] = v;
                }
            }
            return out;
        }

        // The matrix that maps r, g and b by m (row-major 3 x 3) plus offset, and keeps alpha.
        Matrix rgb(const std::array<float, 9>& m, const std::array<float, 3>& offset = {}) {
            Matrix out = paint::identity_color_matrix;
            for (unsigned r = 0; r < 3; ++r) {
                for (unsigned c = 0; c < 3; ++c) {
                    out[r * 5 + c] = m[r * 3 + c];
                }
                out[r * 5 + 4] = offset[r];
            }
            return out;
        }

        std::optional<float> number(std::string_view text) {
            const auto n = css::parse_number<float>(std::string(text),
                                                    static_cast<float>(constants::scene::max_numeric_magnitude));
            return n ? std::optional<float>(*n) : std::nullopt;
        }
    } // namespace

    Result<std::optional<SceneBackdropFilter>> scene_backdrop_filter(std::string_view value) {
        const auto text = css::trim(value);
        if (text.empty() || text == "none") {
            return std::optional<SceneBackdropFilter>{};
        }
        const auto refuse = [&](const std::string& why) {
            return make_diagnostic(Diag::SceneProfile, "backdrop-filter " + text + ": " + why);
        };
        SceneBackdropFilter out;
        bool blurred{};
        std::size_t at{};
        while (at < text.size()) {
            if (text[at] == ' ') {
                ++at;
                continue;
            }
            const auto open = text.find('(', at);
            const auto close = open == std::string::npos ? open : text.find(')', open);
            if (close == std::string::npos) {
                return refuse("filter functions only");
            }
            const auto name = css::trim(std::string_view(text).substr(at, open - at));
            const auto argument = css::trim(std::string_view(text).substr(open + 1, close - open - 1));
            at = close + 1;
            if (name == "blur") {
                if (blurred) {
                    return refuse("one blur()");
                }
                blurred = true;
                // A length in px; 0 may go without its unit.
                const auto digits = argument.ends_with("px") ? std::string_view(argument).substr(0, argument.size() - 2)
                                                             : std::string_view(argument);
                const auto px = argument.empty() ? std::optional<float>(0.f) : number(digits);
                if (!px || !(*px >= 0) || *px > constants::max_backdrop_blur ||
                    (!argument.ends_with("px") && *px != 0)) {
                    return refuse("blur() takes a length of 0 to " + std::to_string(int(constants::max_backdrop_blur)) +
                                  "px");
                }
                out.blur = *px;
                continue;
            }
            constexpr std::string_view color_functions[]{"brightness", "contrast", "grayscale", "sepia",
                                                         "saturate",   "invert",   "opacity",   "hue-rotate"};
            if (std::find(std::begin(color_functions), std::end(color_functions), name) == std::end(color_functions)) {
                return refuse(std::string(name) + "() is not a filter function the scene takes");
            }
            // A color function's amount: a number or a percentage, 1 when left out, at least 0.
            std::optional<float> amount = 1.f;
            if (!argument.empty() && name != "hue-rotate") {
                amount = argument.ends_with('%') ? number(std::string_view(argument).substr(0, argument.size() - 1))
                                                 : number(argument);
                if (amount && argument.ends_with('%')) {
                    *amount /= 100;
                }
                if (!amount || !(*amount >= 0)) {
                    return refuse(std::string(name) + "() takes a number or a percentage of 0 or more");
                }
            }
            const float a = *amount;
            const float capped = std::min(a, 1.f);
            Matrix m = paint::identity_color_matrix;
            if (name == "brightness") {
                m = rgb({a, 0, 0, 0, a, 0, 0, 0, a});
            } else if (name == "contrast") {
                const float b = .5f - .5f * a;
                m = rgb({a, 0, 0, 0, a, 0, 0, 0, a}, {b, b, b});
            } else if (name == "grayscale") {
                const float k = 1 - capped;
                m = rgb({.2126f + .7874f * k, .7152f - .7152f * k, .0722f - .0722f * k, .2126f - .2126f * k,
                         .7152f + .2848f * k, .0722f - .0722f * k, .2126f - .2126f * k, .7152f - .7152f * k,
                         .0722f + .9278f * k});
            } else if (name == "sepia") {
                const float k = 1 - capped;
                m = rgb({.393f + .607f * k, .769f - .769f * k, .189f - .189f * k, .349f - .349f * k, .686f + .314f * k,
                         .168f - .168f * k, .272f - .272f * k, .534f - .534f * k, .131f + .869f * k});
            } else if (name == "saturate") {
                m = rgb({.213f + .787f * a, .715f - .715f * a, .072f - .072f * a, .213f - .213f * a, .715f + .285f * a,
                         .072f - .072f * a, .213f - .213f * a, .715f - .715f * a, .072f + .928f * a});
            } else if (name == "invert") {
                m = rgb({1 - 2 * capped, 0, 0, 0, 1 - 2 * capped, 0, 0, 0, 1 - 2 * capped}, {capped, capped, capped});
            } else if (name == "opacity") {
                m[3 * 5 + 3] = capped;
            } else if (name == "hue-rotate") {
                // An angle; 0 may go without its unit.
                double degrees{};
                const std::pair<const char*, double> units[]{
                    {"deg", 1}, {"grad", .9}, {"rad", 180 / std::numbers::pi}, {"turn", 360}};
                bool parsed = argument.empty();
                for (const auto& [unit, factor] : units) {
                    if (!parsed && argument.ends_with(unit)) {
                        const auto n = number(
                            std::string_view(argument).substr(0, argument.size() - std::string_view(unit).size()));
                        if (!n) {
                            return refuse("hue-rotate() takes an angle");
                        }
                        degrees = *n * factor;
                        parsed = true;
                    }
                }
                if (!parsed) {
                    const auto n = number(argument);
                    if (!n || *n != 0) {
                        return refuse("hue-rotate() takes an angle");
                    }
                }
                const float c = float(std::cos(degrees * std::numbers::pi / 180));
                const float s = float(std::sin(degrees * std::numbers::pi / 180));
                m = rgb({.213f + c * .787f - s * .213f, .715f - c * .715f - s * .715f, .072f - c * .072f + s * .928f,
                         .213f - c * .213f + s * .143f, .715f + c * .285f + s * .140f, .072f - c * .072f - s * .283f,
                         .213f - c * .213f - s * .787f, .715f - c * .715f + s * .715f, .072f + c * .928f + s * .072f});
            }
            auto& into = blurred ? out.post : out.pre;
            into = then(into, m);
        }
        return std::optional<SceneBackdropFilter>(out);
    }
} // namespace weft
