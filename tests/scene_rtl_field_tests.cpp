#include "scene/scene_field.hpp"
#include "support/check.hpp"
#include "support/files.hpp"
#include "support/json_value.hpp"
#include "view/view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// Text fields of right-to-left and mixed text against Chrome: the fields of tests/fixtures/rtl_field in a View, as
// tools/browser_rtl_field_reference.mjs recorded Chrome's (chrome-results.json). For each field: where its caret stands
// at several offsets (Chrome's from a screenshot: the column of its green caret), where a selection's background lies
// (its runs of blue columns: a selection of mixed text may be in pieces), and where the left and right arrow keys take
// a caret or a selection. Columns agree to a px (Chrome draws a caret one px wide at its stop). Portable: the
// repository's fonts.
namespace {
    using weft::testing::JsonValue;
    using weft::testing::require;

    constexpr double tolerance = 1.01;

    std::shared_ptr<const weft::FontCatalog> catalog(const JsonValue& fonts) {
        auto c = std::make_shared<weft::FontCatalog>();
        for (const auto& f : fonts.array) {
            weft::FontFaceEntry face;
            face.family = f["family"].string;
            face.source.path = std::string(WEFT_SOURCE_DIR "/") + f["path"].string;
            c->faces.push_back(face);
        }
        c->generic_families = {{"sans-serif", "Roboto"}, {"system-ui", "Roboto"}, {"monospace", "Roboto"}};
        return c;
    }

    // The fields in tree order, with their border boxes' left and top in the View.
    void fields(const weft::SceneNode& n, double x, double y,
                std::vector<std::pair<const weft::SceneNode*, std::array<double, 2>>>& out) {
        for (const auto& c : n.children) {
            const double ax = x + c.box[0];
            const double ay = y + c.box[1];
            if (c.field) {
                out.push_back({&c, {ax, ay}});
            }
            fields(c, ax, ay, out);
        }
    }

    std::string list(const std::vector<std::array<double, 2>>& runs) {
        std::ostringstream s;
        for (const auto& r : runs) {
            s << "[" << r[0] << " " << r[1] << "]";
        }
        return s.str();
    }
} // namespace

int main() {
    try {
        const auto fixture = weft::testing::parse_json(
            weft::testing::read_file(WEFT_SOURCE_DIR "/tests/fixtures/rtl_field/chrome-results.json"));
        const auto fonts = catalog(fixture["fonts"]);
        weft::SceneText text(*fonts);
        weft::View view(fonts, text, 800, 600, 1);
        weft::ViewPage page;
        page.scripts.push_back(
            {"field.js", weft::testing::read_file(WEFT_SOURCE_DIR "/tests/fixtures/rtl_field/field.js")});
        page.stylesheet = weft::testing::read_file(WEFT_SOURCE_DIR "/tests/fixtures/rtl_field/field.css");
        page.forgiving_css = true;
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
        frame();
        std::vector<std::string> failures;
        std::size_t carets{};
        std::size_t selections{};
        std::size_t arrows{};
        const auto select = [&](std::size_t f, double a, double b) {
            eval("(() => { const e = document.querySelectorAll('.field')[" + std::to_string(f) +
                 "]; e.focus(); e.setSelectionRange(" + std::to_string(int(a)) + ", " + std::to_string(int(b)) +
                 "); return true; })()");
            frame();
            frame();
        };
        const auto& chrome_fields = fixture["fields"].array;
        for (std::size_t f = 0; f < chrome_fields.size(); ++f) {
            const auto& chrome = chrome_fields[f];
            const auto name = "field " + std::to_string(f);
            // The field's node as the View's scene has it now.
            const auto field = [&] {
                std::vector<std::pair<const weft::SceneNode*, std::array<double, 2>>> found;
                fields(view.scene().root, 0, 0, found);
                require(f < found.size(), name + ": in the scene");
                return found[f];
            };
            const double left = chrome["rect"][0].number;
            for (const auto& c : chrome["carets"].array) {
                select(f, c["offset"].number, c["offset"].number);
                const auto [n, at] = field();
                if (std::getenv("WEFT_FIELD_DEBUG")) {
                    std::cerr << name << " box " << n->box[0] << "," << n->box[2] << " dir "
                              << n->style.text(weft::PropertyId::Direction, "?") << " align "
                              << n->style.text(weft::PropertyId::TextAlign, "?") << " ws "
                              << n->style.text(weft::PropertyId::WhiteSpace, "?") << " lines:";
                    for (const auto& l : n->lines) {
                        std::cerr << " [" << l.text << " x" << l.rect[0] << " w" << l.rect[2] << (l.rtl ? " rtl" : "")
                                  << "]";
                    }
                    std::cerr << "\n";
                }
                const auto& state = *n->field->state;
                const auto caret = weft::scene_field_caret(*n, weft::scene_field_shown(state, state.selection_end));
                require(caret.has_value(), name + ": a caret");
                const double x = at[0] + caret->x - left;
                const auto& columns = c["columns"].array;
                require(columns.size() == 1, name + ": Chrome drew one caret");
                // Chrome's caret column is the px left of its stop when the stop lies at a run's right.
                const double chrome_x = columns[0][0].number;
                if (std::abs(x - chrome_x) > tolerance && std::abs(x - (chrome_x + 1)) > tolerance) {
                    std::ostringstream m;
                    m << name << " caret at " << c["offset"].number << ": native " << x << ", Chrome column "
                      << chrome_x;
                    failures.push_back(m.str());
                }
                ++carets;
            }
            for (const auto& s : chrome["selections"].array) {
                select(f, s["start"].number, s["end"].number);
                const auto [n, at] = field();
                const auto& state = *n->field->state;
                const auto from = weft::scene_field_shown(state, state.selection_start);
                const auto to = weft::scene_field_shown(state, state.selection_end);
                std::vector<std::array<double, 2>> runs;
                for (const auto& l : n->lines) {
                    const auto line_end = l.start + std::uint32_t(l.text.size());
                    const auto begin = std::max(from, l.start);
                    const auto end = std::min<std::uint32_t>(to, line_end);
                    // A line break the selection takes draws as a space at the line's end (its left when right to
                    // left), as the display list draws it.
                    const bool line_break =
                        from <= line_end && to > line_end && line_end < n->text.size() && n->text[line_end] == '\n';
                    if (begin < end) {
                        for (const auto& r : weft::scene_field_ranges(l, begin, end)) {
                            runs.push_back({at[0] + l.rect[0] + r[0] - left, at[0] + l.rect[0] + r[1] - left});
                        }
                    }
                    if (line_break) {
                        const auto stop = l.stops.empty() ? 0.f : l.stops.back().second;
                        const double x = at[0] + l.rect[0] + stop - left;
                        const double width = n->field->newline_width;
                        runs.push_back(l.rtl ? std::array<double, 2>{x - width, x}
                                             : std::array<double, 2>{x, x + width});
                    }
                }
                std::sort(runs.begin(), runs.end());
                std::vector<std::array<double, 2>> merged;
                for (const auto& r : runs) {
                    if (!merged.empty() && r[0] <= merged.back()[1] + 0.5) {
                        merged.back()[1] = std::max(merged.back()[1], r[1]);
                    } else {
                        merged.push_back(r);
                    }
                }
                std::vector<std::array<double, 2>> expected;
                for (const auto& r : s["columns"].array) {
                    expected.push_back({r[0].number, r[1].number});
                }
                const auto what = name + " selection " + std::to_string(int(s["start"].number)) + "-" +
                                  std::to_string(int(s["end"].number));
                bool same = merged.size() == expected.size();
                for (std::size_t k = 0; same && k < merged.size(); ++k) {
                    same = std::abs(merged[k][0] - expected[k][0]) <= tolerance &&
                           std::abs(merged[k][1] - expected[k][1]) <= tolerance;
                }
                if (!same) {
                    failures.push_back(what + ": native " + list(merged) + ", Chrome " + list(expected));
                }
                ++selections;
            }
            for (const auto& a : chrome["arrows"].array) {
                select(f, a["start"].number, a["end"].number);
                const auto& key = a["key"].string;
                weft::KeyInput down;
                down.down = true;
                down.key = key;
                down.code = key;
                view.key(down);
                auto up = down;
                up.down = false;
                view.key(up);
                frame();
                const auto got =
                    eval("(e => e.selectionStart + ',' + e.selectionEnd)(document.querySelectorAll('.field')[" +
                         std::to_string(f) + "])");
                const auto want =
                    std::to_string(int(a["after"][0].number)) + "," + std::to_string(int(a["after"][1].number));
                if (got != want) {
                    failures.push_back(name + " " + key + " from " + std::to_string(int(a["start"].number)) + "-" +
                                       std::to_string(int(a["end"].number)) + ": native " + got + ", Chrome " + want);
                }
                ++arrows;
            }
        }
        std::cout << chrome_fields.size() << " right-to-left fields vs " << fixture["browser"].string << ": " << carets
                  << " carets, " << selections << " selections, " << arrows << " arrow keys; " << failures.size()
                  << " failures\n";
        for (const auto& f : failures) {
            std::cout << "  " << f << '\n';
        }
        require(failures.empty(), "right-to-left text fields match Chrome");
        std::cout << "PASS right-to-left text fields against Chrome\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
