#pragma once

#include "base/css_scalar.hpp"
#include "base/diagnostic.hpp"
#include "constants/common.hpp"
#include "constants/scene.hpp"
#include "render/rounded_clip.hpp"
#include "style/values.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>

// Computed style values of the scene renderers (scene_geometry, scene_text_image and the D2D reference raster): one
// parser, so every renderer resolves a length to the same float.
namespace weft::scene_values {
    using Style = StyleValues;

    [[noreturn]] inline void fail(const std::string& s) {
        throw_diagnostic(Diag::SceneRaster, s);
    }

    inline std::string get(const Style& s, const std::string& k, const char* fallback = "") {
        return css::style_value(s, k, fallback);
    }

    inline std::string get(const Style& s, PropertyId k, const char* fallback = "") {
        return std::string(s.text(k, fallback));
    }

    // Element attributes stay a string map.
    inline std::string get(const std::map<std::string, std::string>& s, const std::string& k,
                           const char* fallback = "") {
        return css::style_value(s, k, fallback);
    }

    inline float num(const std::string& s) {
        const auto n = css::parse_number<float>(s, static_cast<float>(constants::scene::max_numeric_magnitude));
        if (!n) {
            fail("numeric value " + s);
        }
        return *n;
    }

    // Unitless numbers are accepted as px; em resolves against reference; calc() takes a + b.
    inline float len(std::string_view input, float reference = 1) {
        const auto s = css::trim(input);
        if (s.starts_with("calc(")) {
            const auto plus = s.find(" + ");
            if (plus == s.npos) {
                fail("mixed length calc");
            }
            return len(s.substr(5, plus - 5), reference) + len(s.substr(plus + 3, s.size() - plus - 4), reference);
        }
        const auto [digits, unit] = css::split_unit(s, {.px = true, .percent = true, .em = true});
        const float n = num(std::string(digits));
        if (unit == css::Unit::Percent) {
            return n * reference / constants::percent_scale;
        }
        return n * (unit == css::Unit::Em ? reference : 1);
    }

    // Corner radii of a w x h box as the scene paints them, top-left first and clockwise: circular, percentages of
    // the shorter side, reduced together so that adjacent corners never overlap.
    inline std::array<float, 4> corner_radii(const Style& s, float w, float h) {
        std::array<float, 4> r{};
        unsigned k{};
        for (const auto id : {PropertyId::BorderTopLeftRadius, PropertyId::BorderTopRightRadius,
                              PropertyId::BorderBottomRightRadius, PropertyId::BorderBottomLeftRadius}) {
            r[k++] = std::max(0.f, len(get(s, id, "0"), std::min(w, h)));
        }
        float factor = 1;
        for (auto q : std::array<std::array<float, 2>, 4>{
                 {{w, r[0] + r[1]}, {w, r[2] + r[3]}, {h, r[0] + r[3]}, {h, r[1] + r[2]}}}) {
            if (q[1] > 0) {
                factor = std::min(factor, q[0] / q[1]);
            }
        }
        for (auto& v : r) {
            v *= factor;
        }
        return r;
    }

    // The rounded border box of a w x h box in its own coordinates, when a corner is rounded.
    inline std::optional<RoundedClip> rounded_box(const Style& s, float w, float h) {
        const auto r = corner_radii(s, w, h);
        if (std::all_of(r.begin(), r.end(), [](float v) {
                return v <= 0;
            })) {
            return std::nullopt;
        }
        return RoundedClip{{0, 0, w, h}, r, r};
    }
} // namespace weft::scene_values
