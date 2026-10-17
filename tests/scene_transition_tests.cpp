#include "base/diagnostic.hpp"
#include "scene/scene.hpp"
#include "style/style.hpp"
#include "support/check.hpp"
#include "support/files.hpp"
#include "support/json_value.hpp"
#include "support/native_scene.hpp"
#include "support/scene_fonts.hpp"
#include "test_constants/scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

// Scene transitions: the transition shorthand and how the transition lists pair with transition-property (CSS
// Transitions 1 §2, §3).
// - Against Chrome (tools/browser_scene_transition_reference.mjs, tests/fixtures/scene_transition_reference.json): the
//   after-change longhands and the opacity, translation and background color of 15 cases at 9 times.
// - Random lists written as longhands (of any lengths) and as the shorthand, against a model of the pairing with linear
//   timing.
// - The shorthand's longhands (inline, important, inherit, initial) and the diagnostics of transitions outside the
//   profile: at load for the shorthand, when an element's style changes for its longhands.
// - Running transitions that end: a property no longer named, a change that starts none.
namespace {
    using weft::testing::JsonValue;
    using weft::testing::require;
    namespace limits = weft::test_constants::scene;

    // The values every case starts from and changes to (.t, then .t.on).
    const std::string base = ".t{width:40px;height:20px;opacity:0;transform:translateX(0px);background-color:#204080}"
                             ".t.on{opacity:1;transform:translateX(100px);background-color:#e0c010}";

    // A scene of one element of class t, which gains the class on at transition_change_ms.
    struct Change {
        weft::testing::NativeScene s;
        weft::dom::Node* element{};
        weft::SceneSolver solver;

        explicit Change(const std::string& sheet, const std::string& style = {})
            : solver(sheet, "", weft::testing::thread_scene_text()) {
            element = &s.html("div");
            s.set(*element, "class", "t");
            if (!style.empty()) {
                s.set(*element, "style", style);
            }
            s.tree.insert(*s.root, *element, nullptr);
            frame(0);
        }

        weft::SceneStyle frame(double time) {
            const auto f = solver.frame(s.tree.snapshot(*s.root), limits::transition_view_size,
                                        limits::transition_view_size, time);
            return f.root.children[0].style;
        }

        void set_class(const std::string& value) {
            s.set(*element, "class", value);
        }

        // The element's style at each time after the change (the first time is the frame that sees the change).
        std::vector<weft::SceneStyle> samples(const std::vector<double>& times) {
            set_class("t on");
            std::vector<weft::SceneStyle> out;
            for (const auto t : times) {
                out.push_back(frame(limits::transition_change_ms + t));
            }
            return out;
        }
    };

    // A number at the start of a value's text.
    double number(const std::string& v) {
        std::size_t used{};
        double n{};
        try {
            n = std::stod(v, &used);
        } catch (const std::exception&) {
        }
        require(used > 0, "a number: '" + v + "'");
        return n;
    }

    // The translation of a transform: translateX(<px>), Chrome's matrix(a, b, c, d, e, f), or none.
    double translate_x(const std::string& v) {
        if (v == "none") {
            return 0;
        }
        if (v.starts_with("matrix(")) {
            std::size_t at = 0;
            for (int comma = 0; comma < 4; ++comma) {
                at = v.find(',', at) + 1;
            }
            return number(v.substr(at));
        }
        require(v.starts_with("translateX("), "a translateX transform: " + v);
        return number(v.substr(std::string_view("translateX(").size()));
    }

    // The channels of a color in 0-255 levels.
    std::array<double, 3> channels(const std::string& v) {
        const auto c = weft::parse_css_color(v);
        return {c[0] * 255., c[1] * 255., c[2] * 255.};
    }

    // A list longhand's items, trimmed, in lowercase.
    std::vector<std::string> items(const std::string& list) {
        std::vector<std::string> out;
        std::string item;
        int depth = 0;
        for (const char ch : list + ",") {
            if (ch == ',' && !depth) {
                const auto first = item.find_first_not_of(' ');
                const auto last = item.find_last_not_of(' ');
                out.push_back(first == std::string::npos ? "" : item.substr(first, last - first + 1));
                item.clear();
                continue;
            }
            depth += ch == '(' ? 1 : ch == ')' ? -1 : 0;
            item += char(std::tolower(static_cast<unsigned char>(ch)));
        }
        return out;
    }

    // A time in ms (Chrome writes seconds).
    double ms(const std::string& v) {
        return v.ends_with("ms") ? number(v) : number(v) * 1000;
    }

    // A timing function as Chrome writes it: steps(n, end) is steps(n).
    std::string easing_text(std::string v) {
        for (const auto* end : {", end)", ", jump-end)"}) {
            if (const auto at = v.find(end); v.starts_with("steps(") && at != std::string::npos) {
                v = v.substr(0, at) + ")";
            }
        }
        return v;
    }

    void chrome() {
        const auto fixture = weft::testing::parse_json(
            weft::testing::read_file(weft::testing::source_path("tests/fixtures/scene_transition_reference.json")));
        double opacity_error{};
        double translate_error{};
        double channel_error{};
        for (const auto& c : fixture["cases"].array) {
            const auto& name = c["name"].string;
            std::vector<double> times;
            for (const auto& s : c["samples"].array) {
                times.push_back(s["time"].number);
            }
            Change change(c["sheet"].string);
            const auto styles = change.samples(times);
            // The after-change longhands, item by item.
            const auto& after = c["after"];
            // A longhand the element does not declare has its initial value.
            const auto ours = [&](std::string_view property) {
                const auto initial = property == "transition-property"          ? "all"
                                     : property == "transition-timing-function" ? "ease"
                                                                                : "0s";
                return std::string(styles[0].text(property, initial));
            };
            require(items(ours("transition-property")) == items(after["transition-property"].string),
                    name + ": transition-property " + ours("transition-property"));
            for (const auto* property : {"transition-duration", "transition-delay"}) {
                const auto a = items(ours(property));
                const auto b = items(after[property].string);
                require(a.size() == b.size(), name + ": the length of " + property);
                for (std::size_t i = 0; i < a.size(); ++i) {
                    require(std::abs(ms(a[i]) - ms(b[i])) < 1e-9, name + ": " + property + " " + ours(property));
                }
            }
            auto timing = items(ours("transition-timing-function"));
            std::transform(timing.begin(), timing.end(), timing.begin(), easing_text);
            require(timing == items(after["transition-timing-function"].string),
                    name + ": transition-timing-function " + ours("transition-timing-function"));
            // The values at each time.
            for (std::size_t i = 0; i < times.size(); ++i) {
                const auto& theirs = c["samples"].array[i]["values"];
                const auto& style = styles[i];
                const auto at = name + " at " + std::to_string(int(times[i])) + "ms: ";
                const double opacity = std::abs(number(style.at("opacity")) - number(theirs["opacity"].string));
                const double translate =
                    std::abs(translate_x(style.at("transform")) - translate_x(theirs["transform"].string));
                const auto a = channels(style.at("background-color"));
                const auto b = channels(theirs["background-color"].string);
                double channel{};
                for (std::size_t k = 0; k < a.size(); ++k) {
                    channel = std::max(channel, std::abs(a[k] - b[k]));
                }
                require(opacity <= limits::transition_opacity_tolerance,
                        at + "opacity " + style.at("opacity") + " (Chrome " + theirs["opacity"].string + ")");
                require(translate <= limits::transition_translate_tolerance_px,
                        at + "transform " + style.at("transform") + " (Chrome " + theirs["transform"].string + ")");
                require(channel <= limits::transition_channel_tolerance,
                        at + "background-color " + style.at("background-color") + " (Chrome " +
                            theirs["background-color"].string + ")");
                opacity_error = std::max(opacity_error, opacity);
                translate_error = std::max(translate_error, translate);
                channel_error = std::max(channel_error, channel);
            }
        }
        std::cout << "Chrome " << fixture["browser"].string << ": " << fixture["cases"].array.size()
                  << " cases, largest differences opacity " << opacity_error << ", translation " << translate_error
                  << "px, channel " << channel_error << "\n";
    }

    // Random transition lists against the CSS pairing: each property takes the last item that names it or all, the
    // duration and delay of that item's index in lists repeated to transition-property's length; a combined duration
    // of 0s or less starts no transition.
    struct Lists {
        std::vector<std::string> properties;
        std::vector<int> durations;
        std::vector<int> delays;
    };

    double model(const Lists& l, const std::string& property, double t) {
        for (std::size_t i = l.properties.size(); i-- > 0;) {
            if (l.properties[i] != property && l.properties[i] != "all") {
                continue;
            }
            const double duration = l.durations[i % l.durations.size()];
            const double delay = l.delays[i % l.delays.size()];
            if (std::max(duration, 0.) + delay <= 0) {
                return 1;
            }
            if (duration == 0) {
                return t >= delay ? 1 : 0;
            }
            return std::clamp((t - delay) / duration, 0., 1.);
        }
        return 1;
    }

    void random_lists() {
        std::mt19937 random(limits::transition_list_seed);
        const auto pick = [&](const auto& choices) {
            return choices[std::uniform_int_distribution<std::size_t>(0, std::size(choices) - 1)(random)];
        };
        const std::array<std::string, 4> names{"opacity", "transform", "background-color", "all"};
        const std::array<int, 4> durations{0, 100, 250, 400};
        const std::array<int, 5> delays{-150, -100, 0, 50, 150};
        const std::vector<double> times{0, 30, 90, 160, 230, 310, 390, 470, 620};
        const auto join = [](const auto& list, const char* unit) {
            std::string out;
            for (const auto& v : list) {
                if constexpr (std::is_same_v<std::decay_t<decltype(v)>, std::string>) {
                    out += (out.empty() ? "" : ", ") + v;
                } else {
                    out += (out.empty() ? "" : ", ") + std::to_string(v) + unit;
                }
            }
            return out;
        };
        const auto length = [&] {
            return std::uniform_int_distribution<std::size_t>(1, 4)(random);
        };
        std::size_t transitions{};
        for (int n = 0; n < limits::transition_list_cases; ++n) {
            Lists l;
            for (std::size_t i = length(); i > 0; --i) {
                l.properties.push_back(pick(names));
            }
            for (std::size_t i = length(); i > 0; --i) {
                l.durations.push_back(pick(durations));
            }
            for (std::size_t i = length(); i > 0; --i) {
                l.delays.push_back(pick(delays));
            }
            const auto longhands = ".t{transition-property:" + join(l.properties, "") +
                                   ";transition-duration:" + join(l.durations, "ms") +
                                   ";transition-delay:" + join(l.delays, "ms") + ";transition-timing-function:linear}";
            // The same transitions as the shorthand: the items paired as the longhands pair them, each item's parts in
            // a random order (the duration before the delay).
            std::string shorthand;
            Lists paired;
            for (std::size_t i = 0; i < l.properties.size(); ++i) {
                paired.properties.push_back(l.properties[i]);
                paired.durations.push_back(l.durations[i % l.durations.size()]);
                paired.delays.push_back(l.delays[i % l.delays.size()]);
                std::vector<std::string> parts{std::to_string(paired.durations.back()) + "ms",
                                               std::to_string(paired.delays.back()) + "ms"};
                parts.insert(parts.begin() + std::ptrdiff_t(random() % 3), l.properties[i]);
                parts.insert(parts.begin() + std::ptrdiff_t(random() % 4), "linear");
                std::string item;
                for (const auto& p : parts) {
                    item += (item.empty() ? "" : " ") + p;
                }
                shorthand += (shorthand.empty() ? "" : ", ") + item;
            }
            for (const auto& sheet : {longhands, ".t{transition:" + shorthand + "}"}) {
                Change change(base + sheet);
                const auto styles = change.samples(times);
                for (std::size_t i = 0; i < times.size(); ++i) {
                    const auto& style = styles[i];
                    const auto at = sheet + " at " + std::to_string(int(times[i])) + "ms: ";
                    const double opacity = model(l, "opacity", times[i]);
                    const double translate = model(l, "transform", times[i]);
                    const double color = model(l, "background-color", times[i]);
                    transitions += opacity < 1 || translate < 1 || color < 1;
                    require(std::abs(number(style.at("opacity")) - opacity) <= limits::transition_model_tolerance,
                            at + "opacity " + style.at("opacity"));
                    require(std::abs(translate_x(style.at("transform")) / 100 - translate) <=
                                limits::transition_model_tolerance,
                            at + "transform " + style.at("transform"));
                    // #204080 to #e0c010: the red channel goes from 32 to 224.
                    require(std::abs((channels(style.at("background-color"))[0] - 32) / 192 - color) <=
                                limits::transition_model_tolerance,
                            at + "background-color " + style.at("background-color"));
                }
            }
        }
        require(transitions > std::size_t(limits::transition_list_cases), "the random lists ran transitions");
        std::cout << limits::transition_list_cases << " random lists as longhands and as the shorthand: " << transitions
                  << " samples mid-transition\n";
    }

    // The diagnostic f throws, or none.
    std::string diagnostic(const std::function<void()>& f) {
        try {
            f();
        } catch (const weft::DiagnosticError& e) {
            return e.what();
        }
        return {};
    }

    void shorthand() {
        // The longhands of the shorthand, from a stylesheet and from the style attribute.
        for (const auto& [sheet, style] : std::vector<std::pair<std::string, std::string>>{
                 {".t{transition:background-color 0.15s, transform 0.1s}", ""},
                 {"", "transition:background-color 0.15s, transform 0.1s"}}) {
            Change change(base + sheet, style);
            const auto s = change.frame(1);
            require(s.at("transition-property") == "background-color, transform" &&
                        s.at("transition-duration") == "0.15s, 0.1s" &&
                        s.at("transition-timing-function") == "ease, ease" && s.at("transition-delay") == "0s, 0s",
                    "the shorthand's longhands " + s.at("transition-property") + "; " + s.at("transition-duration") +
                        "; " + s.at("transition-timing-function") + "; " + s.at("transition-delay"));
        }
        // A later longhand overrides its list; an important shorthand wins over it.
        {
            Change change(base + ".t{transition:opacity 1s linear;transition-duration:2s}");
            const auto s = change.frame(1);
            require(s.at("transition-duration") == "2s" && s.at("transition-property") == "opacity",
                    "a longhand after the shorthand");
            Change important(base + ".t{transition:opacity 1s linear !important;transition-duration:2s}");
            require(important.frame(1).at("transition-duration") == "1s", "an important shorthand");
        }
        // inherit takes the parent's lists, initial the initial values.
        {
            weft::testing::NativeScene s;
            auto& parent = s.html("div");
            auto& child = s.html("div");
            s.set(parent, "style", "transition:opacity 300ms ease-in 50ms");
            s.set(child, "style", "transition:inherit");
            s.tree.insert(*s.root, parent, nullptr);
            s.tree.insert(parent, child, nullptr);
            auto& initial = s.html("div");
            s.set(initial, "style", "transition:opacity 1s;transition:initial");
            s.tree.insert(*s.root, initial, nullptr);
            weft::SceneSolver solver("", "", weft::testing::thread_scene_text());
            const auto f =
                solver.frame(s.tree.snapshot(*s.root), limits::transition_view_size, limits::transition_view_size, 0);
            const auto& c = f.root.children[0].children[0].style;
            require(c.at("transition-property") == "opacity" && c.at("transition-duration") == "300ms" &&
                        c.at("transition-timing-function") == "ease-in" && c.at("transition-delay") == "50ms",
                    "transition: inherit");
            const auto& i = f.root.children[1].style;
            require(i.at("transition-property") == "all" && i.at("transition-duration") == "0s" &&
                        i.at("transition-timing-function") == "ease" && i.at("transition-delay") == "0s",
                    "transition: initial");
        }
        // Outside the profile at load: the shorthand's errors and the transition-behavior longhand.
        for (const auto& [value, why] : std::vector<std::pair<std::string, std::string>>{
                 {"transition:opacity 1s 2s 3s", "more than a duration and a delay"},
                 {"transition:opacity 1s allow-discrete", "allow-discrete outside the scene profile"},
                 {"transition:none, opacity 1s", "none in a list of transitions"},
                 {"transition:opacity 0", "0 is no property name, time, timing function or transition behavior"},
                 {"transition:opacity #fff 1s", "#fff is no property name"},
                 {"transition:opacity -1s", "a negative duration"},
                 {"transition:opacity linear ease 1s", "two timing functions"},
                 {"transition:opacity transform 1s", "two properties"},
                 {"transition:opacity 1s normal normal", "two transition behaviors"},
                 {"transition:opacity 1s,,transform 1s", "an empty transition"},
                 {"transition:opacity 1s, inherit", "inherit is no property name"},
                 {"transition-behavior:allow-discrete", "unsupported property transition-behavior"},
             }) {
            const auto found = diagnostic([&] {
                weft::SceneSolver solver(".t{" + value + "}", "", weft::testing::thread_scene_text());
            });
            require(found.starts_with("WEFT_STYLE_PROFILE") && found.find(why) != std::string::npos,
                    value + ": " + (found.empty() ? "loaded" : found));
        }
        // normal is the initial behavior; var() is checked when it is substituted.
        for (const auto* value : {"transition:opacity 1s normal", "transition:opacity var(--t, 1s 2s 3s)"}) {
            require(diagnostic([&] {
                        weft::SceneSolver solver(std::string(".t{") + value + "}", "",
                                                 weft::testing::thread_scene_text());
                    }).empty(),
                    std::string(value) + " loads");
        }
        // The longhands' items outside the profile: the element's error once its style changes.
        for (const auto& [sheet, why] : std::vector<std::pair<std::string, std::string>>{
                 {".t{transition-property:opacity;transition-duration:-1s}", "a negative duration -1s"},
                 {".t{transition-property:opacity, none;transition-duration:1s}", "none in a list of properties"},
                 {".t{transition:opacity 1s step-start}", "timing function step-start"},
                 {".t{transition-duration:1s;transition-timing-function:linear(0, 1)}", "timing function linear(0, 1)"},
             }) {
            const auto found = diagnostic([&] {
                Change change(base + sheet);
                change.samples({0});
            });
            require(found.starts_with("WEFT_SCENE_PROFILE") && found.find(why) != std::string::npos,
                    sheet + ": " + (found.empty() ? "no diagnostic" : found));
        }
    }

    // A running transition ends (its property shows the target) when transition-property no longer names it, when its
    // property changes again without starting a transition, or when its property leaves the target.
    void ending() {
        const auto opacity = [](const weft::SceneStyle& s) {
            return number(s.at("opacity"));
        };
        const auto sheet = base + ".t{transition:opacity 400ms linear}.t.on.other{transition-property:transform}"
                                  ".t.on.instant{opacity:.5;transition-duration:0s}";
        {
            Change change(sheet);
            const auto mid = change.samples({0, 100});
            require(std::abs(opacity(mid[1]) - .25) < 1e-6, "the transition runs");
            change.set_class("t on other");
            require(opacity(change.frame(limits::transition_change_ms + 150)) == 1,
                    "a transition transition-property no longer names ends");
        }
        {
            Change change(sheet);
            change.samples({0, 100});
            change.set_class("t on instant");
            require(opacity(change.frame(limits::transition_change_ms + 150)) == .5 &&
                        opacity(change.frame(limits::transition_change_ms + 300)) == .5,
                    "a change that starts no transition ends the running one");
        }
        {
            // While it runs, the property's declaration leaves the element.
            Change change(
                ".t{width:40px;height:20px;transition:opacity 400ms linear}.t.on{opacity:1}.t.from{opacity:0}");
            change.set_class("t from");
            change.frame(500);
            change.set_class("t on");
            change.frame(600);
            require(std::abs(opacity(change.frame(700)) - .25) < 1e-6, "a transition between declared values");
            change.set_class("t");
            const auto s = change.frame(750);
            require(!s.contains("opacity"), "a transition whose property left the target ends");
        }
        {
            // A transition ends when it reaches its end; a change back starts from there.
            Change change(sheet);
            const auto at = change.samples({0, 100, 400});
            require(opacity(at[2]) == 1, "the end value at the end");
            change.set_class("t");
            const auto back = change.frame(limits::transition_change_ms + 500);
            require(opacity(back) == 1, "a change back starts from the end value");
            require(std::abs(opacity(change.frame(limits::transition_change_ms + 600)) - .75) < 1e-6,
                    "and runs for its own duration");
        }
    }

    // A shorthand in transition-property starts the transitions of its longhands (CSS Transitions 1 §2.1): Tailwind's
    // transition-colors names border-color, and the scene has only the sides' colors.
    void shorthand_names() {
        for (const std::string name : {"border-color", "border", "border-top"}) {
            Change change(".t{width:40px;height:20px;border:2px solid #204080;transition-property:" + name +
                          ";transition-duration:100ms;transition-timing-function:linear}.t.on{border-color:#e0c010}");
            const auto at = change.samples({0, 50, 200});
            // #204080 to #e0c010: the red channel goes from 32 to 224.
            const auto red = [&](std::size_t i) {
                return channels(at[i].at("border-top-color"))[0];
            };
            require(std::abs(red(1) - 128) < 2,
                    name + " transitions border-top-color: " + at[1].at("border-top-color"));
            require(std::abs(red(2) - 224) < 1, name + " ends at the target: " + at[2].at("border-top-color"));
        }
        Change other(".t{width:40px;height:20px;border:2px solid #204080;transition-property:border-width;"
                     "transition-duration:100ms}.t.on{border-color:#e0c010}");
        const auto at = other.samples({0, 50});
        require(std::abs(channels(at[1].at("border-top-color"))[0] - 224) < 1,
                "border-width does not transition the colors");
    }

    // visibility is visible between its ends when either end is (CSS Display 3 §4): what fades in shows from the start
    // of its fade, what fades out until the end of it; Tailwind's transition names visibility with opacity.
    void visibility() {
        const auto sheet = ".t{width:40px;height:20px;opacity:0;visibility:hidden;"
                           "transition:opacity 100ms linear, visibility 100ms linear}"
                           ".t.on{opacity:1;visibility:visible}";
        Change in(sheet);
        auto at = in.samples({0, 10, 90, 200});
        require(at[0].at("visibility") == "hidden" && at[1].at("visibility") == "visible" &&
                    at[2].at("visibility") == "visible" && at[3].at("visibility") == "visible",
                "fading in, visible from the start: " + at[1].at("visibility"));
        in.set_class("t");
        std::vector<std::string> out;
        for (const double t : {0., 10., 90., 200.}) {
            out.push_back(in.frame(limits::transition_change_ms + 200 + t).at("visibility"));
        }
        require(out[0] == "visible" && out[1] == "visible" && out[2] == "visible" && out[3] == "hidden",
                "fading out, visible until the end: " + out[2] + ", then " + out[3]);
    }
} // namespace

int main() {
    try {
        shorthand();
        ending();
        shorthand_names();
        visibility();
        chrome();
        random_lists();
        std::cout << "scene transitions: passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
