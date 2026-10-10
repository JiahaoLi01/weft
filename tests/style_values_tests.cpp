#include "base/css_scalar.hpp"
#include "scene/scene.hpp"
#include "style/style.hpp"
#include "style/values.hpp"
#include "support/check.hpp"
#include "test_constants/style_text.hpp"

#include <cmath>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

// Typed computed values: the property and keyword registries, the payload parsed from each text, and the
// StyleValues container. The payload must agree with the string parsers it replaces wherever they accept the text.
namespace {
    using weft::testing::require;
    namespace limits = weft::test_constants::style_text;

    void registries() {
        for (std::size_t i = 0; i < weft::property_count; ++i) {
            const auto id = weft::PropertyId(i);
            const auto name = weft::property_name(id);
            require(weft::find_property(name) == id, "property name round trip");
            if (i) {
                require(weft::property_name(weft::PropertyId(i - 1)) < name, "property names sorted and unique");
            }
        }
        require(!weft::find_property("color-scheme") && !weft::find_property("") && !weft::find_property("Color"),
                "unregistered property names");
        // Every property a style profile registers has an id.
        for (const auto& profile : {weft::document_style_profile(), weft::scene_style_profile()}) {
            for (const auto& [name, property] : profile.properties) {
                require(weft::find_property(name).has_value(), "profile property " + name + " has no id");
            }
        }
        for (const auto* text : {"auto", "none", "flex-start", "preserve-3d", "currentcolor", "wrap-reverse"}) {
            const auto k = weft::find_keyword(text);
            require(k && weft::keyword_name(*k) == text, std::string("keyword ") + text);
        }
        require(weft::find_keyword("currentColor") == weft::Keyword::Currentcolor && !weft::find_keyword("Auto") &&
                    !weft::find_keyword("transparent"),
                "keyword spelling");
    }

    void parsing() {
        using Kind = weft::CssValue::Kind;
        using weft::Unit;

        struct Case {
            const char* text;
            Kind kind;
            double number{};
            Unit unit{};
        };

        for (const auto& c : std::vector<Case>{{"auto", Kind::Keyword},
                                               {"16px", Kind::Dimension, 16, Unit::Px},
                                               {"-1.5em", Kind::Dimension, -1.5, Unit::Em},
                                               {"50%", Kind::Dimension, 50, Unit::Percent},
                                               {"0", Kind::Dimension, 0, Unit::None},
                                               {".5s", Kind::Dimension, .5, Unit::S},
                                               {"250ms", Kind::Dimension, 250, Unit::Ms},
                                               {"45deg", Kind::Dimension, 45, Unit::Deg},
                                               {"1fr", Kind::Dimension, 1, Unit::Fr},
                                               {"1e2px", Kind::Dimension, 100, Unit::Px},
                                               {"calc(50% - 10px)", Kind::Calc},
                                               {"#ff8000", Kind::Color},
                                               {"#f80", Kind::Color},
                                               {"rgba(255, 0, 0, .5)", Kind::Color},
                                               {"transparent", Kind::Color},
                                               {"black", Kind::Color},
                                               {"currentColor", Kind::Keyword},
                                               // Raw: grammars without a payload, and texts the string parsers reject.
                                               {"translateX(10px)", Kind::Raw},
                                               {"1px solid red", Kind::Raw},
                                               {" 5px", Kind::Raw},
                                               {"5px ", Kind::Raw},
                                               {"5 px", Kind::Raw},
                                               {"2rem", Kind::Raw},
                                               {"5PX", Kind::Raw},
                                               {"hsl(0, 0%, 0%)", Kind::Raw},
                                               {"Block", Kind::Raw},
                                               {"inf", Kind::Raw},
                                               {"1e400px", Kind::Raw},
                                               {"calc(1px * 2)", Kind::Raw},
                                               {"", Kind::Raw}}) {
            const weft::CssValue v(c.text);
            require(v.text() == c.text, std::string("exact text ") + c.text);
            require(v.kind() == c.kind, std::string("kind of ") + c.text);
            if (c.kind == Kind::Dimension) {
                require(v.dimension()->value == c.number && v.dimension()->unit == c.unit,
                        std::string("dimension of ") + c.text);
            }
        }
        const weft::CssValue calc("calc(50% - 10px)");
        require(calc.calc()->a == weft::Dimension{50, Unit::Percent} &&
                    calc.calc()->b == weft::Dimension{10, Unit::Px} && calc.calc()->subtract,
                "calc payload");
        const weft::CssValue color("rgba(255, 0, 0, .5)");
        require(color.color()->channels == std::array<float, 4>{1, 0, 0, .5f} &&
                    color.color()->form == weft::Rgba::Form::Rgba,
                "rgba payload");
        require(weft::CssValue("#f80").color()->form == weft::Rgba::Form::ShortHex &&
                    weft::CssValue("#f80").color()->channels == weft::parse_css_color("#ff8800"),
                "short hex keeps its form");
        require(weft::CssValue("none").is(weft::Keyword::None) && !weft::CssValue("none").is(weft::Keyword::Auto),
                "keyword test");
        require(weft::CssValue("16px") == weft::CssValue("16px") &&
                    !(weft::CssValue("16px") == weft::CssValue("16.0px")),
                "values compare by text");
    }

    // Wherever split_unit + parse_number (the scene and page scalar helpers) and parse_css_color accept a text, the
    // payload holds the same number or color.
    void differential() {
        std::mt19937 random(limits::value_differential_seed);
        std::uniform_real_distribution<double> magnitude(-1e4, 1e4);
        const char* units[]{"", "px", "%", "em"};
        std::size_t compared{};
        for (int i = 0; i < limits::value_differential_numbers; ++i) {
            const double n = magnitude(random);
            char digits[64];
            std::snprintf(digits, sizeof(digits), i % 3 == 0 ? "%.0f" : i % 3 == 1 ? "%.4f" : "%.12g", n);
            const std::string text = std::string(digits) + units[i % 4];
            const weft::CssValue v(text);
            const auto [d, unit] = weft::css::split_unit(text, {.px = true, .percent = true, .em = true});
            const auto expected = weft::css::parse_number<double>(std::string(d));
            require(expected.has_value() == (v.dimension() != nullptr), "dimension acceptance of " + text);
            if (expected) {
                require(v.dimension()->value == *expected, "dimension value of " + text);
                // Float consumers narrow the double: equal to a direct float parse.
                require(float(v.dimension()->value) == *weft::css::parse_number<float>(std::string(d)),
                        "float narrowing of " + text);
                ++compared;
            }
        }
        for (int i = 0; i < limits::value_differential_colors; ++i) {
            const unsigned r = random() % 256;
            const unsigned g = random() % 256;
            const unsigned b = random() % 256;
            char text[64];
            if (i % 2) {
                std::snprintf(text, sizeof(text), "#%02x%02x%02x", r, g, b);
            } else {
                std::snprintf(text, sizeof(text), "rgba(%u, %u, %u, %.3f)", r, g, b, (random() % 1001) / 1000.);
            }
            const weft::CssValue v(text);
            require(v.color() && v.color()->channels == weft::parse_css_color(text), std::string("color ") + text);
            ++compared;
        }
        require(compared > std::size_t(limits::value_differential_min_compared), "differential coverage");
    }

    // Texts built from the pieces of the strtod grammar (signs, decimal and hexadecimal digits, points, exponents,
    // inf/nan) and an optional px suffix: the payload accepts exactly what parse_number accepts, with the same value,
    // although it refuses most of them without calling it.
    void number_grammar() {
        std::mt19937 random(limits::number_grammar_seed);
        const std::vector<std::string> pieces{"",  "+",  "-",  "0",   "1",   "9",   "42",  "007", ".",  ".5",  "e",
                                              "E", "e+", "e-", "E9",  "0x",  "0X",  "x",   "p",   "P",  "p-",  "p3",
                                              "a", "F",  "f0", "inf", "nan", "INF", "(1)", "px",  "1.", "e400"};
        std::size_t accepted{};
        std::size_t refused{};
        for (int i = 0; i < limits::number_grammar_texts; ++i) {
            std::string text;
            const unsigned count = 1 + random() % limits::number_grammar_max_pieces;
            for (unsigned j = 0; j < count; ++j) {
                text += pieces[random() % pieces.size()];
            }
            if (text.empty() || weft::find_keyword(text)) {
                continue;
            }
            const bool px = text.size() > 2 && text.ends_with("px");
            const auto digits = px ? text.substr(0, text.size() - 2) : text;
            const auto expected = weft::css::parse_number<double>(digits);
            const weft::CssValue v(text);
            require(v.kind() != weft::CssValue::Kind::Color && v.kind() != weft::CssValue::Kind::Calc,
                    "grammar text classified as color or calc: " + text);
            require(expected.has_value() == (v.dimension() != nullptr), "number grammar acceptance of " + text);
            if (expected) {
                require(v.dimension()->value == *expected &&
                            v.dimension()->unit == (px ? weft::Unit::Px : weft::Unit::None),
                        "number grammar value of " + text);
                ++accepted;
            } else {
                ++refused;
            }
        }
        require(accepted > std::size_t(limits::number_grammar_min_each) &&
                    refused > std::size_t(limits::number_grammar_min_each),
                "number grammar coverage");
    }

    void container() {
        weft::StyleValues s;
        s.set(weft::PropertyId::Width, "10px");
        s.set("color", "#123456");
        s.set(weft::PropertyId::AlignItems, "center");
        s.set(weft::PropertyId::Width, "20px");
        require(s.size() == 3 && s.text(weft::PropertyId::Width) == "20px" && s.text("color") == "#123456" &&
                    s.text("height", "auto") == "auto" && s.text("not-a-property", "x") == "x",
                "set, replace, text and fallback");
        require(weft::css::style_value(s, "align-items", "") == "center" &&
                    weft::css::style_value(s, "display", "block") == "block",
                "style_value overload");
        // Iteration follows names, as a name-keyed map would.
        std::map<std::string, std::string> reference{
            {"width", "20px"}, {"color", "#123456"}, {"align-items", "center"}};
        auto r = reference.begin();
        for (const auto& [id, value] : s) {
            require(weft::property_name(id) == r->first && value.text() == r->second, "name order iteration");
            ++r;
        }
        auto t = s;
        require(t == s && t.erase("color") && !t.erase("color") && !(t == s) && t.size() == 2, "erase and equality");
        bool rejected{};
        try {
            s.set("not-a-property", "1");
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected, "unregistered property set");
    }
} // namespace

int main() {
    try {
        registries();
        parsing();
        differential();
        number_grammar();
        container();
        std::cout << "PASS style values: " << weft::property_count
                  << " properties, keyword registry, typed payloads agree with the scalar and color parsers on 22000 "
                     "texts and with parse_number on strtod-grammar texts, StyleValues order/equality\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
