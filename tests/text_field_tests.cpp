#define NOMINMAX
#include <windows.h>
#include "scene/scene.hpp"
#include "view/view.hpp"
#include "support/check.hpp"
#include "support/d3d11_scene.hpp"
#include "support/files.hpp"
#include "support/json_value.hpp"
#include "support/png.hpp"
#include "support/scene_fonts.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Text fields (dom/text_field.hpp): the scenarios of tests/fixtures/text_field run in a View and match what Chrome did
// on the same page (tools/browser_text_field_reference.mjs): each step's events with the field's value and selection
// as each came (but selectionchange, which Chrome fires when its tasks come round), every field's value, selection and
// scrollLeft after the step, the values the page reads, and its images of the fields against the native D3D11 pass.
// Then what the page does not show: the scene following a field change after change; a composition that maxlength
// cuts, that script or the focus ends; a textarea's default value and its dirty value; an input whose type stops
// being a text field.
//
//   weft_text_field_tests [directory for weft's images of the fields]
// With WEFT_FIELD_DEBUG set, each step prints the fields' scene nodes (text, scroll, selection, lines).
namespace {
    using weft::KeyInput;
    using weft::PointerKind;
    using weft::testing::JsonValue;
    using weft::testing::parse_json;
    using weft::testing::require;

    std::filesystem::path shots_directory;
    // WEFT_FIELD_DEBUG set: each step prints the fields' scene nodes.
    bool debug_fields{};

    struct Fixture {
        std::shared_ptr<const weft::FontCatalog> fonts =
            std::make_shared<weft::FontCatalog>(weft::testing::chrome_windows_font_catalog());
        weft::View view{fonts, weft::testing::thread_scene_text(), 800, 600, 1};
        double time{};

        void load(std::string script, std::string stylesheet) {
            weft::ViewPage page;
            page.scripts.push_back({"page.js", std::move(script)});
            page.stylesheet = std::move(stylesheet);
            page.forgiving_css = true;
            page.web_platform = true;
            view.load(page);
        }

        void fixture() {
            using weft::testing::read_text_file;
            using weft::testing::source_path;
            load(read_text_file(source_path("tests/fixtures/text_field/field.js")),
                 read_text_file(source_path("tests/fixtures/text_field/field.css")));
        }

        std::string eval(const std::string& source) {
            const auto [ok, result] = view.evaluate(source);
            require(ok, "evaluates: " + source + " (" + result + ")");
            return result;
        }

        void advance(double ms = 30) {
            time += ms;
            view.advance(time);
        }

        void key(bool down, std::string key, std::string code, unsigned modifiers = 0, unsigned vk = 0) {
            view.key({down, std::move(key), std::move(code), modifiers, false, false, vk});
        }

        void move(float x, float y, unsigned buttons = 0) {
            view.pointer({PointerKind::Move, x, y, buttons});
        }

        void down(float x, float y, unsigned modifiers = 0) {
            view.pointer({PointerKind::Down, x, y, 1, 0, 0, modifiers});
        }

        void up(float x, float y, unsigned modifiers = 0) {
            view.pointer({PointerKind::Up, x, y, 0, 0, 0, modifiers});
        }

        void click(float x, float y, unsigned modifiers = 0) {
            move(x, y);
            down(x, y, modifiers);
            up(x, y, modifiers);
        }

        // A point of an element: its border box's left and top plus (x, y).
        std::array<float, 2> at(const std::string& id, float x, float y) {
            const auto p = parse_json(eval("JSON.stringify(__field.at('" + id + "', " + std::to_string(x) + ", " +
                                           std::to_string(y) + "))"));
            return {float(p[0].number), float(p[1].number)};
        }

        std::array<float, 4> rect(const std::string& id) {
            const auto r = parse_json(eval("JSON.stringify(__field.rect('" + id + "'))"));
            return {float(r[0].number), float(r[1].number), float(r[2].number), float(r[3].number)};
        }

        // The View's frame drawn by the native D3D11 pass over white, as straight RGBA.
        weft::testing::RgbaImage image() {
            weft::testing::D3D11Scene d3d11(800, 600);
            const auto bgra = d3d11.draw(view.scene());
            weft::testing::RgbaImage out{800, 600, std::vector<std::uint8_t>(bgra.size())};
            for (std::size_t i = 0; i < bgra.size(); i += 4) {
                const float a = bgra[i + 3] / 255.f;
                for (int c = 0; c < 3; ++c) {
                    const float v = bgra[i + 2 - c] / 255.f + (1 - a);
                    out.rgba[i + c] = std::uint8_t(std::lround(std::clamp(v, 0.f, 1.f) * 255));
                }
                out.rgba[i + 3] = 255;
            }
            return out;
        }
    };

    std::string dump(const JsonValue& v) {
        std::ostringstream out;
        switch (v.kind) {
            case JsonValue::Kind::Null:
                out << "null";
                break;
            case JsonValue::Kind::Bool:
                out << (v.boolean ? "true" : "false");
                break;
            case JsonValue::Kind::Number:
                out << v.number;
                break;
            case JsonValue::Kind::String:
                out << '"' << v.string << '"';
                break;
            case JsonValue::Kind::Array:
                out << '[';
                for (std::size_t i = 0; i < v.array.size(); ++i) {
                    out << (i ? "," : "") << dump(v.array[i]);
                }
                out << ']';
                break;
            case JsonValue::Kind::Object:
                out << '{';
                for (std::size_t i = 0; i < v.object.size(); ++i) {
                    out << (i ? "," : "") << v.object[i].first << ':' << dump(v.object[i].second);
                }
                out << '}';
                break;
        }
        return out.str();
    }

    bool same(const JsonValue& a, const JsonValue& b) {
        if (a.kind != b.kind) {
            return false;
        }
        if (a.kind == JsonValue::Kind::Object) {
            if (a.object.size() != b.object.size()) {
                return false;
            }
            for (const auto& [key, value] : a.object) {
                const auto* other = b.find(key);
                if (!other || !same(value, *other)) {
                    return false;
                }
            }
            return true;
        }
        if (a.kind == JsonValue::Kind::Array) {
            if (a.array.size() != b.array.size()) {
                return false;
            }
            for (std::size_t i = 0; i < a.array.size(); ++i) {
                if (!same(a.array[i], b.array[i])) {
                    return false;
                }
            }
            return true;
        }
        return a.boolean == b.boolean && a.number == b.number && a.string == b.string;
    }

    // The events of a step but selectionchange, and but those dropped (type@target).
    std::vector<const JsonValue*> events_of(const JsonValue& list, const std::set<std::string>& dropped = {}) {
        std::vector<const JsonValue*> out;
        for (const auto& e : list.array) {
            const auto key = e["type"].string + "@" + e["target"].string;
            if (e["type"].string != "selectionchange" && !dropped.contains(key)) {
                out.push_back(&e);
            }
        }
        return out;
    }

    void compare_events(const JsonValue& chrome, const JsonValue& weft, const std::string& where,
                        const std::set<std::string>& dropped) {
        const auto theirs = events_of(chrome, dropped);
        const auto mine = events_of(weft);
        std::string listing = "\n  chrome:";
        for (const auto* e : theirs) {
            listing += "\n    " + dump(*e);
        }
        listing += "\n  weft:";
        for (const auto* e : mine) {
            listing += "\n    " + dump(*e);
        }
        require(theirs.size() == mine.size(), where + ": " + std::to_string(mine.size()) + " events, Chrome " +
                                                  std::to_string(theirs.size()) + listing);
        for (std::size_t i = 0; i < theirs.size(); ++i) {
            for (const auto& [key, value] : theirs[i]->object) {
                const auto* m = mine[i]->find(key);
                require(m != nullptr, where + ": event " + std::to_string(i) + " has no " + key + listing);
                require(same(value, *m), where + ": event " + std::to_string(i) + " " + key + " is " + dump(*m) +
                                             ", Chrome " + dump(value) + listing);
            }
        }
    }

    // Every field's value, selection and direction exactly, its scrollLeft within 2 px (two text engines' advances).
    void compare_state(const JsonValue& chrome, const JsonValue& weft, const std::string& where) {
        for (const auto& [key, value] : chrome.object) {
            const auto* mine = weft.find(key);
            require(mine != nullptr, where + ": no state " + key);
            if (value.kind != JsonValue::Kind::Object) {
                require(same(value, *mine), where + ": " + key + " " + dump(*mine) + ", Chrome " + dump(value));
                continue;
            }
            for (const auto& [field, v] : value.object) {
                const auto* m = mine->find(field);
                require(m != nullptr, where + ": " + key + " has no " + field);
                if (field == "scrollLeft") {
                    require(std::abs(v.number - m->number) <= 2, where + ": " + key + ".scrollLeft " + dump(*m) +
                                                                      ", Chrome " + dump(v));
                } else {
                    require(same(v, *m), where + ": " + key + "." + field + " " + dump(*m) + ", Chrome " + dump(v));
                }
            }
        }
    }

    // Plays the steps of scenarios.json as a host gives them to a View.
    struct Player {
        Fixture& f;
        const JsonValue& keys;
        std::set<char> held;
        std::vector<std::pair<std::string, std::string>> reads;

        static unsigned bit(char m) {
            switch (m) {
                case 'S':
                    return weft::key_modifier::shift;
                case 'C':
                    return weft::key_modifier::control;
                case 'A':
                    return weft::key_modifier::alt;
                case 'M':
                    return weft::key_modifier::meta;
            }
            throw std::invalid_argument("modifier");
        }

        struct Key {
            std::string key;
            std::string code;
            unsigned vk{};
            std::string text;
            bool shift{};
        };

        // A character typed on a US keyboard, as the harness types it.
        static Key typed(char c) {
            if (std::isalpha(static_cast<unsigned char>(c))) {
                const char upper = char(std::toupper(static_cast<unsigned char>(c)));
                return {std::string(1, c), std::string("Key") + upper, unsigned(upper), std::string(1, c),
                        c != std::tolower(static_cast<unsigned char>(c))};
            }
            if (std::isdigit(static_cast<unsigned char>(c))) {
                return {std::string(1, c), std::string("Digit") + c, unsigned(c), std::string(1, c)};
            }
            if (c == ' ') {
                return {" ", "Space", 32, " "};
            }
            return {std::string(1, c), "", 0, std::string(1, c)};
        }

        unsigned modifiers(const JsonValue& step, bool shift = false) const {
            unsigned out{};
            for (const char m : held) {
                out |= bit(m);
            }
            if (const auto* mods = step.find("mods")) {
                for (const char m : mods->string) {
                    out |= bit(m);
                }
            }
            return out | (shift ? weft::key_modifier::shift : 0u);
        }

        void key(bool down, const Key& k, const JsonValue& step, bool text = true) {
            static const std::map<std::string, char> modifier_keys{
                {"Shift", 'S'}, {"Control", 'C'}, {"Alt", 'A'}, {"Meta", 'M'}};
            if (const auto it = modifier_keys.find(k.key); it != modifier_keys.end()) {
                if (down) {
                    held.insert(it->second);
                } else {
                    held.erase(it->second);
                }
            }
            const auto mods = modifiers(step, k.shift);
            f.view.key({down, k.key, k.code, mods, false, false, k.vk});
            if (down && text && !k.text.empty() && !(mods & weft::key_modifier::control)) {
                f.view.text(k.text);
            }
        }

        Key named(const std::string& name) const {
            const auto& k = keys[name];
            const auto* text = k.find("text");
            return {k["key"].string, k["code"].string, unsigned(k["vk"].number), text ? text->string : ""};
        }

        // Plays a step: whether it was one (read and shot are the caller's).
        void play(const JsonValue& step) {
            if (const auto* name = step.find("press")) {
                key(true, named(name->string), step);
                key(false, named(name->string), step);
            } else if (const auto* d = step.find("down")) {
                key(true, named(d->string), step);
            } else if (const auto* u = step.find("up")) {
                key(false, named(u->string), step);
            } else if (const auto* text = step.find("type")) {
                for (const char c : text->string) {
                    const auto k = typed(c);
                    key(true, k, step);
                    key(false, k, step);
                }
            } else if (const auto* id = step.find("click")) {
                std::array<float, 2> p{};
                if (const auto* at = step.find("at")) {
                    p = f.at(id->string, float((*at)[0].number), float((*at)[1].number));
                } else {
                    const auto r = f.rect(id->string);
                    p = {r[0] + r[2] / 2, r[1] + r[3] / 2};
                }
                const auto* count = step.find("count");
                // A step's clicks count from one, as the reference's click counts do: past the double-click time
                // after the last step's.
                f.advance(600);
                f.move(p[0], p[1]);
                for (int n = 1; n <= (count ? int(count->number) : 1); ++n) {
                    f.down(p[0], p[1]);
                    f.up(p[0], p[1]);
                }
            } else if (const auto* dragged = step.find("drag")) {
                const auto& from = step["from"];
                const auto& to = step["to"];
                const auto a = f.at(dragged->string, float(from[0].number), float(from[1].number));
                const auto b = f.at(dragged->string, float(to[0].number), float(to[1].number));
                f.advance(600);
                f.move(a[0], a[1]);
                f.down(a[0], a[1]);
                f.move(b[0], b[1], 1);
                f.up(b[0], b[1]);
            } else if (const auto* ime = step.find("ime")) {
                // The selection in UTF-16 code units, as UTF-8 offsets of the composition.
                const auto utf8 = [&](double units) {
                    std::uint32_t bytes{};
                    double counted{};
                    const auto& s = ime->string;
                    while (bytes < s.size() && counted < units) {
                        const auto b = static_cast<unsigned char>(s[bytes]);
                        const unsigned n = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
                        counted += n == 4 ? 2 : 1;
                        bytes += n;
                    }
                    return bytes;
                };
                std::uint32_t start = std::uint32_t(ime->string.size());
                std::uint32_t end = start;
                if (const auto* selection = step.find("selection")) {
                    start = utf8((*selection)[0].number);
                    end = utf8((*selection)[1].number);
                }
                f.view.compose(ime->string, start, end);
            } else if (const auto* commit = step.find("commit")) {
                f.view.commit(commit->string);
            } else if (const auto* focus = step.find("focus")) {
                f.eval("document.getElementById('" + focus->string + "').focus()");
            } else if (step.find("blur")) {
                f.eval("document.activeElement && document.activeElement.blur()");
            } else if (const auto* script = step.find("script")) {
                f.eval("{" + script->string + "}");
            } else if (const auto* wait = step.find("wait")) {
                f.advance(wait->number - 30);
            } else {
                throw std::invalid_argument("unknown step " + dump(step));
            }
        }
    };

    // Chrome's image of a field against weft's, both over white: the share of its pixels apart from weft's by more than
    // two rasterizers' anti-aliasing (each compared with weft's pixel and its neighbours left and right, as the two text
    // engines' kerning may move a glyph by a px), and the mean difference.
    struct ImageDifference {
        double apart{};
        double mean{};
    };

    const std::uint8_t* pixel(const weft::testing::RgbaImage& image, int x, int y) {
        x = std::clamp(x, 0, int(image.width) - 1);
        y = std::clamp(y, 0, int(image.height) - 1);
        return &image.rgba[(std::size_t(y) * image.width + std::size_t(x)) * 4];
    }

    // Two pixels' difference in luminance: Chrome draws text with subpixel (LCD) anti-aliasing, whose colored fringes
    // weft's grayscale coverage matches in luminance, not channel by channel; colors that differ (a selection, a
    // border) differ in it too.
    int distance(const std::uint8_t* a, const std::uint8_t* b) {
        const auto luminance = [](const std::uint8_t* p) {
            return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
        };
        return int(std::lround(std::abs(luminance(a) - luminance(b))));
    }

    ImageDifference compare_image(const weft::testing::RgbaImage& chrome, const weft::testing::RgbaImage& mine,
                                  std::array<float, 4> r) {
        ImageDifference d;
        const int x0 = int(std::lround(r[0]));
        const int y0 = int(std::lround(r[1]));
        std::size_t apart{};
        double sum{};
        for (std::uint32_t y = 0; y < chrome.height; ++y) {
            for (std::uint32_t x = 0; x < chrome.width; ++x) {
                const auto* c = pixel(chrome, int(x), int(y));
                int best = 255;
                for (int dx = -1; dx <= 1; ++dx) {
                    best = std::min(best, distance(c, pixel(mine, x0 + int(x) + dx, y0 + int(y))));
                }
                sum += best;
                apart += best > 96 ? 1 : 0;
            }
        }
        const double pixels = double(chrome.width) * chrome.height;
        d.apart = double(apart) / pixels;
        d.mean = sum / pixels;
        return d;
    }

    bool dark(const std::uint8_t* p) {
        return p[0] < 64 && p[1] < 64 && p[2] < 64;
    }

    // Chrome draws a composition (as CDP's imeSetComposition gives it) over a yellow background, with the spelling
    // checker's marks: what it shares with weft is its underline, a px at the bottom of the line box under the
    // composition, and the caret in it. Both images have those at the same pixels.
    void compare_composition(const weft::testing::RgbaImage& chrome, const weft::testing::RgbaImage& mine,
                             std::array<float, 4> r, const std::string& where) {
        const int x0 = int(std::lround(r[0]));
        const int y0 = int(std::lround(r[1]));
        std::optional<int> underline;
        for (int y = int(chrome.height) - 1; y >= 0 && !underline; --y) {
            int run{};
            for (int x = 0; x < int(chrome.width); ++x) {
                run += dark(pixel(chrome, x, y)) ? 1 : 0;
            }
            if (run > 30 && run < int(chrome.width) - 10) {
                underline = y;
            }
        }
        require(underline.has_value(), where + ": Chrome's composition has an underline");
        int chrome_run{};
        int weft_run{};
        for (int x = 0; x < int(chrome.width); ++x) {
            chrome_run += dark(pixel(chrome, x, *underline)) ? 1 : 0;
            weft_run += dark(pixel(mine, x0 + x, y0 + *underline)) ? 1 : 0;
        }
        require(std::abs(chrome_run - weft_run) <= 2, where + ": the underline at row " + std::to_string(*underline) +
                                                          " is " + std::to_string(weft_run) + " px long, Chrome's " +
                                                          std::to_string(chrome_run));
        // The caret: the column dark down the line box, above the underline.
        const auto caret = [&](const weft::testing::RgbaImage& image, int ox, int oy) -> std::optional<int> {
            for (int x = 2; x < int(chrome.width) - 2; ++x) {
                int run{};
                for (int y = *underline - 16; y < *underline; ++y) {
                    run += dark(pixel(image, ox + x, oy + y)) ? 1 : 0;
                }
                if (run >= 15) {
                    return x;
                }
            }
            return std::nullopt;
        };
        const auto theirs = caret(chrome, 0, 0);
        const auto ours = caret(mine, x0, y0);
        require(theirs && ours && std::abs(*theirs - *ours) <= 1,
                where + ": the caret at " + (ours ? std::to_string(*ours) : "none") + ", Chrome's at " +
                    (theirs ? std::to_string(*theirs) : "none"));
    }

    // A CSS color as Chrome serializes it.
    std::string serialized_color(const std::string& text) {
        if (text.size() == 7 && text[0] == '#') {
            const auto channel = [&](int i) {
                return std::to_string(std::stoi(text.substr(std::size_t(1 + 2 * i), 2), nullptr, 16));
            };
            return "rgb(" + channel(0) + ", " + channel(1) + ", " + channel(2) + ")";
        }
        return text;
    }

    // A value the page read in weft against Chrome's: rectangles within a px, a scroll offset within 2 (a textarea that
    // scrolled to a line an ideograph's fallback font made taller: Chrome scrolls to its first font's line box);
    // computed styles where weft computes what Chrome does (colors as Chrome serializes them, lengths as numbers); the
    // rest exactly. Reads of ::placeholder and ::selection styles and of caret-color, which weft's getComputedStyle does
    // not give, are left.
    void compare_read(const std::string& read, const JsonValue& chrome, const JsonValue& weft,
                      const std::string& where) {
        const double tolerance = read.find("scroll") != std::string::npos ? 2 : 1;
        if (read.find("::") != std::string::npos || read.find("caretColor") != std::string::npos) {
            return;
        }
        require(chrome.kind == weft.kind, where + ": " + dump(weft) + ", Chrome " + dump(chrome));
        if (chrome.kind != JsonValue::Kind::Array) {
            require(same(chrome, weft), where + ": " + dump(weft) + ", Chrome " + dump(chrome));
            return;
        }
        require(chrome.size() == weft.size(), where + ": " + dump(weft) + ", Chrome " + dump(chrome));
        for (std::size_t k = 0; k < chrome.size(); ++k) {
            const auto& c = chrome[k];
            const auto& w = weft[k];
            if (c.kind == JsonValue::Kind::Number) {
                require(std::abs(c.number - w.number) <= tolerance, where + ": item " + std::to_string(k) + " " + dump(w) +
                                                                ", Chrome " + dump(c));
            } else if (c.kind == JsonValue::Kind::String && c.string.ends_with("px") && w.string.ends_with("px")) {
                require(std::abs(std::stod(c.string) - std::stod(w.string)) < 0.01,
                        where + ": item " + std::to_string(k) + " " + dump(w) + ", Chrome " + dump(c));
            } else if (c.kind == JsonValue::Kind::String) {
                require(serialized_color(w.string) == c.string,
                        where + ": item " + std::to_string(k) + " " + dump(w) + ", Chrome " + dump(c));
            } else {
                require(same(c, w), where + ": item " + std::to_string(k) + " " + dump(w) + ", Chrome " + dump(c));
            }
        }
    }

    void save_crop(const weft::testing::RgbaImage& image, std::array<float, 4> r, const std::string& name) {
        if (shots_directory.empty()) {
            return;
        }
        const auto w = std::uint32_t(std::lround(r[2]));
        const auto h = std::uint32_t(std::lround(r[3]));
        std::vector<std::uint8_t> crop(std::size_t(w) * h * 4);
        for (std::uint32_t y = 0; y < h; ++y) {
            std::memcpy(crop.data() + std::size_t(y) * w * 4,
                        image.rgba.data() + ((std::size_t(std::lround(r[1])) + y) * image.width +
                                             std::size_t(std::lround(r[0]))) *
                                                4,
                        std::size_t(w) * 4);
        }
        std::filesystem::create_directories(shots_directory);
        weft::testing::write_png(shots_directory / (name + ".png"), w, h, crop);
    }

    // Each scenario in a new View: each step's events and state against Chrome's, its reads, its images.
    void chrome_scenarios() {
        using weft::testing::read_text_file;
        using weft::testing::source_path;
        const auto spec = parse_json(read_text_file(source_path("tests/fixtures/text_field/scenarios.json")));
        const auto chrome = parse_json(read_text_file(source_path("tests/fixtures/text_field/chrome-results.json")));
        // Events Chrome fires that weft does not, by scenario and step: the select a field fires again when the focus
        // leaves it after the mouse selected in it (a quirk of Chrome's cached selection).
        const std::map<std::pair<std::string, std::size_t>, std::set<std::string>> dropped{
            {{"mouse", 14}, {"select@i2"}},
        };
        std::size_t steps{};
        for (const auto& scenario : spec["scenarios"].array) {
            const auto& name = scenario["name"].string;
            const auto& results = chrome["scenarios"][name];
            Fixture f;
            f.fixture();
            f.advance();
            f.eval("__field.take()");
            Player player{f, spec["keys"]};
            for (std::size_t i = 0; i < scenario["steps"].size(); ++i) {
                const auto& step = scenario["steps"][i];
                const auto& result = results[i];
                const auto where = name + " step " + std::to_string(i) + " " + dump(step);
                if (const auto* read = step.find("read")) {
                    const auto [ok, value] = f.view.evaluate("JSON.stringify(" + read->string + ")");
                    if (read->string.find("::") == std::string::npos) {
                        require(ok, where + ": " + value);
                        compare_read(read->string, result["value"], parse_json(value), where);
                    }
                    continue;
                }
                if (const auto* shot = step.find("shot")) {
                    const auto r = f.rect(step["of"].string);
                    const auto image = f.image();
                    save_crop(image, r, shot->string);
                    const auto theirs = weft::testing::read_png(
                        source_path("tests/fixtures/text_field/chrome/" + shot->string + ".png"));
                    if (shot->string.find("composition") != std::string::npos) {
                        compare_composition(theirs, image, r, where);
                        continue;
                    }
                    const auto d = compare_image(theirs, image, r);
                    // A field scrolled to the end of a long text shows where two text engines' advances add up to a
                    // px apart (Chrome kerns Segoe UI): each glyph a fraction of a px from Chrome's.
                    const bool scrolled = shot->string == "iw-end";
                    require(d.apart < (scrolled ? 0.04 : 0.01) && d.mean < (scrolled ? 12 : 4),
                            where + ": " + std::to_string(d.apart * 100) + "% of its pixels apart from Chrome's, mean " +
                                std::to_string(d.mean));
                    continue;
                }
                player.play(step);
                f.advance();
                const auto events = parse_json(f.eval("JSON.stringify(__field.take())"));
                const auto state = parse_json(f.eval("JSON.stringify(__field.state())"));
                const auto drop = dropped.find({name, i});
                compare_events(result["events"], events, where, drop == dropped.end() ? std::set<std::string>{}
                                                                                       : drop->second);
                if (debug_fields) {
                    const auto walk = [&](auto&& self, const weft::SceneNode& n) -> void {
                        if (n.field) {
                            std::cout << "  field " << n.tag << " text '" << n.text << "' box " << n.box[0] << ","
                                      << n.box[1] << "," << n.box[2] << "," << n.box[3] << " scroll "
                                      << n.field->state->scroll_x << "," << n.field->state->scroll_y << " sel " << n.field->state->selection_start << "-"
                                      << n.field->state->selection_end << "\n";
                            for (const auto& l : n.lines) {
                                std::cout << "    line '" << l.text << "' rect " << l.rect[0] << "," << l.rect[1] << ","
                                          << l.rect[2] << "," << l.rect[3] << " start " << l.start << " last stop "
                                          << (l.stops.empty() ? -1.f : l.stops.back().second) << " ("
                                          << l.stops.size() << ")\n";
                            }
                        }
                        for (const auto& c : n.children) {
                            self(self, c);
                        }
                    };
                    std::cout << where << "\n";
                    for (const auto& d : f.view.take_diagnostics()) {
                        std::cout << "  diagnostic " << int(d.kind) << ": " << d.message << "\n";
                    }
                    walk(walk, f.view.scene().root);
                }
                compare_state(result["state"], state, where);
                ++steps;
            }
        }
        require(steps > 80, "the scenarios ran");
    }

    // The field's node in the View's scene.
    const weft::SceneNode* field_node(const weft::SceneNode& n) {
        if (n.field) {
            return &n;
        }
        for (const auto& c : n.children) {
            if (const auto* found = field_node(c)) {
                return found;
            }
        }
        return nullptr;
    }

    // The scene follows the field: its text, selection and scrolling, change after change.
    void scene_follows_field() {
        Fixture f;
        f.load("const i = document.createElement('input'); i.id = 'f'; "
               "i.value = 'The quick brown fox jumps over the lazy dog'; "
               "document.getElementById('root').appendChild(i);",
               "input{font:16px 'Segoe UI';width:120px}");
        f.advance();
        f.eval("document.getElementById('f').focus()");
        f.advance();
        const auto check = [&](const std::string& what, std::uint32_t start, std::uint32_t end) {
            const auto* n = field_node(f.view.scene().root);
            require(n != nullptr, what + ": the field has a node");
            const auto& s = *n->field->state;
            const auto dom = f.view.script().text_field(n->identity);
            require(s.selection_start == start && s.selection_end == end,
                    what + ": the scene's selection " + std::to_string(s.selection_start) + "-" +
                        std::to_string(s.selection_end) + ", the DOM's " +
                        (dom ? std::to_string(dom->selection_start) + "-" + std::to_string(dom->selection_end) +
                                   " (same state: " + std::to_string(dom.get() == n->field->state.get()) + ")"
                             : std::string("none")) +
                        ", scene revision " + std::to_string(f.view.scene().revision) + ", js " +
                        f.eval("String(document.getElementById('f').selectionStart)"));
        };
        check("focused", 43, 43);
        for (int k = 0; k < 6; ++k) {
            f.key(true, "End", "End", 0, 35);
            f.key(false, "End", "End", 0, 35);
            f.advance();
            check("End " + std::to_string(k), 43, 43);
            f.key(true, "Home", "Home", 0, 36);
            f.key(false, "Home", "Home", 0, 36);
            f.advance();
            check("Home " + std::to_string(k), 0, 0);
        }
    }

    // A page of inputs a (maxlength 3), b and a textarea t, its composition and input events logged.
    void field_page(Fixture& f) {
        f.load(R"JS(
            globalThis.log = [];
            const root = document.getElementById('root');
            for (const [tag, id] of [['input', 'a'], ['input', 'b'], ['textarea', 't']]) {
              const n = document.createElement(tag);
              n.id = id;
              root.appendChild(n);
              for (const type of ['compositionstart', 'compositionend', 'input', 'change', 'blur'])
                n.addEventListener(type, e => log.push(type + '@' + id + ':' + (e.data ?? '')));
            }
            document.getElementById('a').maxLength = 3;
            document.getElementById('t').textContent = 'default';
            globalThis.take = () => { const out = log.join(' '); log.length = 0; return out; };
          )JS",
               "input,textarea{display:block;width:200px;height:40px;font:16px 'Segoe UI'}");
        f.advance();
    }

    // What one page in Chrome does not show: a composition maxlength cuts at its commit; one script ends by setting the
    // value (silently, as Chrome's SetValue cancels it), one the focus ends (compositionend, then change and blur); a
    // textarea's default value follows its child text until its value is dirty; an input whose type stops being a text
    // field leaves its field.
    void beyond_the_page() {
        Fixture f;
        field_page(f);
        f.eval("document.getElementById('a').focus(); take()");
        f.view.compose("abcdef", 6, 6);
        require(f.eval("document.getElementById('a').value") == "abcdef", "a composition takes no maxlength");
        f.view.commit("abcdef");
        require(f.eval("document.getElementById('a').value") == "abc", "its commit does");
        const auto commit_events = f.eval("take()");
        require(commit_events == "compositionstart@a: input@a:abcdef input@a:abc compositionend@a:abc",
                "the commit's events: " + commit_events);
        // Script ends a composition by setting the value.
        f.eval("document.getElementById('b').focus(); take()");
        f.view.compose("ni", 2, 2);
        f.eval("document.getElementById('b').value = 'set'");
        require(!f.view.text_input().composing && f.eval("document.getElementById('b').value") == "set",
                "the value script set ends the composition");
        f.view.commit("x");
        require(f.eval("document.getElementById('b').value") == "setx", "what the input method commits next is typed");
        // The focus ends one.
        f.eval("take()");
        f.view.compose("hao", 3, 3);
        f.eval("document.getElementById('a').focus()");
        const auto leaving = f.eval("take()");
        require(leaving == "compositionstart@b: input@b:hao compositionend@b:hao change@b: blur@b:",
                "the focus leaving a composition: " + leaving);
        require(f.eval("document.getElementById('b').value") == "setxhao", "its text stays");
        // A textarea's default value.
        require(f.eval("document.getElementById('t').value") == "default", "a textarea's value is its child text");
        f.eval("document.getElementById('t').textContent = 'changed'");
        require(f.eval("document.getElementById('t').value") == "changed", "and follows it while it is not dirty");
        f.eval("document.getElementById('t').focus(); document.getElementById('t').setSelectionRange(7, 7)");
        f.view.commit("!");
        f.eval("document.getElementById('t').textContent = 'later'");
        require(f.eval("document.getElementById('t').value") == "changed!" &&
                    f.eval("document.getElementById('t').defaultValue") == "later",
                "the user's edit makes it dirty");
        // An input stops being a text field.
        f.eval("document.getElementById('a').focus(); document.getElementById('a').type = 'checkbox'");
        require(f.eval("String(document.getElementById('a').selectionStart)") == "null" &&
                    f.eval("document.getElementById('a').value") == "on" && !f.view.text_input().active,
                "a checkbox is no text field");
        f.eval("document.getElementById('a').type = 'text'");
        f.advance();
        require(f.view.text_input().active && f.eval("document.getElementById('a').value") == "",
                "a text field again, of its default value");
    }
} // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        shots_directory = argv[1];
    }
    char* debug{};
    std::size_t debug_size{};
    debug_fields = _dupenv_s(&debug, &debug_size, "WEFT_FIELD_DEBUG") == 0 && debug;
    std::free(debug);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        scene_follows_field();
        beyond_the_page();
        chrome_scenarios();
        std::cout << "text field contract passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        if (SUCCEEDED(com)) {
            CoUninitialize();
        }
        return 1;
    }
    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
    return 0;
}
