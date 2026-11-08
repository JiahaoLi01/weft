#include "support/check.hpp"
#include "support/files.hpp"
#include "support/json_value.hpp"
#include "view/view.hpp"

#include <iostream>
#include <string>
#include <vector>

// Hits through CSS clip-path against Chrome: tests/fixtures/clip_path/page.js (boxes clipped by every shape the scene
// takes, tools/browser_clip_path_reference.mjs) in a View, a press at each of the points cases.json lists, and the
// element each reaches (the page logs mousedown targets) against what Chrome's presses reached (hits.json). The drawing
// of the same page is tests/clip_path_metal_tests.mm's. Portable.
namespace {
    using weft::testing::require;
} // namespace

int main() {
    try {
        const auto directory = weft::testing::source_path("tests/fixtures/clip_path");
        const auto manifest = weft::testing::parse_json(weft::testing::read_file(directory / "cases.json"));
        const auto chrome = weft::testing::parse_json(weft::testing::read_file(directory / "hits.json"));
        const auto width = float(manifest["width"].number);
        const auto height = float(manifest["height"].number);
        const auto fonts = std::make_shared<weft::FontCatalog>();
        weft::SceneText text{*fonts};
        weft::View view{fonts, text, width, height, 1};
        weft::ViewPage page;
        page.scripts.push_back({"page.js", weft::testing::read_text_file(directory / "page.js")});
        page.stylesheet = "#root{position:relative}";
        page.forgiving_css = true;
        page.web_platform = true;
        view.load(page);
        double time = 16;
        view.advance(time);
        const auto diagnostics = view.take_diagnostics();
        require(diagnostics.empty(), diagnostics.empty() ? std::string() : diagnostics.front().message);
        const auto& points = manifest["points"].array;
        for (const auto& p : points) {
            weft::PointerInput down;
            down.kind = weft::PointerKind::Down;
            down.x = float(p[1].number);
            down.y = float(p[2].number);
            down.buttons = 1;
            view.pointer(down);
            auto up = down;
            up.kind = weft::PointerKind::Up;
            up.buttons = 0;
            view.pointer(up);
            time += 16;
            view.advance(time);
        }
        const auto [ok, log] = view.evaluate("JSON.stringify(window.__hits)");
        require(ok, "the hits read: " + log);
        const auto native = weft::testing::parse_json(log);
        const auto& want = chrome["hits"].array;
        require(native.array.size() == want.size(),
                std::to_string(native.array.size()) + " presses logged of " + std::to_string(want.size()));
        const auto& cases = manifest["cases"].array;
        std::vector<int> wrong(cases.size());
        std::size_t failures{};
        for (std::size_t i = 0; i < want.size(); ++i) {
            if (native.array[i].string != want[i].string) {
                const auto c = std::size_t(points[i][0].number);
                if (wrong[c]++ < 3) {
                    std::cout << "  " << cases[c]["name"].string << " at (" << points[i][1].number << ", "
                              << points[i][2].number << "): weft " << native.array[i].string << ", Chrome "
                              << want[i].string << '\n';
                }
                ++failures;
            }
        }
        for (std::size_t c = 0; c < cases.size(); ++c) {
            if (wrong[c]) {
                std::cout << "FAIL " << cases[c]["name"].string << ": " << wrong[c] << " presses\n";
            }
        }
        std::cout << want.size() << " presses over " << cases.size() << " cases vs " << chrome["browser"].string
                  << ": " << failures << " reach another element\n";
        require(failures == 0, "clip-path hits match Chrome");

        // A transition between two insets passes through the insets between (CSS Shapes 1 §4.2.1: each length
        // interpolates), the hits with it: inset(0) to inset(50px) over 1 s, linear, is inset(25px) halfway. Past the
        // end the shape has no area and takes no hits. Not a Chrome recording: the spec's arithmetic.
        // The element a press reaches.
        const auto press = [&](float x, float y) {
            weft::PointerInput down;
            down.kind = weft::PointerKind::Down;
            down.x = x;
            down.y = y;
            down.buttons = 1;
            view.pointer(down);
            auto up = down;
            up.kind = weft::PointerKind::Up;
            up.buttons = 0;
            view.pointer(up);
            const auto [ok, hit] = view.evaluate("window.__hits[window.__hits.length - 1]");
            require(ok, hit);
            return hit;
        };
        const float left = width - 160;
        const float top = height - 160;
        const auto [made, why] = view.evaluate(
            "const tween = document.createElement('div'); tween.id = 'tween'; tween.setAttribute('style', "
            "'position:absolute;left:" +
            std::to_string(int(left)) + "px;top:" + std::to_string(int(top)) +
            "px;width:100px;height:100px;background:#000;clip-path:inset(0px);transition:clip-path 1000ms linear'); "
            "document.getElementById('root').appendChild(tween);");
        require(made, why);
        time += 16;
        view.advance(time);
        require(press(left + 10.5f, top + 10.5f) == "tween", "inset(0) takes a press near the corner");
        const auto [changed, change] =
            view.evaluate("document.getElementById('tween').style.clipPath = 'inset(50px)'");
        require(changed, change);
        time += 16;
        view.advance(time);
        time += 500;
        view.advance(time);
        const auto inside = press(left + 30.5f, top + 30.5f);
        const auto outside = press(left + 20.5f, top + 20.5f);
        std::cout << "halfway: (30, 30) reaches " << inside << ", (20, 20) reaches " << outside << '\n';
        require(inside == "tween" && outside != "tween", "halfway the inset is about 25px");
        time += 1000;
        view.advance(time);
        require(press(left + 50.5f, top + 50.5f) != "tween", "inset(50px) of a 100px box takes no press");
        std::cout << "PASS clip-path hits against Chrome\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
