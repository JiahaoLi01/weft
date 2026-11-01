#define NOMINMAX
#include "support/check.hpp"
#include "support/scene_fonts.hpp"
#include "view/view.hpp"

#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// What a forgiving page's CSS the scene takes without a word, and what it draws without with a note (docs/
// compatibility.md, "CSS the scene drops"), as KaTeX's stylesheet needs them: declarations of other engines' prefixes
// (-ms-, -moz-, -o-) drop silently, as a browser drops what it does not parse; counter-reset, counter-increment and
// counter-set change nothing; clip-path other than none, a stroke-miterlimit other than 4 and counter() in content are
// each a scene diagnostic once, the element drawn without them. A strict page refuses clip-path and stroke-miterlimit
// (and every pseudo-element, so counter() in content never reaches it).
namespace {
    using weft::testing::require;

    std::vector<std::string> messages(weft::View& view) {
        std::vector<std::string> out;
        for (const auto& d : view.take_diagnostics()) {
            require(d.kind == weft::ViewDiagnostic::Kind::Scene, "a scene diagnostic: " + d.message);
            out.push_back(d.message);
        }
        return out;
    }

    std::string listed(const std::vector<std::string>& all) {
        std::string out;
        for (const auto& m : all) {
            out += "\n  " + m;
        }
        return out;
    }

    weft::ViewPage page(std::string stylesheet, bool forgiving) {
        weft::ViewPage p;
        p.scripts.push_back({"page.js", "for (const name of ['a', 'b', 'c', 'd']) {"
                                        "  const e = document.createElement('div');"
                                        "  e.className = name;"
                                        "  e.textContent = name;"
                                        "  document.getElementById('root').appendChild(e);"
                                        "}"});
        p.stylesheet = std::move(stylesheet);
        p.forgiving_css = forgiving;
        return p;
    }

    void forgiving() {
        const auto fonts = std::make_shared<weft::FontCatalog>(weft::testing::chrome_windows_font_catalog());
        weft::SceneText text{*fonts};
        weft::View view{fonts, text, 320, 200, 1};
        view.load(page("#root { counter-reset: n; font-size: 16px }"
                       ".a { -ms-filter: progid:DXImageTransform.Microsoft.Alpha(Opacity=50);"
                       " -moz-osx-font-smoothing: grayscale; -o-transition: none; counter-increment: n;"
                       " counter-set: n 2; height: 20px }",
                       true));
        view.advance(0);
        const auto quiet = messages(view);
        require(quiet.empty(), "other engines' prefixes and counter properties drop without a word:" + listed(quiet));

        view.load(page(".b { clip-path: inset(1px); height: 20px }"
                       ".c { stroke-miterlimit: 10; height: 20px }"
                       ".d::before { content: '(' counter(n) ')' }",
                       true));
        view.advance(0);
        view.advance(16);
        const auto noted = messages(view);
        const auto says = [&](const std::string& part) {
            return std::count_if(noted.begin(), noted.end(), [&](const std::string& m) {
                return m.find(part) != std::string::npos;
            });
        };
        require(noted.size() == 3 && says("clip-path inset(1px) outside scene profile (drawn unclipped)") == 1 &&
                    says("stroke-miterlimit 10 outside scene profile (the miter limit is 4)") == 1 &&
                    says("counters give nothing") == 1,
                "each a scene diagnostic, once:" + listed(noted));
        // The page still draws: its elements are laid out.
        const auto [ok, height] = view.evaluate("String(document.querySelector('.b').getBoundingClientRect().height + "
                                                "document.querySelector('.c').getBoundingClientRect().height)");
        require(ok && height == "40", "the elements drawn without them: " + height);
    }

    void strict() {
        const auto fonts = std::make_shared<weft::FontCatalog>(weft::testing::chrome_windows_font_catalog());
        weft::SceneText text{*fonts};
        const std::vector<std::pair<std::string, std::string>> cases{
            {".b { clip-path: inset(1px) }", "clip-path inset(1px) outside scene profile"},
            {".c { stroke-miterlimit: 10 }", "stroke-miterlimit 10 outside scene profile"}};
        for (const auto& [sheet, reason] : cases) {
            weft::View view{fonts, text, 320, 200, 1};
            std::string said;
            try {
                view.load(page(sheet, false));
                view.advance(0);
            } catch (const std::exception& e) {
                said = e.what();
            }
            for (const auto& d : view.take_diagnostics()) {
                said += "\n  " + d.message;
            }
            require(said.find(reason) != std::string::npos, "a strict page refuses " + sheet + ":" + said);
        }
    }
} // namespace

int main() {
    try {
        forgiving();
        strict();
        std::cout << "PASS view CSS profile: other engines' prefixes and counter properties dropped silently; "
                     "clip-path, stroke-miterlimit and counter() noted once and drawn without; a strict page refuses "
                     "clip-path and stroke-miterlimit\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
