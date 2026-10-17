#include "base/css_scalar.hpp"
#include "scene/scene_transform.hpp"
#include "support/check.hpp"
#include "test_constants/style_text.hpp"

#include <cmath>
#include <iostream>
#include <numbers>
#include <random>
#include <regex>
#include <string>
#include <vector>

// The hand-written grammars that replaced std::regex in the scene and canvas paths, against those exact regexes on
// random texts built from each grammar's pieces (valid, truncated and mixed forms).
namespace {
    using weft::testing::require;
    namespace limits = weft::test_constants::style_text;

    std::string random_text(std::mt19937& random, const std::vector<std::string>& pieces, int max_pieces) {
        std::string out;
        const auto count = random() % unsigned(max_pieces + 1);
        for (unsigned i = 0; i < count; ++i) {
            out += pieces[random() % pieces.size()];
        }
        return out;
    }

    // Interpolation splits values at these numbers.
    std::size_t numbers() {
        static const std::regex pattern(R"([-+]?(?:\d*\.\d+|\d+\.?\d*)(?:[eE][-+]?\d+)?)");
        std::mt19937 random(limits::number_span_seed);
        const std::vector<std::string> pieces{"0", "1",  "25",  "3.5", ".",     ".5",        "-",      "+",     "e",
                                              "E", "e-", "e+3", "px",  "%",     "deg",       " ",      ",",     "(",
                                              ")", "1.", "a",   "x",   "rgba(", "translate", "rotate", "scale", "E2"};
        std::size_t spans{};
        for (int i = 0; i < limits::number_span_texts; ++i) {
            const auto text = random_text(random, pieces, limits::number_span_max_pieces);
            std::vector<weft::css::NumberSpan> expected;
            for (std::sregex_iterator m(text.begin(), text.end(), pattern), end; m != end; ++m) {
                expected.push_back({std::size_t(m->position()), std::size_t(m->length())});
            }
            const auto actual = weft::css::number_spans(text);
            require(actual.size() == expected.size(), "number span count of '" + text + "'");
            for (std::size_t k = 0; k < actual.size(); ++k) {
                require(actual[k].offset == expected[k].offset && actual[k].length == expected[k].length,
                        "number span of '" + text + "'");
            }
            spans += actual.size();
        }
        require(spans > std::size_t(limits::number_span_min_spans), "number span coverage");
        return spans;
    }

    // Mixed text whitespace and keyframe comments.
    void replacements() {
        static const std::regex whitespace("[\\t\\r\\n ]+");
        static const std::regex comment(R"(/\*[\s\S]*?\*/)");
        std::mt19937 random(limits::replacement_seed);
        const std::vector<std::string> pieces{" ", "\t",  "\r\n", "\n", "a",  "bc", "/*", "*/",      "*",
                                              "/", "/*/", "x*/",  "\f", "\v", "{",  "}",  "\xc3\xa9"};
        for (int i = 0; i < limits::replacement_texts; ++i) {
            const auto text = random_text(random, pieces, limits::replacement_max_pieces);
            require(weft::css::collapse_spaces(text) == std::regex_replace(text, whitespace, " "),
                    "collapsed spaces of '" + text + "'");
            require(weft::css::strip_block_comments(text) == std::regex_replace(text, comment, ""),
                    "stripped comments of '" + text + "'");
        }
    }

    // Canvas color components.
    void decimals() {
        static const std::regex number("[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:e[+-]?[0-9]+)?");
        std::mt19937 random(limits::decimal_seed);
        const std::vector<std::string> pieces{"0", "12", ".", ".5", "+", "-", "e", "E", "e+", "e-7", "x", " ", "%"};
        std::size_t accepted{};
        for (int i = 0; i < limits::decimal_texts; ++i) {
            const auto text = random_text(random, pieces, limits::decimal_max_pieces);
            const bool expected = std::regex_match(text, number);
            require(weft::css::decimal_syntax(text) == expected, "decimal syntax of '" + text + "'");
            accepted += expected;
        }
        require(accepted > std::size_t(limits::decimal_min_accepted), "decimal coverage");
    }

    // The scene's one projective transform. Every text the regex of the former perspective matcher accepts, with
    // valid numbers, parses through the one transform grammar to the same plane; that grammar also takes other CSS
    // spellings (no space between functions, exponents), which the old matcher refused.
    void perspectives() {
        static const std::regex grammar(R"(perspective\(\s*([\d.]+)px\s*\)\s+rotateY\(\s*([-\d.]+)(deg)?\s*\))");
        std::mt19937 random(limits::perspective_seed);
        const std::vector<std::string> pieces{"perspective(",
                                              "rotateY(",
                                              "px",
                                              "deg",
                                              ")",
                                              " ",
                                              "\t",
                                              "1",
                                              "2.5",
                                              ".",
                                              "-",
                                              "-30",
                                              "x",
                                              "(",
                                              "perspective(100px) rotateY(30deg)",
                                              "perspective(80px)",
                                              " rotateY(-12.5)",
                                              "translate(1px)"};
        std::size_t matched{};
        for (int i = 0; i < limits::perspective_texts; ++i) {
            const auto text = random_text(random, pieces, limits::perspective_max_pieces);
            std::smatch m;
            if (!std::regex_match(text, m, grammar)) {
                continue;
            }
            const auto distance = weft::css::parse_number<double>(m[1].str());
            const auto angle = weft::css::parse_number<double>(m[2].str());
            const auto operations = weft::parse_scene_transform(text);
            if (!distance || !angle) {
                require(!operations, "transform grammar accepted invalid numbers in '" + text + "'");
                continue;
            }
            require(bool(operations), "transform grammar refused '" + text + "'");
            if (*distance <= 0) {
                bool refused{};
                try {
                    (void)weft::scene_perspective(*operations);
                } catch (const weft::DiagnosticError&) {
                    refused = true;
                }
                require(refused, "perspective distance accepted in '" + text + "'");
                continue;
            }
            const auto plane = weft::scene_perspective(*operations);
            const double radians = m[3].matched ? *angle * std::numbers::pi / 180 : *angle;
            require(plane && plane->distance == *distance &&
                        std::abs(plane->radians - radians) <= limits::perspective_radians_tolerance,
                    "perspective plane of '" + text + "'");
            ++matched;
        }
        require(matched > std::size_t(limits::perspective_min_matched), "perspective coverage");
    }
} // namespace

int main() {
    try {
        const auto spans = numbers();
        replacements();
        decimals();
        perspectives();
        std::cout << "PASS regex-free grammars equal their regexes: " << spans
                  << " number spans, whitespace and comment replacement, decimal and perspective plane\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
