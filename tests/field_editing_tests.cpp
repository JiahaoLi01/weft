#include "support/check.hpp"
#include "support/files.hpp"
#include "support/json_value.hpp"
#include "view/view.hpp"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// Copy, cut, paste, undo and redo in text fields against Chrome: tests/fixtures/field_editing/steps.json run in a View on
// editing.js's fields, with the platform's editing shortcuts (editing_modifier), against what
// tools/browser_field_editing_reference.mjs recorded Chrome doing with the same steps (chrome-results.json): every
// clipboard and input event at the fields (its clipboardData's or dataTransfer's text, inputType and data), and after
// the steps that say so, which field has the focus and each field's value and selection. The View's own clipboard
// stands in for the system's, as headless Chrome's does. Portable.
namespace {
    using weft::testing::JsonValue;
    using weft::testing::require;

    const std::string directory = WEFT_SOURCE_DIR "/tests/fixtures/field_editing/";

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

    std::string quote(const std::string& s) {
        std::string out = "'";
        for (const char c : s) {
            out += c == '\'' || c == '\\' ? std::string("\\") + c : c == '\n' ? std::string("\\n") : std::string(1, c);
        }
        return out + "'";
    }
} // namespace

int main() {
    try {
        const auto fixture = weft::testing::parse_json(weft::testing::read_file(directory + "chrome-results.json"));
        const auto steps = weft::testing::parse_json(weft::testing::read_file(directory + "steps.json"));
        const auto fonts = catalog();
        weft::SceneText text(*fonts);
        weft::View view(fonts, text, 800, 600, 1);
        weft::ViewPage page;
        page.scripts.push_back({"editing.js", weft::testing::read_file(directory + "editing.js")});
        page.stylesheet = "input,textarea{display:block;width:300px;font-family:sans-serif;font-size:16px}";
        page.forgiving_css = true;
        page.web_platform = true;
        view.load(page);
        double time = 0;
        const auto frame = [&] {
            time += 16;
            view.advance(time);
        };
        const auto eval = [&](const std::string& source) {
            const auto [ok, result] = view.evaluate(source);
            require(ok, "evaluates: " + source + " (" + result + ")");
            return result;
        };
        const auto press = [&](const std::string& key, const std::string& code, unsigned modifiers) {
            weft::KeyInput down;
            down.down = true;
            down.key = key;
            down.code = code;
            down.modifiers = modifiers;
            view.key(down);
            auto up = down;
            up.down = false;
            view.key(up);
        };
        frame();
        for (const auto& step : steps.array) {
            const auto& kind = step[0].string;
            if (kind == "focus") {
                eval("document.getElementById(" + quote(step[1].string) + ").focus()");
            } else if (kind == "select") {
                eval("document.activeElement.setSelectionRange(" + std::to_string(int(step[1].number)) + ", " +
                     std::to_string(int(step[2].number)) + ")");
            } else if (kind == "type") {
                for (const char c : step[1].string) {
                    const std::string key(1, c);
                    weft::KeyInput down;
                    down.down = true;
                    down.key = key;
                    view.key(down);
                    view.text(key);
                    auto up = down;
                    up.down = false;
                    view.key(up);
                }
            } else if (kind == "key") {
                press(step[1].string, step[1].string, 0);
            } else if (kind == "command") {
                const auto& name = step[1].string;
                const auto m = weft::editing_modifier;
                if (name == "copy") {
                    press("c", "KeyC", m);
                } else if (name == "cut") {
                    press("x", "KeyX", m);
                } else if (name == "paste") {
                    press("v", "KeyV", m);
                } else if (name == "undo") {
                    press("z", "KeyZ", m);
                } else if (name == "redo") {
                    press("Z", "KeyZ", m | weft::key_modifier::shift);
                } else if (name == "selectAll") {
                    press("a", "KeyA", m);
                } else {
                    throw std::runtime_error("command " + name);
                }
            } else if (kind == "script") {
                eval(step[1].string);
            } else if (kind == "state") {
                eval("__state(" + quote(step[1].string) + ")");
            } else {
                throw std::runtime_error("step " + kind);
            }
            frame();
        }
        const auto native = weft::testing::parse_json(eval("JSON.stringify(window.__log)"));
        const auto& chrome = fixture["log"].array;
        std::vector<std::string> failures;
        std::size_t states{};
        for (std::size_t k = 0; k < std::max(native.array.size(), chrome.size()); ++k) {
            const auto got = k < native.array.size() ? json_text(native.array[k]) : std::string("(none)");
            const auto want = k < chrome.size() ? json_text(chrome[k]) : std::string("(none)");
            if (got != want) {
                failures.push_back("entry " + std::to_string(k) + ": native " + got + ", Chrome " + want);
                // After the first difference the logs fall out of step: one is enough to read.
                break;
            }
            states += chrome[k][0].string == "state" ? 1 : 0;
        }
        std::cout << steps.size() << " steps vs " << fixture["browser"].string << ": " << chrome.size()
                  << " log entries (" << states << " states) compared; " << failures.size() << " failures\n";
        for (const auto& f : failures) {
            std::cout << "  " << f << '\n';
        }
        require(failures.empty(), "text field editing matches Chrome");
        std::cout << "PASS text field editing against Chrome\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
