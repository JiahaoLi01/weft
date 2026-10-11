#pragma once

#include "base/result.hpp"

#include <cmath>
#include <exception>
#include <limits>
#include <optional>
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

    // Declared value of a property in a string-keyed style map, or fallback when the property is absent.
    // StyleValues (style/values.hpp) has its own overload.
    template <class StyleMap, class Key>
        requires requires { typename StyleMap::mapped_type; }
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

    // Offset and length of one number inside a value.
    struct NumberSpan {
        std::size_t offset{};
        std::size_t length{};
    };

    // The numbers inside a value, left to right, as interpolation splits lengths, percentages and transform
    // arguments: what the ECMAScript regex [-+]?(?:\d*\.\d+|\d+\.?\d*)(?:[eE][-+]?\d+)? finds.
    inline std::vector<NumberSpan> number_spans(std::string_view text) {
        const auto digit = [&](std::size_t i) {
            return i < text.size() && text[i] >= '0' && text[i] <= '9';
        };
        // Length of the unsigned number at i: \d*\.\d+ when it matches, else \d+\.?\d*, then an exponent.
        const auto number = [&](std::size_t i) -> std::size_t {
            auto end = i;
            while (digit(end)) {
                ++end;
            }
            if (end < text.size() && text[end] == '.' && digit(end + 1)) {
                for (++end; digit(end); ++end) {
                }
            } else if (end > i) {
                if (end < text.size() && text[end] == '.') {
                    ++end;
                }
            } else {
                return 0;
            }
            if (end < text.size() && (text[end] == 'e' || text[end] == 'E')) {
                auto exponent = end + 1;
                if (exponent < text.size() && (text[exponent] == '+' || text[exponent] == '-')) {
                    ++exponent;
                }
                if (digit(exponent)) {
                    for (end = exponent; digit(end); ++end) {
                    }
                }
            }
            return end - i;
        };
        std::vector<NumberSpan> out;
        for (std::size_t i = 0; i < text.size();) {
            const std::size_t sign = text[i] == '+' || text[i] == '-' ? 1 : 0;
            const auto length = number(i + sign);
            if (!length) {
                ++i;
                continue;
            }
            out.push_back({i, sign + length});
            i += sign + length;
        }
        return out;
    }

    // Each run of spaces, tabs, carriage returns and line feeds becomes one space.
    inline std::string collapse_spaces(std::string_view text) {
        std::string out;
        out.reserve(text.size());
        bool run{};
        for (const char c : text) {
            const bool space = c == ' ' || c == '\t' || c == '\r' || c == '\n';
            if (!space || !run) {
                out += space ? ' ' : c;
            }
            run = space;
        }
        return out;
    }

    // Removes every closed /* ... */ comment, each ending at the first */ after its opening; an unclosed /* and the
    // text after it stay.
    inline std::string strip_block_comments(std::string_view text) {
        constexpr std::string_view opening = "/*";
        constexpr std::string_view closing = "*/";
        std::string out;
        std::size_t p{};
        for (;;) {
            const auto open = text.find(opening, p);
            const auto close = open == text.npos ? text.npos : text.find(closing, open + opening.size());
            if (close == text.npos) {
                break;
            }
            out.append(text.substr(p, open - p));
            p = close + closing.size();
        }
        out.append(text.substr(p));
        return out;
    }

    // Whether the whole text is a decimal number [+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:e[+-]?[0-9]+)?, the form
    // canvas color components take (no hexadecimal, no uppercase exponent).
    constexpr bool decimal_syntax(std::string_view text) noexcept {
        std::size_t i{};
        const auto digit = [&](std::size_t at) {
            return at < text.size() && text[at] >= '0' && text[at] <= '9';
        };
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            ++i;
        }
        if (digit(i)) {
            while (digit(i)) {
                ++i;
            }
            if (i < text.size() && text[i] == '.') {
                for (++i; digit(i); ++i) {
                }
            }
        } else if (i < text.size() && text[i] == '.' && digit(i + 1)) {
            for (++i; digit(i); ++i) {
            }
        } else {
            return false;
        }
        if (i < text.size() && text[i] == 'e') {
            ++i;
            if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
                ++i;
            }
            if (!digit(i)) {
                return false;
            }
            while (digit(i)) {
                ++i;
            }
        }
        return i == text.size();
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

    // ECMAScript \s for char in the C locale.
    constexpr bool ecma_space(char c) noexcept {
        return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
    }

    // repeat(<1-8>, minmax(0, 1fr)) as the whole text, with \s* around every token: the column count.
    inline std::optional<unsigned> grid_repeat(std::string_view text) {
        std::size_t i{};
        const auto spaces = [&] {
            while (i < text.size() && ecma_space(text[i])) {
                ++i;
            }
        };
        const auto token = [&](std::string_view s) {
            spaces();
            if (!text.substr(i).starts_with(s)) {
                return false;
            }
            i += s.size();
            return true;
        };
        constexpr std::string_view repeat = "repeat(";
        if (!text.starts_with(repeat)) {
            return {};
        }
        i = repeat.size();
        spaces();
        if (i >= text.size() || text[i] < '1' || text[i] > '8') {
            return {};
        }
        const auto count = unsigned(text[i++] - '0');
        if (!token(",") || !token("minmax(") || !token("0") || !token(",") || !token("1fr") || !token(")") ||
            !token(")")) {
            return {};
        }
        return i == text.size() ? std::optional(count) : std::nullopt;
    }
} // namespace weft::css
