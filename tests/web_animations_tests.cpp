#include "support/check.hpp"
#include "support/files.hpp"
#include "support/json_value.hpp"
#include "view/view.hpp"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// Web Animations against Chrome: tests/fixtures/web_animations/animations.js's steps (element.animate(), Animation,
// KeyframeEffect: states, timing, computed values at set times, the order of events and promises) run in a View, a
// frame of 16 ms at a time until they are over, against what tools/browser_web_animations_reference.mjs recorded them
// doing in Chrome (chrome-results.json), entry by entry. Portable.
namespace {
    using weft::testing::JsonValue;
    using weft::testing::require;

    const std::string directory = WEFT_SOURCE_DIR "/tests/fixtures/web_animations/";

    std::shared_ptr<const weft::FontCatalog> catalog() {
        auto c = std::make_shared<weft::FontCatalog>();
        weft::FontFaceEntry face;
        face.family = "Roboto";
        face.source.path = WEFT_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf";
        c->faces.push_back(face);
        c->generic_families = {{"sans-serif", "Roboto"}, {"system-ui", "Roboto"}, {"monospace", "Roboto"}};
        return c;
    }

    // A value as compact JSON text, to compare log entries.
    std::string json_text(const JsonValue& v) {
        switch (v.kind) {
            case JsonValue::Kind::Null:
                return "null";
            case JsonValue::Kind::Bool:
                return v.boolean ? "true" : "false";
            case JsonValue::Kind::Number: {
                std::ostringstream s;
                s << v.number;
                return s.str();
            }
            case JsonValue::Kind::String: {
                std::string out = "\"";
                for (const char c : v.string) {
                    out += c == '\n' ? std::string("\\n") : std::string(1, c);
                }
                return out + "\"";
            }
            case JsonValue::Kind::Array: {
                std::string out = "[";
                for (std::size_t i = 0; i < v.array.size(); ++i) {
                    out += (i ? "," : "") + json_text(v.array[i]);
                }
                return out + "]";
            }
            case JsonValue::Kind::Object:
                break;
        }
        return "{}";
    }
} // namespace

int main() {
    try {
        const auto fixture = weft::testing::parse_json(weft::testing::read_file(directory + "chrome-results.json"));
        // The animate page's copy of the log (examples/animate/chrome-log.js), which it checks the same steps against on
        // the device it runs on: the same as the recording.
        {
            const auto copy = weft::testing::read_file(WEFT_SOURCE_DIR "/examples/animate/chrome-log.js");
            const auto begin = copy.find('{');
            const auto end = copy.rfind('}');
            require(begin != std::string::npos && end != std::string::npos && begin < end,
                    "examples/animate/chrome-log.js holds an object");
            const auto page = weft::testing::parse_json(copy.substr(begin, end - begin + 1));
            require(page["browser"].string == fixture["browser"].string &&
                        json_text(page["log"]) == json_text(fixture["log"]),
                    "examples/animate/chrome-log.js is chrome-results.json's log");
        }
        const auto fonts = catalog();
        weft::SceneText text(*fonts);
        weft::View view(fonts, text, 800, 600, 1);
        weft::ViewPage page;
        page.scripts.push_back({"animations.js", weft::testing::read_file(directory + "animations.js")});
        page.stylesheet = "#root{position:relative}";
        page.forgiving_css = true;
        page.web_platform = true;
        view.load(page);
        double time = 16;
        view.advance(time);
        const auto eval = [&](const std::string& source) {
            const auto [ok, result] = view.evaluate(source);
            require(ok, "evaluates: " + source + " (" + result + ")");
            return result;
        };
        eval("window.__run(); 0");
        int frames{};
        for (; frames < 4000 && eval("String(window.__done)") != "true"; ++frames) {
            time += 16;
            view.advance(time);
        }
        require(eval("String(window.__done)") == "true", "the steps end");
        for (const auto& d : view.take_diagnostics()) {
            std::cout << "  diagnostic: " << d.message << '\n';
        }
        const auto native = weft::testing::parse_json(eval("JSON.stringify(window.__log)"));
        const auto& chrome = fixture["log"].array;
        std::size_t failures{};
        for (std::size_t k = 0; k < std::max(native.array.size(), chrome.size()); ++k) {
            const auto got = k < native.array.size() ? json_text(native.array[k]) : std::string("(none)");
            const auto want = k < chrome.size() ? json_text(chrome[k]) : std::string("(none)");
            if (got != want) {
                if (++failures <= 40) {
                    std::cout << "entry " << k << ":\n  weft   " << got << "\n  Chrome " << want << '\n';
                }
            }
        }
        std::cout << chrome.size() << " log entries vs " << fixture["browser"].string << " over " << frames
                  << " frames: " << failures << " differ\n";
        require(failures == 0, "Web Animations match Chrome");

        // A running animation, frame by frame: the value the scene solves at each frame's time is the one its current
        // time gives (opacity 0 to 1 over 160 ms, linear), from the frame after it starts to the one after it ends,
        // when it shows its element's own value again. The specification's arithmetic, not a recording.
        eval("const running = document.createElement('div'); running.setAttribute('style', 'position:absolute;"
             "width:10px;height:10px;opacity:0.5'); document.getElementById('root').appendChild(running); "
             "window.__running = running.animate({opacity: [0, 1]}, 160); 0");
        std::size_t samples{};
        for (int k = 0; k < 14; ++k) {
            time += 16;
            view.advance(time);
            const auto read = weft::testing::parse_json(eval(
                "JSON.stringify([window.__running.currentTime, window.__running.playState, "
                "Number(getComputedStyle(window.__running.effect.target).opacity)])"));
            const auto& current = read[0];
            const double opacity = read[2].number;
            const double expected = read[1].string == "finished" ? 0.5 : current.number / 160;
            if (std::abs(opacity - expected) > 1e-3) {
                throw std::runtime_error("frame " + std::to_string(k) + ": opacity " + std::to_string(opacity) +
                                         " where the current time gives " + std::to_string(expected) + " (" +
                                         read[1].string + ")");
            }
            ++samples;
        }
        require(eval("window.__running.playState") == "finished", "the running animation finishes");
        std::cout << samples << " frames of a running animation: the scene's values follow its current time\n";
        std::cout << "PASS Web Animations against Chrome\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
