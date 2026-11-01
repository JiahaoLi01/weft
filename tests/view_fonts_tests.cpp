#define NOMINMAX
#include "style/style.hpp"
#include "support/check.hpp"
#include "support/files.hpp"
#include "support/scene_fonts.hpp"
#include "view/view.hpp"
#include "view/view_fonts.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// A page's fonts (src/view/view_fonts.hpp, docs/compatibility.md "@font-face"): of a rule's sources the first that
// loads gives its face, woff and woff2 (by format() hint, unread, or by their bytes), local(), files that are no font
// and missing files passing to the next; a rule none of whose sources loads is one diagnostic with every source's
// reason. A page's family hides an installed one of its name. Each View keeps the catalog of its own page (a page
// without @font-face the runtime's), reads its fonts again each page load, and lays its text out in them; its document
// is in no-quirks mode (KaTeX refuses to render in quirks mode).
namespace {
    using weft::testing::require;
    using Source = weft::FontFaceRule::Source;

    std::vector<std::uint8_t> katex_font(std::string_view name) {
        const auto bytes =
            weft::testing::read_file(weft::testing::source_path("third_party/katex/fonts/" + std::string(name)));
        return {bytes.begin(), bytes.end()};
    }

    weft::FontFaceRule rule(std::string family, std::vector<Source> sources, std::string range = {}) {
        weft::FontFaceRule r;
        r.family = std::move(family);
        r.sources = std::move(sources);
        r.unicode_range = std::move(range);
        return r;
    }

    struct Files {
        std::map<std::string, std::vector<std::uint8_t>> files;
        std::vector<std::string> reads;

        weft::ViewResourceReader reader() {
            return [this](std::string_view path) -> std::optional<std::vector<std::uint8_t>> {
                reads.emplace_back(path);
                const auto found = files.find(std::string(path));
                if (found == files.end()) {
                    return std::nullopt;
                }
                return found->second;
            };
        }
    };

    void sources() {
        const auto ttf = katex_font("KaTeX_Main-Regular.ttf");
        Files f;
        f.files = {{"ui/fonts/main.ttf", ttf},
                   {"ui/fonts/plain", ttf},
                   {"ui/fonts/disguised.ttf", {'w', 'O', 'F', '2', 0, 1, 0, 0}},
                   {"ui/fonts/junk.ttf", {1, 2, 3, 4, 5, 6, 7, 8}},
                   {"ui/fonts/broken.ttf", {0, 1, 0, 0, 0, 9, 0, 0, 0, 0, 0, 0}}};
        std::vector<std::string> diagnosed;
        const std::vector<weft::FontFaceRule> rules{
            rule("A", {{"main.woff2", {"woff2"}, {}}, {"main.woff", {"woff"}, {}}, {"main.ttf", {"truetype"}, {}}}),
            rule("B", {{{}, {}, "KaTeX Main"}, {"plain", {}, {}}}),
            rule("C", {{"disguised.ttf", {}, {}},
                       {"junk.ttf", {"truetype"}, {}},
                       {"broken.ttf", {}, {}},
                       {"none.ttf", {}, {}}}),
            rule("D", {{"main.ttf", {"opentype", "woff2"}, {}}}, "U+zz"),
        };
        const auto faces = weft::view_font_faces(rules, "ui/fonts/page.css", f.reader(), [&](std::string m) {
            diagnosed.push_back(std::move(m));
        });
        require(faces.size() == 3 && faces[0].family == "A" && faces[1].family == "B" && faces[2].family == "D",
                "a face for each rule a source of which loads");
        require(faces[0].source.path == "ui/fonts/main.ttf" && faces[0].source.data &&
                    faces[0].source.data->size() == ttf.size() && faces[1].source.path == "ui/fonts/plain",
                "each face from the first source that loads, its bytes in memory");
        const std::vector<std::string> read{"ui/fonts/main.ttf", "ui/fonts/plain",      "ui/fonts/disguised.ttf",
                                            "ui/fonts/junk.ttf", "ui/fonts/broken.ttf", "ui/fonts/none.ttf",
                                            "ui/fonts/main.ttf"};
        require(f.reads == read, "woff2 and woff sources by their hints and local() unread, the others read in order");
        require(diagnosed.size() == 2, "a diagnostic for the rule that loads nothing and one for its range");
        const auto& c = diagnosed[0];
        require(c.starts_with("WEFT_FONT_FACE: C: ") &&
                    c.find("url(ui/fonts/disguised.ttf): a WOFF2 file is not read") != std::string::npos &&
                    c.find("url(ui/fonts/junk.ttf): no TrueType or OpenType font") != std::string::npos &&
                    c.find("url(ui/fonts/broken.ttf): ") != std::string::npos &&
                    c.find("url(ui/fonts/none.ttf): no such file") != std::string::npos,
                "every source's reason: " + c);
        require(diagnosed[1] == "WEFT_FONT_FACE: D: unicode-range U+zz does not parse" &&
                    faces[2].unicode_range.empty(),
                "a range that does not parse: the face covers every code point");
        // local() alone: nothing is looked up by name.
        diagnosed.clear();
        require(weft::view_font_faces(
                    {rule("E", {{{}, {}, "Arial"}})}, "", f.reader(),
                    [&](std::string m) {
                        diagnosed.push_back(std::move(m));
                    }).empty() &&
                    diagnosed.size() == 1 && diagnosed[0].find("local(Arial)") != std::string::npos,
                "local() only");
    }

    void hides_installed() {
        const auto runtime = weft::testing::chrome_windows_font_catalog();
        const auto has = [](const weft::FontCatalog& c, std::string_view family) {
            return std::any_of(c.faces.begin(), c.faces.end(), [&](const weft::FontFaceEntry& e) {
                return e.family == family;
            });
        };
        require(has(runtime, "Segoe UI") && has(runtime, "Arial"), "the runtime's families");
        const auto segoe = std::count_if(runtime.faces.begin(), runtime.faces.end(), [](const weft::FontFaceEntry& e) {
            return e.family == "Segoe UI";
        });
        weft::FontFaceEntry page;
        page.family = "segoe ui";
        page.source.path = "ui/fonts/main.ttf";
        const auto catalog = weft::view_page_catalog(runtime, {page});
        require(!has(catalog, "Segoe UI") && has(catalog, "segoe ui") && has(catalog, "Arial") &&
                    std::ptrdiff_t(catalog.faces.size()) == std::ptrdiff_t(runtime.faces.size()) - segoe + 1 &&
                    catalog.generic_families == runtime.generic_families &&
                    catalog.fallback_families == runtime.fallback_families,
                "a page's family hides the installed one of its name, ASCII case-insensitively, and only it");
    }

    void in_a_view() {
        const auto runtime = std::make_shared<weft::FontCatalog>(weft::testing::chrome_windows_font_catalog());
        weft::SceneText text{*runtime};
        Files f;
        f.files = {{"ui/fonts/main.ttf", katex_font("KaTeX_Main-Regular.ttf")}};
        weft::ViewPage page;
        page.scripts.push_back({"page.js", "const t = document.createElement('span');"
                                           "t.id = 't';"
                                           "t.textContent = 'Wide words';"
                                           "document.getElementById('root').appendChild(t);"});
        page.stylesheet = "@font-face { font-family: 'Page Main'; src: url(../fonts/main.woff2) format('woff2'),"
                          " url(../fonts/main.ttf) format('truetype'); font-display: block }"
                          "#root { font-family: 'Page Main', Arial; font-size: 20px }";
        page.base = "ui/page/page.css";
        // @font-face is a forgiving sheet's (page.json "forgivingCss", as KaTeX's pages are); a strict sheet refuses
        // every at-rule.
        weft::View view{runtime, text, 320, 120, 1};
        view.set_resource_reader(f.reader());
        bool refused{};
        try {
            view.load(page);
        } catch (const std::exception& e) {
            refused = std::string(e.what()).find("at-rule") != std::string::npos;
        }
        const auto strict = view.take_diagnostics();
        require(refused && f.reads.empty() && strict.size() == 1 &&
                    strict[0].message.find("at-rule") != std::string::npos,
                "a strict sheet refuses @font-face as an at-rule");
        page.forgiving_css = true;
        view.load(page);
        view.advance(0);
        std::string listed;
        for (const auto& d : view.take_diagnostics()) {
            listed += "\n  " + d.message;
        }
        require(listed.empty(), "a page's @font-face loads without a diagnostic:" + listed);
        require(f.reads == std::vector<std::string>{"ui/fonts/main.ttf"}, "the TrueType source read");
        const auto fonts = view.fonts();
        require(fonts != runtime && std::any_of(fonts->faces.begin(), fonts->faces.end(),
                                                [](const weft::FontFaceEntry& e) {
                                                    return e.family == "Page Main";
                                                }),
                "the page's catalog, with its face");
        const auto width = [](weft::View& v) {
            const auto [ok, value] = v.evaluate("String(document.getElementById('t').getBoundingClientRect().width)");
            require(ok, "the text's width: " + value);
            return std::stod(value);
        };
        const auto [mode_ok, mode] = view.evaluate("document.compatMode");
        require(mode_ok && mode == "CSS1Compat", "the document in no-quirks mode: " + mode);

        // Another View of the same runtime, its page without the rule: the runtime's catalog, its text in Arial.
        weft::View other{runtime, text, 320, 120, 1};
        auto plain = page;
        plain.stylesheet = "#root { font-family: 'Page Main', Arial; font-size: 20px }";
        other.load(plain);
        other.advance(0);
        require(other.take_diagnostics().empty() && other.fonts() == runtime, "a page without @font-face");
        require(std::abs(width(view) - width(other)) > 1,
                "each View lays its text out in its own page's fonts: " + std::to_string(width(view)) + " and " +
                    std::to_string(width(other)));

        // A page load reads its fonts anew; a missing font is a scene diagnostic and the text falls to Arial.
        view.load(page);
        view.advance(0);
        require(f.reads.size() == 2 && view.take_diagnostics().empty(), "a page load reads its fonts again");
        auto missing = page;
        missing.stylesheet = "@font-face { font-family: 'Page Main'; src: url(none.ttf) }"
                             "#root { font-family: 'Page Main', Arial; font-size: 20px }";
        view.load(missing);
        view.advance(0);
        const auto diagnostics = view.take_diagnostics();
        require(diagnostics.size() == 1 && diagnostics[0].kind == weft::ViewDiagnostic::Kind::Scene &&
                    diagnostics[0].message == "WEFT_FONT_FACE: Page Main: url(ui/page/none.ttf): no such file" &&
                    view.fonts() == runtime && std::abs(width(view) - width(other)) < 1e-3,
                "a face that loads nothing: a diagnostic, and the runtime's fonts");
    }
} // namespace

int main() {
    try {
        sources();
        hides_installed();
        in_a_view();
        std::cout << "PASS view fonts: sources by hint and bytes, reasons, ranges, a page family over an installed "
                     "one, a catalog per View and page load, no-quirks mode\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
