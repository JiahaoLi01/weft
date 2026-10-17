#include "scene/scene_border_image.hpp"

#include "base/css_scalar.hpp"
#include "constants/common.hpp"
#include "constants/scene.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace weft {
    namespace {
        enum class Kind {
            Number,
            Px,
            Percent,
            Auto,
        };

        struct Value {
            float number{};
            Kind kind{};
        };

        [[noreturn]] void outside(const char* property, std::string_view value) {
            throw std::invalid_argument(std::string(property) + " " + std::string(value) + " outside scene profile");
        }

        Value parse(std::string_view word, const char* property, std::string_view whole) {
            if (word == "auto") {
                return {0, Kind::Auto};
            }
            const auto [digits, unit] = css::split_unit(word);
            const auto n = css::parse_number<float>(std::string(digits),
                                                    static_cast<float>(constants::scene::max_numeric_magnitude));
            if (!n || *n < 0) {
                outside(property, whole);
            }
            return {*n, unit == css::Unit::Px ? Kind::Px : unit == css::Unit::Percent ? Kind::Percent : Kind::Number};
        }

        // One to four values of a box side list as top, right, bottom, left (CSS Backgrounds 3 §6.3).
        template <class T>
        std::array<T, 4> sides(const std::vector<T>& v, const char* property, std::string_view whole) {
            static constexpr int indices[4][4] = {{0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
            if (v.empty() || v.size() > 4) {
                outside(property, whole);
            }
            const auto& at = indices[v.size() - 1];
            return {v[at[0]], v[at[1]], v[at[2]], v[at[3]]};
        }

        std::vector<std::string> words(std::string_view value) {
            return css::split(value, ' ').parts;
        }
    } // namespace

    std::vector<SceneBorderImagePiece> scene_border_image(const StyleValues& style, float box_width, float box_height,
                                                          float image_width, float image_height) {
        // The border widths, top, right, bottom, left (0 for a side whose style is none, as the scene computes them).
        std::array<float, 4> border{};
        {
            unsigned k{};
            for (const auto id : {PropertyId::BorderTopWidth, PropertyId::BorderRightWidth,
                                  PropertyId::BorderBottomWidth, PropertyId::BorderLeftWidth}) {
                const auto text = std::string(style.text(id, "0"));
                const auto [digits, unit] = css::split_unit(text);
                const auto n = css::parse_number<float>(std::string(digits));
                if (!n || unit == css::Unit::Percent) {
                    outside("border width", text);
                }
                border[k++] = std::max(0.f, *n);
            }
        }

        // border-image-slice: numbers are image px, percentages of the image's size; fill keeps the middle.
        const auto slice_text = std::string(style.text(PropertyId::BorderImageSlice, "100%"));
        bool fill{};
        std::vector<Value> slice_values;
        for (const auto& w : words(slice_text)) {
            if (w == "fill") {
                fill = true;
                continue;
            }
            const auto v = parse(w, "border-image-slice", slice_text);
            if (v.kind != Kind::Number && v.kind != Kind::Percent) {
                outside("border-image-slice", slice_text);
            }
            slice_values.push_back(v);
        }
        const auto slice_sides = sides(slice_values, "border-image-slice", slice_text);
        std::array<float, 4> slice{};
        for (unsigned k = 0; k < 4; ++k) {
            const float extent = k % 2 == 0 ? image_height : image_width;
            const auto& v = slice_sides[k];
            slice[k] = std::clamp(v.kind == Kind::Percent ? v.number / constants::percent_scale * extent : v.number,
                                  0.f, extent);
        }

        // border-image-outset: numbers are multiples of the border width.
        const auto outset_text = std::string(style.text(PropertyId::BorderImageOutset, "0"));
        std::vector<Value> outset_values;
        for (const auto& w : words(outset_text)) {
            const auto v = parse(w, "border-image-outset", outset_text);
            if (v.kind != Kind::Number && v.kind != Kind::Px) {
                outside("border-image-outset", outset_text);
            }
            outset_values.push_back(v);
        }
        const auto outset_sides = sides(outset_values, "border-image-outset", outset_text);
        std::array<float, 4> outset{};
        for (unsigned k = 0; k < 4; ++k) {
            const auto& v = outset_sides[k];
            outset[k] = v.kind == Kind::Number ? v.number * border[k] : v.number;
        }
        // The border image area: the border box grown by the outsets.
        const float area_x = -outset[3];
        const float area_y = -outset[0];
        const float area_width = box_width + outset[1] + outset[3];
        const float area_height = box_height + outset[0] + outset[2];

        // border-image-width: numbers are multiples of the border width, percentages of the area, auto the slice's.
        const auto width_text = std::string(style.text(PropertyId::BorderImageWidth, "1"));
        std::vector<Value> width_values;
        for (const auto& w : words(width_text)) {
            width_values.push_back(parse(w, "border-image-width", width_text));
        }
        const auto width_sides = sides(width_values, "border-image-width", width_text);
        std::array<float, 4> width{};
        for (unsigned k = 0; k < 4; ++k) {
            const auto& v = width_sides[k];
            switch (v.kind) {
                case Kind::Number:
                    width[k] = v.number * border[k];
                    break;
                case Kind::Px:
                    width[k] = v.number;
                    break;
                case Kind::Percent:
                    width[k] = v.number / constants::percent_scale * (k % 2 == 0 ? area_height : area_width);
                    break;
                case Kind::Auto:
                    width[k] = slice[k];
                    break;
            }
        }
        // Opposite widths that would overlap are scaled down together (CSS Backgrounds 3 §6.4.1).
        float f = 1;
        if (width[1] + width[3] > 0) {
            f = std::min(f, area_width / (width[1] + width[3]));
        }
        if (width[0] + width[2] > 0) {
            f = std::min(f, area_height / (width[0] + width[2]));
        }
        f = std::max(f, 0.f);
        for (auto& w : width) {
            w *= f;
        }

        const std::array<float, 4> source_x{0, slice[3], image_width - slice[1], image_width};
        const std::array<float, 4> source_y{0, slice[0], image_height - slice[2], image_height};
        const std::array<float, 4> target_x{area_x, area_x + width[3], area_x + area_width - width[1],
                                            area_x + area_width};
        const std::array<float, 4> target_y{area_y, area_y + width[0], area_y + area_height - width[2],
                                            area_y + area_height};
        std::vector<SceneBorderImagePiece> pieces;
        for (unsigned row = 0; row < 3; ++row) {
            for (unsigned column = 0; column < 3; ++column) {
                if (row == 1 && column == 1 && !fill) {
                    continue;
                }
                const float sw = source_x[column + 1] - source_x[column];
                const float sh = source_y[row + 1] - source_y[row];
                const float tw = target_x[column + 1] - target_x[column];
                const float th = target_y[row + 1] - target_y[row];
                if (sw <= 0 || sh <= 0 || tw <= 0 || th <= 0) {
                    continue;
                }
                pieces.push_back(
                    {{source_x[column], source_y[row], sw, sh}, {target_x[column], target_y[row], tw, th}});
            }
        }
        return pieces;
    }

    bool scene_border_image_stretches(const StyleValues& style) {
        const auto repeat = words(style.text(PropertyId::BorderImageRepeat, "stretch"));
        return std::all_of(repeat.begin(), repeat.end(), [](const std::string& w) {
            return w == "stretch";
        });
    }
} // namespace weft
