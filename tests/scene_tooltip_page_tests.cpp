#define NOMINMAX
#include "../probes/gallery_cluster_input.hpp"
#include "../probes/gallery_effects.hpp"
#include "../probes/gallery_scene_session.hpp"
#include "scene_test_image.hpp"
#include "support/check.hpp"
#include "support/d3d11_scene.hpp"
#include "support/files.hpp"
#include "support/json_value.hpp"
#include "style/style.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// The CRPG UI's nested tooltip page (11.3d) as a scene: the captured component under the gallery harness
// (tools/build_tooltip_page.mjs), the whole page with html as the scene's root so the tooltips the page portals to body
// show, its own stylesheet (tooltip-utilities.css) and the smoke as the crpg-smoke effect. Hovering a keyword opens its
// tooltip on body beside it; a keyword inside the tooltip opens the next; holding still for 3 s pins a window inside
// #root, which moves when dragged by its title; the native pass draws the smoke behind the item cards and the tooltips
// over the page. Then the acceptance against Chrome (tools/browser_tooltip_page_reference.mjs,
// tests/fixtures/tooltip-reference): the case's frames and inputs (examples/crpg-gallery/tooltip-cases.json), and at
// each capture every element's rect and painted style, the native pass's pixels and each smoke where only it shows
// (Chrome draws the original canvas).
namespace {
    using weft::testing::require;

    constexpr float width = 1596;
    constexpr float height = 800;
    constexpr double frame_ms = 1000.0 / 60;
    // Chrome converts the page's oklch colors to 8-bit sRGB as the style compiler does; reading a translucent color
    // back from its canvas rounds the channels.
    constexpr double max_color_difference = 2;
    // Chrome lays out in 1/64 px.
    constexpr double max_rect_difference = 1.0 / 64;
    // Where a smoke shows: its element less the border's and the rounded corner's pixels, and the pixels around what
    // is drawn over it (antialiasing; a shadow-lg panel's shadow reaches 22 px below it and 12 px to its sides).
    constexpr int smoke_border = 2;
    constexpr int smoke_corner = 10;
    constexpr int content_margin = 2;
    constexpr int shadow_reach = 24;
    // A smoke shows over at least this many pixels and matches Chrome's canvas to a level (the upscaled canvas's
    // rounding) at all but this share of them (at most 0.05% here). One 66 ms tick of the smoke's clock moves a third
    // to a half of them by more.
    constexpr std::size_t min_smoke_pixels = 1000;
    constexpr int smoke_level_tolerance = 1;
    constexpr double max_smoke_off_share = .01;

    struct Rect {
        float x{};
        float y{};
        float w{};
        float h{};
    };

    Rect rect(GallerySceneSession& s, const std::string& expression) {
        std::istringstream in(s.vm.eval("(()=>{const r=(" + expression +
                                        ").getBoundingClientRect();return r.x+' '+r.y+' '+r.width+' '+r.height})()"));
        Rect r;
        in >> r.x >> r.y >> r.w >> r.h;
        return r;
    }

    void run(GallerySceneSession& s, double until) {
        while (s.time + frame_ms <= until + 1e-6) {
            s.step(s.time + frame_ms);
        }
    }

    std::size_t effects(const weft::SceneNode& n) {
        std::size_t count = n.effect ? 1 : 0;
        for (const auto& c : n.children) {
            count += effects(c);
        }
        return count;
    }

    using Premultiplied = std::array<double, 4>;

    // A computed color premultiplied, in 8-bit units: ours from its text (empty is the property's initial value),
    // Chrome's from the straight bytes a canvas drew it with.
    Premultiplied premultiplied(const std::string& text, const Premultiplied& initial, const Premultiplied& current) {
        if (text.empty()) {
            return initial;
        }
        if (text == "currentcolor") {
            return current;
        }
        const auto c = weft::parse_css_color(text);
        return {c[0] * c[3] * 255, c[1] * c[3] * 255, c[2] * c[3] * 255, c[3] * 255};
    }

    Premultiplied premultiplied(const weft::testing::JsonValue& bytes) {
        const auto a = bytes[3].number;
        return {bytes[0].number * a / 255, bytes[1].number * a / 255, bytes[2].number * a / 255, a};
    }

    bool alike(const Premultiplied& a, const Premultiplied& b) {
        for (std::size_t k = 0; k < 4; ++k) {
            if (std::abs(a[k] - b[k]) > max_color_difference) {
                return false;
            }
        }
        return true;
    }

    // A number of a computed text: a percentage as a fraction, a border width keyword in px.
    double number(const std::string& text, double initial) {
        if (text.empty()) {
            return initial;
        }
        if (text == "thin" || text == "medium" || text == "thick") {
            return text == "thin" ? 1 : text == "medium" ? 3 : 5;
        }
        if (text == "normal" || text == "bold") {
            return text == "normal" ? 400 : 700;
        }
        return text.ends_with('%') ? std::stod(text) / 100 : std::stod(text);
    }

    // Each element's computed style that paints, against Chrome's (examples/crpg-gallery's sheet decides it there):
    // background color, color, opacity, font weight and style, and each side's border width, style and color where
    // a border is drawn. Returns the number of elements that differ, the first few shown.
    std::size_t compare_styles(GallerySceneSession& s, const weft::testing::JsonValue& chrome,
                               const std::string& name) {
        const auto styles = weft::testing::parse_json(s.vm.eval(
            "JSON.stringify([...document.body.querySelectorAll('*')].filter(n=>!n.closest('defs')&&n.localName!=="
            "'script').map(n=>{const s=getComputedStyle(n);return [s.backgroundColor,s.color,s.opacity,s.fontWeight,"
            "s.fontStyle,...['top','right','bottom','left'].flatMap(k=>['width','style','color'].map(p=>"
            "s.getPropertyValue('border-'+k+'-'+p)))]}))"));
        require(styles.size() == chrome.size(), name + ": a style for each of Chrome's elements");
        constexpr std::array<const char*, 4> sides{"top", "right", "bottom", "left"};
        std::size_t mismatched{};
        for (std::size_t i = 0; i < chrome.size(); ++i) {
            const auto& theirs = chrome[i]["style"];
            const auto& ours = styles[i];
            std::string wrong;
            const auto color = premultiplied(ours[1].string, {0, 0, 0, 255}, {0, 0, 0, 255});
            if (!alike(premultiplied(ours[0].string, {}, color), premultiplied(theirs["background"]))) {
                wrong += " background-color '" + ours[0].string + "'";
            }
            if (!alike(color, premultiplied(theirs["color"]))) {
                wrong += " color '" + ours[1].string + "'";
            }
            if (std::abs(number(ours[2].string, 1) - theirs["opacity"].number) > 1e-3) {
                wrong += " opacity '" + ours[2].string + "'";
            }
            if (number(ours[3].string, 400) != theirs["font_weight"].number) {
                wrong += " font-weight '" + ours[3].string + "'";
            }
            if ((ours[4].string.empty() ? "normal" : ours[4].string) != theirs["font_style"].string) {
                wrong += " font-style '" + ours[4].string + "'";
            }
            for (std::size_t k = 0; k < sides.size(); ++k) {
                const auto& style = ours[6 + 3 * k].string;
                const auto drawn =
                    style.empty() || style == "none" || style == "hidden" ? 0 : number(ours[5 + 3 * k].string, 3);
                if (std::abs(drawn - theirs["border_widths"][k].number) > 1e-3) {
                    wrong +=
                        std::string(" border-") + sides[k] + "-width '" + ours[5 + 3 * k].string + "' '" + style + "'";
                } else if (drawn > 0 && (style != theirs["border_styles"][k].string ||
                                         !alike(premultiplied(ours[7 + 3 * k].string, color, color),
                                                premultiplied(theirs["border_colors"][k])))) {
                    wrong += std::string(" border-") + sides[k] + " '" + style + "' '" + ours[7 + 3 * k].string + "'";
                }
            }
            if (!wrong.empty() && mismatched++ < 8) {
                std::cout << "  " << name << " element " << i << " <" << chrome[i]["tag"].string << ">:" << wrong
                          << '\n';
            }
        }
        return mismatched;
    }

    struct Box {
        int x0{};
        int y0{};
        int x1{};
        int y1{};

        bool holds(int x, int y) const {
            return x >= x0 && x < x1 && y >= y0 && y < y1;
        }
    };

    Box box(const weft::testing::JsonValue& r, int grow) {
        return {int(std::round(r[0].number)) - grow, int(std::round(r[1].number)) - grow,
                int(std::round(r[0].number + r[2].number)) + grow, int(std::round(r[1].number + r[3].number)) + grow};
    }

    // Each smoke against Chrome's canvas, frame by frame, where only the smoke shows: the smoke's element less its
    // border and rounded corners, what is drawn over it (each later element, in tree order, with text of its own, no
    // children or an svg viewport, and the drawn border sides of the others, with the antialiasing around them) and
    // the floating panels it is not in (the tooltips on body and the pinned windows, with their shadows). Returns the
    // number of smokes that differ, each one shown.
    std::size_t compare_smoke(GallerySceneSession& s, const weft::testing::JsonValue& chrome,
                              const weft::SceneBitmap& ours, const weft::SceneBitmap& theirs, const std::string& name) {
        const auto smokes = weft::testing::parse_json(s.vm.eval(
            "(()=>{const all=[...document.body.querySelectorAll('*')].filter(n=>!n.closest('defs')&&n.localName!=="
            "'script'),rect=n=>{const r=n.getBoundingClientRect();return [r.x,r.y,r.width,r.height]},paints=n=>!n."
            "children.length||n.localName==='svg'||[...n.childNodes].some(c=>c.nodeType===3&&c.textContent.trim()),"
            "sides=n=>{const s=getComputedStyle(n),[x,y,w,h]=rect(n),v=k=>/^(none|hidden|)$/.test(s.getPropertyValue("
            "'border-'+k+'-style'))?0:parseFloat(s.getPropertyValue('border-'+k+'-width'))||0,t=v('top'),r=v('right'),"
            "b=v('bottom'),l=v('left');return [[x,y,w,t],[x+w-r,y,r,h],[x,y+h-b,w,b],[x,y,l,h]].filter(e=>e[2]>0&&e[3]>"
            "0)},floating=[...[...document.body.children].slice(1),...document.querySelectorAll('[data-pinned-tooltip]')"
            "];return JSON.stringify(all.map((n,i)=>[n,i]).filter(([n])=>getComputedStyle(n).backgroundImage.startsWith("
            "'-weft-effect')).map(([n,i])=>({index:i,rect:rect(n),content:all.slice(i+1).flatMap(c=>paints(c)?[rect(c)]"
            ":sides(c)),floating:floating.filter(f=>!f.contains(n)).map(rect)})))})()"));
        require(smokes.size() >= 3, name + ": the item cards' smoke");
        std::size_t mismatched{};
        for (const auto& smoke : smokes.array) {
            require(chrome[std::size_t(smoke["index"].number)]["tag"].string == "canvas",
                    name + ": each smoke a canvas in Chrome");
            auto area = box(smoke["rect"], -smoke_border);
            std::vector<Box> covered;
            for (const auto& r : smoke["content"].array) {
                covered.push_back(box(r, content_margin));
            }
            for (const auto& r : smoke["floating"].array) {
                covered.push_back(box(r, shadow_reach));
            }
            double total{};
            std::size_t pixels{};
            std::size_t off{};
            for (int y = std::max(area.y0, 0); y < std::min(area.y1, int(height)); ++y) {
                for (int x = std::max(area.x0, 0); x < std::min(area.x1, int(width)); ++x) {
                    const bool corner = (x < area.x0 + smoke_corner || x >= area.x1 - smoke_corner) &&
                                        (y < area.y0 + smoke_corner || y >= area.y1 - smoke_corner);
                    if (corner || std::any_of(covered.begin(), covered.end(), [&](const Box& b) {
                            return b.holds(x, y);
                        })) {
                        continue;
                    }
                    const auto i = (std::size_t(y) * std::size_t(width) + std::size_t(x)) * 4;
                    int worst{};
                    for (std::size_t c = 0; c < 3; ++c) {
                        const auto d = std::abs(int(ours.rgba[i + c]) - int(theirs.rgba[i + c]));
                        total += d;
                        worst = std::max(worst, d);
                    }
                    off += worst > smoke_level_tolerance;
                    ++pixels;
                }
            }
            const auto mean = pixels ? total / (3 * pixels) : 0;
            const auto share = pixels ? double(off) / pixels : 1;
            std::cout << "  " << name << " smoke " << smoke["index"].number << ": " << pixels << " px, mean " << mean
                      << ", off by more than a level " << share * 100 << "%\n";
            if (pixels < min_smoke_pixels || share > max_smoke_off_share) {
                ++mismatched;
            }
        }
        return mismatched;
    }
} // namespace

// The page as a whole document, html the scene's root: body over the gallery's backdrop, as the Chrome reference's
// page has it.
GallerySceneOptions tooltip_options() {
    return {"tooltip-utilities.css", "TOOLTIP_TESTS.js", "html",
            "document.body.setAttribute('style','margin:0;position:relative;overflow:hidden;"
            "background-color:#050403')",
            &probes::gallery_effects()};
}

// The frames and inputs of the case, compared with Chrome at its captures.
void replay(const std::filesystem::path& out) {
    const auto directory = weft::testing::source_path("tests/fixtures/tooltip-reference");
    const auto report = weft::testing::parse_json(weft::testing::read_file(directory / "report.json"));
    const auto spec = scene_read(WEFT_SOURCE_DIR "/examples/crpg-gallery/tooltip-cases.json");
    const auto id = report["id"].string;
    GallerySceneSession s("TOOLTIP_TESTS", 0, width, height, tooltip_options());
    s.vm.eval("globalThis.__ceClusterSpec=" + spec + ";globalThis.__ceClusterInputs=__ceClusterSpec.inputs['" + id +
              "']||[]");
    weft::testing::D3D11Scene d3d11{unsigned(width), unsigned(height)};
    std::size_t compared{};
    const auto& captures = report["captures"].array;
    std::size_t next{};
    for (unsigned frame = 0; frame < unsigned(report["frames"].number) && next < captures.size(); ++frame) {
        s.step(frame * frame_ms);
        gallery_cluster_inputs(s, frame);
        if (unsigned(captures[next]["frame"].number) != frame) {
            continue;
        }
        ++next;
        const auto name = id + "-" + std::to_string(frame);
        // Every element's rect, in Chrome's order (body's elements in tree order; the smoke is a canvas there).
        const auto chrome_geometry =
            weft::testing::parse_json(weft::testing::read_file(directory / (name + "-geometry.json")));
        std::istringstream ours(s.vm.eval(
            "[...document.body.querySelectorAll('*')].filter(n=>!n.closest('defs')&&n.localName!=='script')"
            ".map(n=>{const r=n.getBoundingClientRect();return [r.x,r.y,r.width,r.height].join(' ')}).join(' ')"));
        const auto& elements = chrome_geometry.array;
        double worst{};
        std::size_t shown{};
        for (std::size_t i = 0; i < elements.size(); ++i) {
            const auto& e = elements[i];
            std::array<double, 4> r{};
            double off{};
            for (std::size_t k = 0; k < 4; ++k) {
                ours >> r[k];
                off = std::max(off, std::abs(r[k] - e["rect"][k].number));
            }
            worst = std::max(worst, off);
            if (off > 0.25 && shown++ < 6) {
                std::cout << "  " << name << " element " << i << " <" << e["tag"].string << ">: (" << r[0] << ", "
                          << r[1] << ", " << r[2] << ", " << r[3] << "), Chrome (" << e["rect"][0].number << ", "
                          << e["rect"][1].number << ", " << e["rect"][2].number << ", " << e["rect"][3].number << ")\n";
            }
        }
        require(bool(ours), name + ": an element for each of Chrome's");
        const auto mismatched = compare_styles(s, chrome_geometry, name);
        weft::testing::write_file(
            out / (name + "-geometry.json"),
            s.vm.eval("JSON.stringify([...document.body.querySelectorAll('*')].filter(n=>!n.closest('defs')&&"
                      "n.localName!=='script').map(n=>{const r=n.getBoundingClientRect();return {tag:n.localName,"
                      "rect:[r.x,r.y,r.width,r.height]}}))"));
        // The native pass against Chrome's screenshot.
        const auto bgra = d3d11.draw(s.current);
        weft::SceneBitmap image{unsigned(width), unsigned(height), bgra};
        for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
            std::swap(image.rgba[i], image.rgba[i + 2]);
        }
        png(out / (name + ".png"), image);
        const auto reference = scene_reference(directory / (name + ".png"));
        const auto d = scene_difference(image, reference);
        const auto smoky = compare_smoke(s, chrome_geometry, image, reference, name);
        std::cout << name << ": " << elements.size() << " elements, rects within " << worst << " px, " << mismatched
                  << " styled unlike Chrome; pixel means " << d.mean[0] << ' ' << d.mean[1] << ' ' << d.mean[2]
                  << ", soft IoU " << d.soft_iou << "; " << smoky << " smokes unlike Chrome's\n";
        require(worst <= max_rect_difference, name + ": every element's rect as Chrome's");
        require(mismatched == 0, name + ": every element's painted style as Chrome's");
        require(d.passes(), name + ": the native pass as Chrome's screenshot");
        require(smoky == 0, name + ": each smoke as Chrome's canvas");
        ++compared;
    }
    d3d11.gpu.check_debug_messages();
    require(compared == captures.size(), "every capture compared");
}

int main(int argc, char** argv) {
    try {
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM init");
        GallerySceneSession s("TOOLTIP_TESTS", 0, width, height, tooltip_options());
        run(s, 500);
        require(s.vm.eval("document.querySelectorAll('[data-keyword-span]').length") != "0", "keywords on the page");
        std::cout << "smoke effects " << effects(s.current.root) << ", keywords "
                  << s.vm.eval("document.querySelectorAll('[data-keyword-span]').length") << '\n';
        require(effects(s.current.root) == 3, "the three item cards' smoke as effects");

        // Hover the first keyword: its tooltip opens on body, beside it.
        const auto keyword = rect(s, "document.querySelectorAll('[data-keyword-span]')[0]");
        s.input_value(weft::MouseType::Move, keyword.x + keyword.w / 2, keyword.y + keyword.h / 2);
        run(s, s.time + 200);
        require(s.vm.eval("document.body.children.length") == "2", "the tooltip portals to body");
        const auto tooltip = rect(s, "document.body.children[1]");
        std::cout << "keyword (" << keyword.x << ", " << keyword.y << ", " << keyword.w << ", " << keyword.h
                  << "), tooltip (" << tooltip.x << ", " << tooltip.y << ", " << tooltip.w << ", " << tooltip.h
                  << ")\n";
        require(std::abs(tooltip.x - (keyword.x + keyword.w + 8)) < 1 && std::abs(tooltip.y - (keyword.y - 4)) < 1 &&
                    tooltip.w == 256 && tooltip.h > 40,
                "the tooltip fixed beside its keyword, 16 rem wide");

        // The native pass: the tooltip's panel over the page, the smoke behind a card.
        weft::testing::D3D11Scene d3d11{unsigned(width), unsigned(height)};
        const auto native = d3d11.draw(s.current);
        d3d11.gpu.check_debug_messages();
        const auto at = [&](float x, float y) {
            const auto* p = native.data() + (std::size_t(y) * std::size_t(width) + std::size_t(x)) * 4;
            return std::array<int, 3>{p[2], p[1], p[0]};
        };
        const auto inside = at(tooltip.x + tooltip.w / 2, tooltip.y + 4);
        std::cout << "tooltip pixel " << inside[0] << ' ' << inside[1] << ' ' << inside[2] << '\n';
        require(at(4, 4) == std::array<int, 3>{5, 4, 3}, "the page over the gallery's backdrop on body");
        require(inside != std::array<int, 3>{5, 4, 3}, "the native pass draws the tooltip");

        // Into the tooltip and onto a keyword inside it: the next tooltip opens.
        const auto nested = rect(s, "document.body.children[1].querySelector('[data-keyword-span]')");
        s.input_value(weft::MouseType::Move, tooltip.x + tooltip.w / 2, tooltip.y + 10);
        run(s, s.time + 100);
        s.input_value(weft::MouseType::Move, nested.x + nested.w / 2, nested.y + nested.h / 2);
        run(s, s.time + 200);
        require(s.vm.eval("document.body.children.length") == "3", "a nested tooltip opens on body");

        // Holding still for 3 s pins the nested keyword as a window inside #root.
        run(s, s.time + 3100);
        require(s.vm.eval("document.querySelectorAll('[data-pinned-tooltip]').length") == "1", "a pinned window");
        const auto pinned = rect(s, "document.querySelector('[data-pinned-tooltip]')");
        const auto handle = rect(s, "document.querySelector('[data-pinned-tooltip] .cursor-grab')");
        std::cout << "pinned (" << pinned.x << ", " << pinned.y << ", " << pinned.w << ", " << pinned.h << ")\n";

        // Dragged by its title, it moves with the pointer.
        const float hx = handle.x + handle.w / 2;
        const float hy = handle.y + handle.h / 2;
        s.input_value(weft::MouseType::Down, hx, hy, 1);
        run(s, s.time + 50);
        s.input_value(weft::MouseType::Move, hx + 60, hy + 30, 1);
        run(s, s.time + 50);
        s.input_value(weft::MouseType::Move, hx + 120, hy + 70, 1);
        run(s, s.time + 50);
        s.input_value(weft::MouseType::Up, hx + 120, hy + 70);
        run(s, s.time + 50);
        const auto moved = rect(s, "document.querySelector('[data-pinned-tooltip]')");
        std::cout << "moved (" << moved.x << ", " << moved.y << ")\n";
        require(std::abs(moved.x - pinned.x - 120) < 0.5 && std::abs(moved.y - pinned.y - 70) < 0.5,
                "the pinned window follows the drag");
        const auto out = std::filesystem::path(argc > 1 ? argv[1] : "artifacts/tooltip-page");
        std::filesystem::create_directories(out);
        replay(out);
        std::cout << "PASS the nested tooltip page as a scene: portals, nested tooltips, pin, drag, native pass\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
