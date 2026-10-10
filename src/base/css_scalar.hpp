#pragma once

#include "base/result.hpp"

#include <cmath>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// Scalar CSS value helpers shared by the style, layout, scene and canvas profiles.
//
// They replace per-profile copies. Profiles keep their own grammar (accepted units, numeric limits)
// and their own diagnostic text by passing options and wrapping the result.
namespace weft::css {
    // Whitespace trimmed around declared values by the layout and scene profiles.
    inline constexpr std::string_view value_whitespace = " \t\r\n";
    // CSS Syntax whitespace, which also includes form feed (style sheets, canvas colors).
    inline constexpr std::string_view syntax_whitespace = " \t\r\n\f";

    inline std::string trim(std::string_view text, std::string_view whitespace = value_whitespace) {
        const auto first = text.find_first_not_of(whitespace);
        if (first == std::string_view::npos) {
            return {};
        }
        return std::string(text.substr(first, text.find_last_not_of(whitespace) - first + 1));
    }

    // Declared value of a property in a style map, or fallback when the property is absent.
    template <class StyleMap, class Key>
    std::string style_value(const StyleMap& style, const Key& property, const char* fallback = "") {
        const auto it = style.find(property);
        return it == style.end() ? std::string(fallback) : it->second;
    }

    struct SplitOptions {
        // Quote runs (' or ") protect delimiters.
        bool quotes = true;
        // [] nests like (); only style sheet syntax uses it.
        bool brackets = false;
        // Trim every part and drop parts that are empty after trimming.
        bool trim_parts = true;
    };

    enum class Nesting {
        Balanced,
        // A ) or ] closed more than was opened.
        Unmatched,
        // A (, [ or quote was still open at the end.
        Unclosed,
    };

    struct SplitParts {
        std::vector<std::string> parts;
        Nesting nesting = Nesting::Balanced;
    };

    // Splits text at delimiters outside parentheses (and, as configured, brackets and quotes).
    // Unbalanced input is still split, with the nesting recorded, so lenient callers can ignore it.
    inline SplitParts split(std::string_view text, char delimiter, SplitOptions options = {}) {
        SplitParts out;
        std::size_t start = 0;
        int paren = 0;
        int bracket = 0;
        char quote = 0;
        const auto emit = [&](std::string_view part) {
            if (!options.trim_parts) {
                out.parts.emplace_back(part);
                return;
            }
            auto trimmed = trim(part);
            if (!trimmed.empty()) {
                out.parts.push_back(std::move(trimmed));
            }
        };
        for (std::size_t p = 0; p < text.size(); ++p) {
            const char c = text[p];
            if (quote) {
                if (c == quote) {
                    quote = 0;
                }
                continue;
            }
            if (options.quotes && (c == '\'' || c == '"')) {
                quote = c;
                continue;
            }
            if (c == '(') {
                ++paren;
            } else if (c == ')') {
                --paren;
            } else if (options.brackets && c == '[') {
                ++bracket;
            } else if (options.brackets && c == ']') {
                --bracket;
            }
            if ((paren < 0 || bracket < 0) && out.nesting == Nesting::Balanced) {
                out.nesting = Nesting::Unmatched;
            }
            if (c == delimiter && !paren && !bracket) {
                emit(text.substr(start, p - start));
                start = p + 1;
            }
        }
        if (out.nesting == Nesting::Balanced && (paren || bracket || quote)) {
            out.nesting = Nesting::Unclosed;
        }
        emit(text.substr(start));
        return out;
    }

    enum class Unit {
        None,
        Px,
        Percent,
        Em,
    };

    struct UnitSyntax {
        bool px = true;
        bool percent = true;
        bool em = false;
    };

    struct UnitSplit {
        std::string_view number;
        Unit unit = Unit::None;
    };

    // Splits a dimension into its numeric text and an accepted unit suffix. Text without an accepted
    // suffix is returned whole with Unit::None, so a foreign unit then fails number parsing.
    constexpr UnitSplit split_unit(std::string_view text, UnitSyntax accepted = {}) noexcept {
        if (accepted.percent && text.ends_with('%')) {
            return {text.substr(0, text.size() - 1), Unit::Percent};
        }
        if (accepted.px && text.ends_with("px")) {
            return {text.substr(0, text.size() - 2), Unit::Px};
        }
        if (accepted.em && text.ends_with("em")) {
            return {text.substr(0, text.size() - 2), Unit::Em};
        }
        return {text, Unit::None};
    }

    // Parses the whole text as a finite float or double whose magnitude is at most max_magnitude.
    //
    // Uses the std::stof/std::stod grammar the profiles have always used: leading whitespace and C99
    // forms such as hexadecimal floats are accepted, trailing text, overflow and underflow are not.
    template <class T>
    Result<T> parse_number(const std::string& text, T max_magnitude = std::numeric_limits<T>::infinity()) {
        static_assert(std::is_same_v<T, float> || std::is_same_v<T, double>);
        std::size_t consumed = 0;
        T value{};
        try {
            if constexpr (std::is_same_v<T, float>) {
                value = std::stof(text, &consumed);
            } else {
                value = std::stod(text, &consumed);
            }
        } catch (const std::exception&) {
            return make_diagnostic(Diag::InvalidNumber, "invalid numeric value " + text);
        }
        if (consumed != text.size() || !std::isfinite(value) || std::abs(value) > max_magnitude) {
            return make_diagnostic(Diag::InvalidNumber, "invalid numeric value " + text);
        }
        return value;
    }

    // Rewrites the color forms parse_css_color does not accept: the black and white keywords
    // and, when expand_short_hex is set, #rgb short hex.
    inline std::string expand_color_shorthand(std::string text, bool expand_short_hex = true) {
        if (text == "black") {
            return "#000000";
        }
        if (text == "white") {
            return "#ffffff";
        }
        if (expand_short_hex && text.size() == 4 && text[0] == '#') {
            return std::string("#") + text[1] + text[1] + text[2] + text[2] + text[3] + text[3];
        }
        return text;
    }
} // namespace weft::css
