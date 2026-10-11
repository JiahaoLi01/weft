#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// Fixed small grammars of the scene profile, parsed without std::regex. tests/grammar_differential_tests.cpp keeps
// each equal to the regex it replaced.
namespace weft::scene_grammar {
    // ECMAScript \s for char in the C locale.
    constexpr bool space(char c) noexcept {
        return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
    }

    // perspective(<distance>px) rotateY(<angle>[deg]), the one projective transform the scene profile draws. The
    // number texts are the runs perspective\(\s*([\d.]+)px\s*\)\s+rotateY\(\s*([-\d.]+)(deg)?\s*\) captures;
    // callers parse them.
    struct Perspective {
        std::string distance;
        std::string angle;
        bool degrees{};
    };

    // The grammar starting at offset at; *end receives the offset after its closing parenthesis.
    inline std::optional<Perspective> perspective_at(std::string_view text, std::size_t at,
                                                     std::size_t* end = nullptr) {
        auto i = at;
        const auto literal = [&](std::string_view s) {
            if (!text.substr(i).starts_with(s)) {
                return false;
            }
            i += s.size();
            return true;
        };
        const auto spaces = [&] {
            const auto from = i;
            while (i < text.size() && space(text[i])) {
                ++i;
            }
            return i - from;
        };
        const auto run = [&](bool minus) {
            const auto from = i;
            while (i < text.size() &&
                   ((text[i] >= '0' && text[i] <= '9') || text[i] == '.' || (minus && text[i] == '-'))) {
                ++i;
            }
            return text.substr(from, i - from);
        };
        if (!literal("perspective(")) {
            return {};
        }
        spaces();
        const auto distance = run(false);
        if (distance.empty() || !literal("px")) {
            return {};
        }
        spaces();
        if (!literal(")") || !spaces() || !literal("rotateY(")) {
            return {};
        }
        spaces();
        const auto angle = run(true);
        if (angle.empty()) {
            return {};
        }
        const bool degrees = literal("deg");
        spaces();
        if (!literal(")")) {
            return {};
        }
        if (end) {
            *end = i;
        }
        return Perspective{std::string(distance), std::string(angle), degrees};
    }

    // The whole text (std::regex_match).
    inline std::optional<Perspective> perspective(std::string_view text) {
        std::size_t end{};
        auto out = perspective_at(text, 0, &end);
        return out && end == text.size() ? out : std::nullopt;
    }

    // The first occurrence (std::regex_search).
    inline std::optional<Perspective> find_perspective(std::string_view text) {
        for (auto at = text.find("perspective("); at != text.npos; at = text.find("perspective(", at + 1)) {
            if (auto out = perspective_at(text, at)) {
                return out;
            }
        }
        return {};
    }
} // namespace weft::scene_grammar
