#include "base/diagnostic.hpp"
#include "scene/scene.hpp"
#include "scene/scene_inline.hpp"
#include "scene/scene_text.hpp"
#include "support/check.hpp"
#include "support/d3d11_scene.hpp"
#include "support/files.hpp"
#include "support/json_value.hpp"
#include "support/native_scene.hpp"
#include "support/png.hpp"
#include "support/scene_fonts.hpp"
#include "text/case_mapping.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Scene text styles and the CSS around them (11.3c), beyond Chrome's line geometry (scene_inline_browser_contract
// covers text-transform, italics and numeric variants there): the slant of a synthetic oblique against Chrome's
// pixels on the reference raster and the native D3D11 pass, tabular digits on the lines both draw, a face's own
// italics left upright, Unicode's case mappings, viewport lengths following the viewport, overflow axes,
// -webkit-user-select, and the diagnostics.
namespace {
    using weft::testing::require;

    weft::dom::Node& text(weft::testing::NativeScene& s, weft::dom::Node& parent, std::string data) {
        auto& t = s.tree.create(weft::constants::text_node_type, "#text", "", s.document->identity);
        t.data = std::move(data);
        s.tree.insert(parent, t, nullptr);
        return t;
    }

    weft::dom::Node& element(weft::testing::NativeScene& s, weft::dom::Node& parent, const std::string& style) {
        auto& e = s.html("div");
        s.set(e, "style", style);
        s.tree.insert(parent, e, nullptr);
        return e;
    }

    // The slant of the lower text against the upper one, the same words at the same x: each row's ink centroid in
    // the lower band less the one in the upper band, regressed on the row (px of shift per row down; an oblique leaning
    // right shifts its top right, so the slope is negative). channel reads one byte of a 4-byte pixel.
    double slant(const std::uint8_t* pixels, unsigned width, unsigned upper, unsigned lower, unsigned band) {
        const auto centroid = [&](unsigned y) -> std::optional<double> {
            double sum{};
            double weight{};
            for (unsigned x = 0; x < width; ++x) {
                const double v = pixels[(std::size_t(y) * width + x) * 4 + 1];
                sum += v * x;
                weight += v;
            }
            return weight > 0 ? std::optional<double>(sum / weight) : std::nullopt;
        };
        std::vector<std::pair<double, double>> rows;
        for (unsigned r = 0; r < band; ++r) {
            const auto a = centroid(upper + r);
            const auto b = centroid(lower + r);
            if (a && b) {
                rows.emplace_back(r, *b - *a);
            }
        }
        require(rows.size() > 10, "ink in both bands");
        double mx{};
        double my{};
        for (const auto& [x, y] : rows) {
            mx += x;
            my += y;
        }
        mx /= double(rows.size());
        my /= double(rows.size());
        double covariance{};
        double variance{};
        for (const auto& [x, y] : rows) {
            covariance += (x - mx) * (y - my);
            variance += (x - mx) * (x - mx);
        }
        return covariance / variance;
    }

    template <class F>
    std::pair<weft::Diag, std::string> diagnostic(F&& f) {
        try {
            f();
        } catch (const weft::DiagnosticError& e) {
            return {e.code(), e.what()};
        }
        return {weft::Diag{}, {}};
    }
} // namespace

int main() {
    try {
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM init");
        auto& scene_text = weft::testing::thread_scene_text();

        // Unicode's full case mappings, beyond what Chrome's geometry can show: Final_Sigma (σ inside a word, ς at
        // its end, also before case-ignorable characters), one character becoming several, and bytes that are not
        // UTF-8 kept.
        using weft::TextCase;
        require(weft::case_mapped("ΟΔΟΣ ΣΟΦΟΣ'. Σ", TextCase::Lower) == "οδος σοφος'. σ",
                "lowercase sigma: final at a word's end, not alone");
        require(weft::case_mapped("straße ﬁnale ǆ", TextCase::Upper) == "STRASSE FINALE Ǆ",
                "uppercase: ß and the ﬁ ligature become two letters");
        require(weft::case_mapped("İ", TextCase::Lower) == "i\xCC\x87", "lowercase İ: i and a combining dot");
        require(weft::case_mapped("a\xFF"
                                  "b\xC3",
                                  TextCase::Upper) == "A\xFF"
                                                      "B\xC3",
                "bytes that are not UTF-8");

        // A face's own italics are not slanted again; a face without them is (Impact).
        {
            weft::SceneStyle italic{{"font-family", "Segoe UI"}, {"font-size", "16px"}, {"font-style", "italic"}};
            weft::SceneStyle impact{{"font-family", "Impact"}, {"font-size", "16px"}, {"font-style", "italic"}};
            const auto own = scene_text.shape("Words", italic);
            const auto slanted = scene_text.shape("Words", impact);
            require(std::none_of(own->glyphs.begin(), own->glyphs.end(),
                                 [](const weft::SceneGlyph& g) {
                                     return g.oblique;
                                 }) &&
                        std::all_of(slanted->glyphs.begin(), slanted->glyphs.end(),
                                    [](const weft::SceneGlyph& g) {
                                        return g.oblique;
                                    }),
                    "italics from the face where it has them, a synthetic oblique where it has none");
        }

        // The slant against Chrome's: the same words upright and in italics, one band above the other.
        {
            const auto directory = weft::testing::source_path("tests/fixtures/scene-oblique-reference");
            const auto fixture = weft::testing::parse_json(weft::testing::read_file(directory / "cases.json"));
            const auto chrome = weft::testing::read_png(directory / "oblique.png");
            const unsigned width = unsigned(fixture["width"].number);
            const unsigned height = unsigned(fixture["height"].number);
            const unsigned band = unsigned(fixture["lines"][1]["y"].number);
            weft::testing::NativeScene s;
            s.set(*s.root, "style", "position:relative;overflow:hidden;background-color:#000000");
            for (const auto& line : fixture["lines"].array) {
                auto& l =
                    element(s, *s.root,
                            "position:absolute;white-space:nowrap;left:" + std::to_string(int(fixture["left"].number)) +
                                "px;top:" + std::to_string(int(line["y"].number)) + "px;" + line["style"].string);
                text(s, l, fixture["text"].string);
            }
            weft::SceneSolver solver("", "", scene_text);
            const auto frame = solver.frame(s.tree.snapshot(*s.root), float(width), float(height), 0);
            weft::SceneRaster raster(scene_text);
            const auto reference = raster.raster(frame, 1);
            weft::testing::D3D11Scene d3d11(width, height);
            const auto native = d3d11.draw(frame);
            d3d11.gpu.check_debug_messages();
            const double expected = slant(chrome.rgba.data(), width, 0, band, band - 10);
            const double drawn = slant(reference.rgba.data(), width, 0, band, band - 10);
            const double gpu = slant(native.data(), width, 0, band, band - 10);
            std::cout << "synthetic oblique slant: Chrome " << expected << ", reference raster " << drawn << ", native "
                      << gpu << " px per row\n";
            require(std::abs(drawn - expected) < .02 && std::abs(gpu - expected) < .02,
                    "a synthetic oblique slants as Chrome's");
        }

        // Numeric variants on the lines the reference raster and the native pass draw (scene_text_line): Segoe UI
        // Light's digits are proportional unless tabular-nums asks otherwise, and its default 4 is wider than its
        // tabular one. Each line measures as wide as layout made it (the native pass refuses text wider than its box),
        // and a 4 after a 1 ends where a 4 after a 4 does.
        {
            const weft::SceneStyle proportional{{"font-family", "Segoe UI Light"}, {"font-size", "28px"}};
            auto tabular = proportional;
            tabular[weft::PropertyId::FontVariantNumeric] = "tabular-nums";
            const float four = scene_text.measure("4", proportional).width;
            const float one = scene_text.measure("1", proportional).width;
            const float tabular_four = scene_text.measure("4", tabular).width;
            require(four - tabular_four > .25f && four - one > 1 &&
                        scene_text.measure("1", tabular).width == tabular_four,
                    "Segoe UI Light: a default 4 wider than a tabular 4 and than a default 1, tabular 1 and 4 alike");
            constexpr unsigned width = 64;
            constexpr unsigned height = 136;
            const std::vector<std::string> texts{"4", "14", "44"};
            weft::testing::NativeScene s;
            s.set(*s.root, "style", "position:relative;overflow:hidden;background-color:#000000");
            for (std::size_t i = 0; i < texts.size(); ++i) {
                auto& l = element(s, *s.root,
                                  "position:absolute;left:4px;top:" + std::to_string(4 + 44 * i) +
                                      "px;white-space:nowrap;color:#ffffff;font-family:Segoe UI Light;font-size:28px;"
                                      "font-variant-numeric:tabular-nums");
                text(s, l, texts[i]);
            }
            weft::SceneSolver solver("", "", scene_text);
            const auto frame = solver.frame(s.tree.snapshot(*s.root), float(width), float(height), 0);
            std::string widths;
            bool fits = true;
            for (std::size_t i = 0; i < texts.size(); ++i) {
                const auto& n = frame.root.children.at(i);
                require(n.lines.size() == 1, "one line of " + texts[i]);
                const auto line = weft::scene_text_line(n, 0);
                const float measured = scene_text.measure(line.text, line.style).width;
                widths += ' ' + texts[i] + ' ' + std::to_string(measured) + '/' + std::to_string(n.lines[0].rect[2]);
                fits = fits && measured == n.lines[0].rect[2];
            }
            weft::SceneRaster raster(scene_text);
            const auto reference = raster.raster(frame, 1);
            weft::testing::D3D11Scene d3d11(width, height);
            std::vector<std::uint8_t> native;
            std::string refused;
            try {
                native = d3d11.draw(frame);
            } catch (const std::exception& e) {
                refused = e.what();
            }
            // The last column with ink in the rows of line i; -1 for none.
            const auto ink_end = [&](const std::vector<std::uint8_t>& pixels, std::size_t i) {
                const auto& box = frame.root.children.at(i).box;
                int last = -1;
                for (auto y = unsigned(box[1]); !pixels.empty() && y < unsigned(box[1] + box[3]) && y < height; ++y) {
                    for (unsigned x = 0; x < width; ++x) {
                        if (pixels[(std::size_t(y) * width + x) * 4 + 1] > 0) {
                            last = std::max(last, int(x));
                        }
                    }
                }
                return last;
            };
            std::cout << "Segoe UI Light 28px: 4 " << four << " px, tabular " << tabular_four << ", 1 " << one
                      << "; measured/laid out" << widths << "; a 4 ends at column " << ink_end(reference.rgba, 1)
                      << " after a 1 and " << ink_end(reference.rgba, 2) << " after a 4 on the reference raster, "
                      << (refused.empty() ? std::to_string(ink_end(native, 1)) + " and " +
                                                std::to_string(ink_end(native, 2)) + " on the native pass"
                                          : refused)
                      << '\n';
            require(fits, "tabular digits measure as wide as layout made their lines");
            require(refused.empty(), "the native pass draws tabular digits: " + refused);
            d3d11.gpu.check_debug_messages();
            require(
                ink_end(reference.rgba, 1) > 0 && ink_end(reference.rgba, 1) == ink_end(reference.rgba, 2) &&
                    ink_end(native, 1) > 0 && ink_end(native, 1) == ink_end(native, 2),
                "tabular digits: a 4 after a 1 ends where a 4 after a 4 does, on the reference raster and the native "
                "pass");
        }

        // Viewport lengths: px of the frame's viewport, again when only the viewport changes.
        {
            weft::testing::NativeScene s;
            s.set(*s.root, "style", "position:relative");
            auto& sized = element(s, *s.root, "width:50vw;height:10vh;max-height:90vh;margin-left:5vmin");
            auto& tall = element(s, *s.root, "width:10px;height:200vh;max-height:90dvh");
            weft::SceneSolver solver("", "", scene_text);
            weft::SceneFrame frame;
            solver.update(s.tree.snapshot(*s.root), frame, 400, 300, 0);
            const auto& a = frame.root.children.at(0);
            const auto& b = frame.root.children.at(1);
            std::cout << "viewport 400x300: " << a.box[2] << 'x' << a.box[3] << " at " << a.box[0] << ", " << b.box[3]
                      << '\n';
            require(a.box[2] == 200 && a.box[3] == 30 && a.box[0] == 15 && b.box[3] == 270,
                    "vw, vh, vmin and dvh of a 400 x 300 viewport");
            solver.update(s.tree.snapshot(*s.root), frame, 800, 500, 16);
            const auto& c = frame.root.children.at(0);
            require(c.box[2] == 400 && c.box[3] == 50 && c.box[0] == 25 && frame.root.children.at(1).box[3] == 450,
                    "the same page in an 800 x 500 viewport: lengths follow the viewport");
            (void)sized;
            (void)tall;
            // Keyframes are read once; a viewport length there would not follow the viewport.
            weft::testing::NativeScene k;
            element(k, *k.root, "width:10px;height:10px;animation:grow 1s infinite");
            const auto [code, message] = diagnostic([&] {
                weft::SceneSolver refusing("", "@keyframes grow{from{height:10vh}to{height:20vh}}", scene_text);
                refusing.frame(k.tree.snapshot(*k.root), 100, 100, 0);
            });
            require(message.find("viewport length in keyframes") != std::string::npos,
                    "diagnostic: viewport lengths in keyframes (got " + message + ")");
        }

        // Overflow axes: one axis that clips makes the box clip (visible beside it is auto); clip and scroll stay as
        // written, another value is a diagnostic. -webkit-user-select is accepted and does nothing.
        {
            const auto overflow_of = [&](const std::string& style, const std::string& child) {
                weft::testing::NativeScene s;
                auto& box = element(s, *s.root, "width:100px;height:40px;-webkit-user-select:none;" + style);
                element(s, box, child);
                weft::SceneSolver solver("", "", scene_text);
                const auto frame = solver.frame(s.tree.snapshot(*s.root), 200, 100, 0);
                return std::string(frame.root.children.at(0).style.text(weft::PropertyId::Overflow, "visible"));
            };
            require(overflow_of("overflow-y:auto", "height:20px") == "auto" &&
                        overflow_of("overflow-x:hidden;overflow-y:hidden", "height:80px") == "hidden" &&
                        overflow_of("overflow-x:hidden", "height:20px") == "auto" &&
                        overflow_of("overflow:hidden;overflow-y:visible", "height:20px") == "auto" &&
                        overflow_of("overflow-y:visible;overflow:hidden", "height:80px") == "hidden" &&
                        overflow_of("overflow:hidden auto", "height:20px") == "auto",
                    "overflow axes fold into one value");
            const auto refused = [&](const std::string& style, const std::string& child, const std::string& expected) {
                const auto [code, message] = diagnostic([&] {
                    overflow_of(style, child);
                });
                require(message.find(expected) != std::string::npos,
                        "diagnostic: " + expected + " (got " + message + ")");
            };
            require(overflow_of("overflow-y:auto", "height:80px") == "auto" &&
                        overflow_of("overflow-x:clip", "height:20px") == "clip" &&
                        overflow_of("overflow:scroll", "height:80px") == "scroll",
                    "an overflowing auto box, clip and scroll");
            refused("overflow-y:overlay", "height:20px", "overflow:overlay");
        }

        // The background shorthand cascades as its color and image: an inline background-color beats a sheet's
        // background (the CRPG page's body over the gallery's backdrop), a later shorthand beats an earlier longhand,
        // and which part a var() gives is known once it is substituted.
        {
            const auto background = [&](const std::string& sheet, const std::string& inline_style) {
                weft::testing::NativeScene s;
                auto& box = s.html("div");
                s.set(box, "class", "page");
                if (!inline_style.empty()) {
                    s.set(box, "style", inline_style);
                }
                s.tree.insert(*s.root, box, nullptr);
                weft::SceneSolver solver(sheet, "", scene_text);
                const auto frame = solver.frame(s.tree.snapshot(*s.root), 100, 100, 0);
                const auto& style = frame.root.children.at(0).style;
                return std::string(style.text(weft::PropertyId::BackgroundColor, "")) + " / " +
                       std::string(style.text(weft::PropertyId::BackgroundImage, ""));
            };
            const auto cases = std::vector<std::pair<std::string, std::string>>{
                {background(".page{background:transparent}", "background-color:#050403"), "#050403 / none"},
                {background(".page{background-color:#ff0000;background:#00ff00}", ""), "#00ff00 / none"},
                {background(".page{background:#00ff00;background-color:#ff0000}", ""), "#ff0000 / none"},
                {background(".page{--g:linear-gradient(red, blue);background:var(--g)}", ""),
                 "transparent / linear-gradient(red, blue)"},
                {background(".page{--c:#123456;background:var(--c)}", ""), "#123456 / none"},
                {background(".page{background:none}", ""), "transparent / none"}};
            for (const auto& [got, expected] : cases) {
                require(got == expected, "background cascade: " + got + ", expected " + expected);
            }
        }

        // Outside the profile: text-transform beyond upper and lower case, impossible numeric variants.
        {
            const auto refused = [&](const std::string& style, const std::string& expected) {
                weft::testing::NativeScene s;
                auto& box = element(s, *s.root, style);
                text(s, box, "Some text 123");
                weft::SceneSolver solver("", "", scene_text);
                const auto [code, message] = diagnostic([&] {
                    solver.frame(s.tree.snapshot(*s.root), 200, 100, 0);
                });
                require(message.find(expected) != std::string::npos,
                        "diagnostic: " + expected + " (got " + message + ")");
            };
            refused("text-transform:capitalize", "text-transform capitalize outside scene profile");
            refused("text-transform:full-width", "text-transform full-width outside scene profile");
            refused("font-variant-numeric:lining-nums oldstyle-nums", "font-variant-numeric");
            refused("font-variant-numeric:tabular-nums sparkly", "font-variant-numeric");
        }
        std::cout << "PASS scene text styles: synthetic oblique, tabular digits, case mappings, viewport lengths, "
                     "overflow axes, diagnostics\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
