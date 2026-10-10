#include "base/css_scalar.hpp"
#include "scene/scene_grammar.hpp"
#include "support/check.hpp"
#include "test_constants/style_text.hpp"

#include <iostream>
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
        std::size_t found{};
        for (int i = 0; i < limits::perspective_texts; ++i) {
            const auto text = random_text(random, pieces, limits::perspective_max_pieces);
            std::smatch m;
            const bool whole = std::regex_match(text, m, grammar);
            const auto p = weft::scene_grammar::perspective(text);
            require(whole == p.has_value(), "perspective match of '" + text + "'");
            if (whole) {
                require(p->distance == m[1].str() && p->angle == m[2].str() && p->degrees == m[3].matched,
                        "perspective captures of '" + text + "'");
                ++matched;
            }
            const bool any = std::regex_search(text, m, grammar);
            const auto q = weft::scene_grammar::find_perspective(text);
            require(any == q.has_value(), "perspective search of '" + text + "'");
            if (any) {
                require(q->distance == m[1].str() && q->angle == m[2].str() && q->degrees == m[3].matched,
                        "perspective search captures of '" + text + "'");
                ++found;
            }
        }
        require(matched > std::size_t(limits::perspective_min_matched) && found > matched, "perspective coverage");
    }

    void grid_repeats() {
        static const std::regex repeat(R"(repeat\(\s*([1-8])\s*,\s*minmax\(\s*0\s*,\s*1fr\s*\)\s*\))");
        std::mt19937 random(limits::grid_repeat_seed);
        const std::vector<std::string> pieces{"repeat(",
                                              "minmax(",
                                              "0",
                                              "1fr",
                                              ",",
                                              ")",
                                              " ",
                                              "\n",
                                              "3",
                                              "9",
                                              "8",
                                              "repeat(4, minmax(0, 1fr))",
                                              "repeat(2,minmax(0,1fr))",
                                              "x"};
        std::size_t matched{};
        for (int i = 0; i < limits::grid_repeat_texts; ++i) {
            const auto text = random_text(random, pieces, limits::grid_repeat_max_pieces);
            std::smatch m;
            const bool expected = std::regex_match(text, m, repeat);
            const auto actual = weft::scene_grammar::grid_repeat(text);
            require(expected == actual.has_value(), "grid repeat of '" + text + "'");
            if (expected) {
                require(*actual == unsigned(std::stoul(m[1].str())), "grid repeat count of '" + text + "'");
                ++matched;
            }
        }
        require(matched > std::size_t(limits::grid_repeat_min_matched), "grid repeat coverage");
    }
} // namespace

int main() {
    try {
        const auto spans = numbers();
        replacements();
        decimals();
        perspectives();
        grid_repeats();
        std::cout << "PASS regex-free grammars equal their regexes: " << spans
                  << " number spans, whitespace and comment replacement, decimal, perspective match/search, grid "
                     "repeat\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
