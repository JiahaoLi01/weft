#include "caseelephant/base/css_scalar.hpp"
#include "support/check.hpp"
#include "caseelephant/base/diagnostic.hpp"
#include "caseelephant/base/generational_slots.hpp"
#include "caseelephant/base/owner_thread.hpp"
#include "caseelephant/base/rect.hpp"
#include "caseelephant/base/result.hpp"
#include "caseelephant/constants/common.hpp"
#include "caseelephant/constants/layout.hpp"
#include "caseelephant/constants/scene.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
    using namespace weft;

    using weft::testing::require;

    // Verbatim behavior of the per-profile helpers that src/base replaces. The differential tests
    // below require the shared helpers to reproduce each of them on random input.
    namespace legacy {
        std::string trim(std::string s, const char* whitespace) {
            auto a = s.find_first_not_of(whitespace);
            return a == s.npos ? "" : s.substr(a, s.find_last_not_of(whitespace) - a + 1);
        }

        // scene_probe.cpp: quote-aware, trimmed, empty parts dropped.
        std::vector<std::string> split_scene(std::string_view s, char delimiter) {
            std::vector<std::string> out;
            std::size_t start{};
            int depth{};
            char quote{};
            for (std::size_t p = 0; p < s.size(); ++p) {
                char c = s[p];
                if (quote) {
                    if (c == quote) {
                        quote = 0;
                    }
                } else if (c == '\'' || c == '"') {
                    quote = c;
                } else if (c == '(') {
                    ++depth;
                } else if (c == ')') {
                    --depth;
                } else if (c == delimiter && !depth) {
                    auto part = trim(std::string(s.substr(start, p - start)), " \t\r\n");
                    if (!part.empty()) {
                        out.push_back(part);
                    }
                    start = p + 1;
                }
            }
            auto last = trim(std::string(s.substr(start)), " \t\r\n");
            if (!last.empty()) {
                out.push_back(last);
            }
            return out;
        }

        // scene_native_probe.cpp and scene_d2d_probe.cpp: parentheses only.
        std::vector<std::string> split_native(std::string s, char c) {
            std::vector<std::string> out;
            std::size_t start{};
            int depth{};
            for (std::size_t i = 0; i < s.size(); ++i) {
                if (s[i] == '(') {
                    ++depth;
                }
                if (s[i] == ')') {
                    --depth;
                }
                if (s[i] == c && depth == 0) {
                    auto v = trim(s.substr(start, i - start), " \t\r\n");
                    if (!v.empty()) {
                        out.push_back(v);
                    }
                    start = i + 1;
                }
            }
            auto v = trim(s.substr(start), " \t\r\n");
            if (!v.empty()) {
                out.push_back(v);
            }
            return out;
        }

        // style_probe.cpp: untrimmed parts, brackets, strict nesting ("" when it would have failed).
        std::optional<std::vector<std::string>> split_style(std::string_view text, char separator,
                                                            std::string& failure) {
            std::vector<std::string> out;
            std::size_t start{};
            int paren{};
            int bracket{};
            char quote{};
            for (std::size_t p = 0; p < text.size(); ++p) {
                const char c = text[p];
                if (quote) {
                    if (c == quote) {
                        quote = 0;
                    }
                    continue;
                }
                if (c == '\'' || c == '"') {
                    quote = c;
                    continue;
                }
                if (c == '(') {
                    ++paren;
                } else if (c == ')') {
                    --paren;
                } else if (c == '[') {
                    ++bracket;
                } else if (c == ']') {
                    --bracket;
                }
                if (paren < 0 || bracket < 0) {
                    failure = "unmatched delimiter";
                    return std::nullopt;
                }
                if (c == separator && !paren && !bracket) {
                    out.push_back(std::string(text.substr(start, p - start)));
                    start = p + 1;
                }
            }
            if (paren || bracket || quote) {
                failure = "unbalanced syntax";
                return std::nullopt;
            }
            out.push_back(std::string(text.substr(start)));
            return out;
        }

        // positioned/auto/flow_page_probe.cpp: float, |n| <= limit, every failure rejected.
        std::optional<float> number_float(const std::string& v, float limit) {
            try {
                std::size_t end{};
                const float n = std::stof(v, &end);
                if (end == v.size() && std::isfinite(n) && std::abs(n) <= limit) {
                    return n;
                }
            } catch (const std::exception&) {
            }
            return std::nullopt;
        }

        // scene_probe.cpp: double, |n| <= limit; std::stod exceptions escaped unwrapped.
        std::optional<double> number_double(std::string s, double limit) {
            try {
                std::size_t p{};
                double n = std::stod(s, &p);
                if (p != s.size() || !std::isfinite(n) || std::abs(n) > limit) {
                    return std::nullopt;
                }
                return n;
            } catch (const std::exception&) {
                return std::nullopt;
            }
        }
    } // namespace legacy

    struct Random {
        std::uint32_t state = 0xce1a2026u;

        std::uint32_t next() {
            state = state * constants::lcg_multiplier + constants::lcg_increment;
            return state >> 8;
        }

        std::string text(std::string_view alphabet, std::size_t max_length) {
            std::string out;
            const auto length = next() % (max_length + 1);
            for (std::size_t i = 0; i < length; ++i) {
                out += alphabet[next() % alphabet.size()];
            }
            return out;
        }
    };

    void diagnostics() {
        const auto d = make_diagnostic(Diag::SceneProfile, "transform grammar");
        require(d.code == Diag::SceneProfile && d.message == "WEFT_SCENE_PROFILE: transform grammar",
                "diagnostic text");
        require(diag_message(Diag::CanvasVersionLimit, {}) == "WEFT_CANVAS_VERSION_LIMIT", "name-only diagnostic");
        require(diag_message(Diag::CanvasIndexSize, "negative arc radius") == "IndexSizeError: negative arc radius",
                "DOM exception name");
        try {
            throw_diagnostic(Diag::FlowProfile, "px required: 1em");
        } catch (const std::invalid_argument& e) {
            const auto* error = dynamic_cast<const DiagnosticError*>(&e);
            require(error && error->code() == Diag::FlowProfile, "DiagnosticError keeps its code");
            require(std::string(e.what()) == "WEFT_FLOW_PROFILE: px required: 1em", "DiagnosticError message");
        }
    }

    void results() {
        Result<int> ok = 7;
        require(ok && *ok == 7 && ok.value() == 7 && ok.value_or(1) == 7, "value result");
        Result<int> failed = make_diagnostic(Diag::InvalidNumber, "x");
        require(!failed && failed.error().code == Diag::InvalidNumber && failed.value_or(3) == 3, "error result");
        bool thrown = false;
        try {
            (void)failed.value();
        } catch (const DiagnosticError& e) {
            thrown = e.code() == Diag::InvalidNumber;
        }
        require(thrown, "value() bridges to DiagnosticError");
        Status done;
        Status rejected = make_diagnostic(Diag::StyleProfile, "rule bound");
        require(done && !rejected && rejected.error().message == "WEFT_STYLE_PROFILE: rule bound", "status");
        Result<std::string> moved = std::string("moved");
        require(std::move(moved).value() == "moved", "rvalue value");
    }

    void scalars() {
        require(css::trim(" \t a b \r\n") == "a b" && css::trim(" \t\r\n").empty(), "trim");
        require(css::trim("\fa\f", css::syntax_whitespace) == "a" && css::trim("\fa\f") == "\fa\f", "form feed");
        const std::map<std::string, std::string> style{{"display", "flex"}};
        require(css::style_value(style, "display") == "flex" && css::style_value(style, "width", "auto") == "auto",
                "style lookup");
        require(css::split_unit("12px").unit == css::Unit::Px && css::split_unit("12px").number == "12", "px");
        require(css::split_unit("50%").unit == css::Unit::Percent, "percent");
        require(css::split_unit("2em").unit == css::Unit::None && css::split_unit("2em", {.em = true}).number == "2",
                "em only when accepted");
        require(css::expand_color_shorthand("black") == "#000000" && css::expand_color_shorthand("#abc") == "#aabbcc" &&
                    css::expand_color_shorthand("#abc", false) == "#abc",
                "color shorthand");
        const auto parsed = css::parse_number<float>("1e9", 1e6f);
        require(!parsed && parsed.error().message == "WEFT_CSS_VALUE: invalid numeric value 1e9", "number limit text");
        require(css::parse_number<float>("1e-50").has_value() == false, "float underflow rejected like std::stof");
        require(css::parse_number<double>("1e-50").has_value(), "double keeps small values");
        const auto overlap = geometry::intersect({0, 0, 10, 10}, {5, 6, 10, 10});
        require(overlap.x == 5 && overlap.y == 6 && overlap.w == 5 && overlap.h == 4, "intersect");
        const auto disjoint = geometry::intersect({0, 0, 1, 1}, {5, 5, 1, 1});
        require(disjoint.x == 5 && disjoint.w == 0 && disjoint.h == 0, "disjoint intersect");
    }

    void split_differential() {
        Random random;
        constexpr std::string_view alphabet = "ab ,;()[]'\"\t";
        constexpr int cases = 20000;
        for (int i = 0; i < cases; ++i) {
            const auto text = random.text(alphabet, 24);
            for (char delimiter : {' ', ',', ';'}) {
                require(css::split(text, delimiter).parts == legacy::split_scene(text, delimiter),
                        "scene split differs on [" + text + "]");
                require(css::split(text, delimiter, {.quotes = false}).parts == legacy::split_native(text, delimiter),
                        "native split differs on [" + text + "]");
                std::string failure;
                const auto expected = legacy::split_style(text, delimiter, failure);
                const auto actual =
                    css::split(text, delimiter, {.quotes = true, .brackets = true, .trim_parts = false});
                if (expected) {
                    require(actual.nesting == css::Nesting::Balanced && actual.parts == *expected,
                            "style split differs on [" + text + "]");
                } else {
                    const auto nesting =
                        failure == "unmatched delimiter" ? css::Nesting::Unmatched : css::Nesting::Unclosed;
                    require(actual.nesting == nesting, "style nesting differs on [" + text + "]");
                }
            }
        }
    }

    void number_differential() {
        Random random;
        constexpr std::string_view alphabet = "0123456789.-+eE xp%";
        const std::vector<std::string> fixed{"",   "inf",   "-inf",   "nan",    "1e40",   "1e-50", "0x1p3",
                                             " 5", "5 ",    "100000", "100001", "1e7",    "1.5e7", "+.5",
                                             "-0", "1e400", "1e-400", "00012",  "999999", ".",     "-"};
        std::vector<std::string> inputs = fixed;
        for (int i = 0; i < 40000; ++i) {
            inputs.push_back(random.text(alphabet, 10));
        }
        const float layout_limit = static_cast<float>(constants::layout::max_length_magnitude_px);
        const double scene_limit = constants::scene::max_numeric_magnitude;
        for (const auto& input : inputs) {
            const auto expected_float = legacy::number_float(input, layout_limit);
            const auto actual_float = css::parse_number<float>(input, layout_limit);
            require(expected_float.has_value() == actual_float.has_value() &&
                        (!expected_float || *expected_float == *actual_float),
                    "float parse differs on [" + input + "]");
            const auto expected_double = legacy::number_double(input, scene_limit);
            const auto actual_double = css::parse_number<double>(input, scene_limit);
            require(expected_double.has_value() == actual_double.has_value() &&
                        (!expected_double || *expected_double == *actual_double),
                    "double parse differs on [" + input + "]");
        }
    }

    void generational_slots() {
        GenerationalSlots<int> slots(2);
        require(slots.first_free() == 0 && slots.occupy(0) == 1, "first occupancy is generation 1");
        slots[0] = 42;
        require(slots.find(0, 1) && *slots.find(0, 1) == 42 && !slots.find(0, 2) && !slots.find(5, 1), "resolve");
        require(slots.first_free() == 1, "occupied slot skipped");
        slots.vacate(0);
        require(!slots.find(0, 1) && slots.first_free() == 0, "vacated handle is stale, slot reusable");
        require(slots.occupy(0) == 2 && slots[0] == 42, "reuse bumps generation and keeps the value for the caller");
        require(slots.append() == 2 && slots.size() == 3 && slots.generation(2) == 0, "growth");

        GenerationalSlots<int, std::uint8_t> small(1);
        for (unsigned i = 1; i <= 255; ++i) {
            require(small.first_free() == 0 && small.occupy(0) == i, "generation sequence");
            small.vacate(0);
        }
        require(!small.reusable(0) && small.first_free() == small.size(), "exhausted slot is retired");
    }

    void owner_thread() {
        OwnerThread owner;
        owner.require(Diag::SceneOwner);
        std::string message;
        bool current = true;
        std::thread outsider([&] {
            current = owner.is_current();
            try {
                owner.require(Diag::WrongThread, "font owner");
            } catch (const std::logic_error& e) {
                message = e.what();
            }
        });
        outsider.join();
        require(!current && message == "WEFT_WRONG_THREAD: font owner", "owner thread violation");
    }
} // namespace

int main() {
    try {
        diagnostics();
        results();
        scalars();
        split_differential();
        number_differential();
        generational_slots();
        owner_thread();
        std::cout << "PASS base diagnostics, Result, scalar parsing differential, slots and owner thread\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
