#include "base/diagnostic.hpp"
#include "scene/scene.hpp"
#include "support/check.hpp"
#include "support/d3d11_scene.hpp"
#include "support/files.hpp"
#include "support/json_value.hpp"
#include "support/native_scene.hpp"
#include "support/png.hpp"
#include "support/scene_fonts.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

// Dotted and dashed borders against Chrome's pixels: the cases of tools/browser_scene_border_reference.mjs
// (tests/fixtures/scene-border-reference), absolutely positioned boxes and a keyword span in a line of text, laid out
// by the scene solver and drawn by the reference raster and the native D3D11 pass at device scale 1 and 2, against
// Chrome at those device scale factors. Each box is compared over its snapped border box and two pixels around it,
// the span over its bottom border's rows. A pixel Chrome paints in the background or a border color exactly must come out
// in the same color: square dots and dashes are whole pixels in both. Chrome antialiases round dots its own way, so an
// edge pixel of one may differ by part of its coverage, and a whole dot's ink must agree. Then the forms pages write
// borders in, and what the scene profile refuses.
namespace {
    using weft::testing::JsonValue;
    using weft::testing::require;

    using Rgb = std::array<int, 3>;

    struct Image {
        unsigned width{};
        unsigned height{};
        std::vector<std::uint8_t> rgba;

        Rgb at(unsigned x, unsigned y) const {
            const auto i = (std::size_t(y) * width + x) * 4;
            return {rgba[i], rgba[i + 1], rgba[i + 2]};
        }
    };

    Rgb hex(std::string_view s) {
        const auto pos = s.find('#');
        require(pos != std::string_view::npos && pos + 7 <= s.size(), "a #rrggbb color");
        const auto v = std::stoi(std::string(s.substr(pos + 1, 6)), nullptr, 16);
        return {(v >> 16) & 255, (v >> 8) & 255, v & 255};
    }

    int distance(const Rgb& a, const Rgb& b) {
        return std::max({std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2])});
    }

    // The colors of a case's style: background, then the border colors it names.
    std::vector<Rgb> colors(const std::string& style, const Rgb& page) {
        std::vector<Rgb> out{page};
        std::size_t at{};
        while ((at = style.find('#', at)) != std::string::npos) {
            const auto c = hex(style.substr(at, 7));
            if (style.rfind("background-color:", at) != std::string::npos &&
                style.rfind("background-color:", at) + std::string("background-color:").size() == at) {
                out[0] = c;
            } else {
                out.push_back(c);
            }
            at += 7;
        }
        return out;
    }

    struct Score {
        std::size_t pixels{};
        // Chrome's pixel is a pure color of the case; ours is not that color.
        std::size_t crisp_misses{};
        // An antialiased pixel of Chrome's that ours misses by more than half the range.
        std::size_t soft_misses{};
        int worst{};
        double chrome_ink{};
        double native_ink{};
    };

    // Ours against Chrome's over [x0, x1) x [y0, y1) in device pixels.
    Score compare(const Image& chrome, const Image& native, const std::vector<Rgb>& pure, int x0, int y0, int x1,
                  int y1) {
        Score s;
        for (int y = std::max(0, y0); y < std::min(int(chrome.height), y1); ++y) {
            for (int x = std::max(0, x0); x < std::min(int(chrome.width), x1); ++x) {
                const auto c = chrome.at(unsigned(x), unsigned(y));
                const auto n = native.at(unsigned(x), unsigned(y));
                const int d = distance(c, n);
                const bool crisp = std::any_of(pure.begin(), pure.end(), [&](const Rgb& p) {
                    return distance(p, c) <= 4;
                });
                ++s.pixels;
                s.worst = std::max(s.worst, d);
                if (crisp && d > 24) {
                    ++s.crisp_misses;
                } else if (!crisp && d > 128) {
                    ++s.soft_misses;
                }
                s.chrome_ink += distance(c, pure[0]);
                s.native_ink += distance(n, pure[0]);
            }
        }
        return s;
    }

    // Round dots one by one: each of Chrome's dots (its pixels off the background, connected), and the centroid of
    // the ink over those pixels and one around them in both images.
    struct Dots {
        std::size_t count{};
        double worst{};
    };

    Dots dots(const Image& chrome, const Image& native, const Rgb& background, int x0, int y0, int x1, int y1) {
        x0 = std::max(x0, 0);
        y0 = std::max(y0, 0);
        x1 = std::min(x1, int(chrome.width));
        y1 = std::min(y1, int(chrome.height));
        const auto ink = [&](const Image& image, int x, int y) {
            return distance(image.at(unsigned(x), unsigned(y)), background);
        };
        std::vector<bool> seen(std::size_t(x1 - x0) * std::size_t(y1 - y0));
        const auto index = [&](int x, int y) {
            return std::size_t(y - y0) * std::size_t(x1 - x0) + std::size_t(x - x0);
        };
        Dots out;
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                if (seen[index(x, y)] || ink(chrome, x, y) <= 8) {
                    continue;
                }
                std::array<int, 4> bounds{x, y, x, y};
                std::vector<std::array<int, 2>> stack{{x, y}};
                seen[index(x, y)] = true;
                while (!stack.empty()) {
                    const auto [px, py] = stack.back();
                    stack.pop_back();
                    bounds = {std::min(bounds[0], px), std::min(bounds[1], py), std::max(bounds[2], px),
                              std::max(bounds[3], py)};
                    for (const auto& [nx, ny] :
                         std::array<std::array<int, 2>, 4>{{{px + 1, py}, {px - 1, py}, {px, py + 1}, {px, py - 1}}}) {
                        if (nx >= x0 && ny >= y0 && nx < x1 && ny < y1 && !seen[index(nx, ny)] &&
                            ink(chrome, nx, ny) > 8) {
                            seen[index(nx, ny)] = true;
                            stack.push_back({nx, ny});
                        }
                    }
                }
                std::array<double, 3> a{};
                std::array<double, 3> b{};
                for (int dy = std::max(0, bounds[1] - 1); dy <= std::min(int(chrome.height) - 1, bounds[3] + 1); ++dy) {
                    for (int dx = std::max(0, bounds[0] - 1); dx <= std::min(int(chrome.width) - 1, bounds[2] + 1);
                         ++dx) {
                        const double ca = ink(chrome, dx, dy);
                        const double nb = ink(native, dx, dy);
                        a = {a[0] + ca, a[1] + ca * dx, a[2] + ca * dy};
                        b = {b[0] + nb, b[1] + nb * dx, b[2] + nb * dy};
                    }
                }
                const double shift = b[0] > 0 ? std::hypot(a[1] / a[0] - b[1] / b[0], a[2] / a[0] - b[2] / b[0])
                                              : std::numeric_limits<double>::infinity();
                out.worst = std::max(out.worst, shift);
                ++out.count;
            }
        }
        return out;
    }

    Image from_bgra(unsigned width, unsigned height, const std::vector<std::uint8_t>& bgra) {
        Image out{width, height, bgra};
        for (std::size_t i = 0; i < out.rgba.size(); i += 4) {
            std::swap(out.rgba[i], out.rgba[i + 2]);
        }
        return out;
    }

    const weft::SceneNode* find(const weft::SceneNode& n, const std::string& tag) {
        if (n.tag == tag) {
            return &n;
        }
        for (const auto& c : n.children) {
            if (const auto* f = find(c, tag)) {
                return f;
            }
        }
        return nullptr;
    }
} // namespace

int main(int argc, char** argv) {
    try {
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM init");
        const auto out = std::filesystem::path(argc > 1 ? argv[1] : "artifacts/scene-border");
        std::filesystem::create_directories(out);
        const auto directory = weft::testing::source_path("tests/fixtures/scene-border-reference");
        const auto fixture = weft::testing::parse_json(weft::testing::read_file(directory / "cases.json"));
        const unsigned width = unsigned(fixture["width"].number);
        const unsigned height = unsigned(fixture["height"].number);
        const Rgb page{0x10, 0x10, 0x10};
        auto& scene_text = weft::testing::thread_scene_text();

        // The page: the cases absolutely positioned on a dark root, then the line with the keyword span.
        weft::testing::NativeScene s;
        s.set(*s.root, "style", "position:relative;overflow:hidden;background-color:#101010");
        for (const auto& c : fixture["cases"].array) {
            auto& box = s.html("div");
            std::ostringstream style;
            style << "position:absolute;left:" << c["x"].number << "px;top:" << c["y"].number
                  << "px;width:" << c["w"].number << "px;height:" << c["h"].number << "px;" << c["style"].string;
            s.set(box, "style", style.str());
            s.tree.insert(*s.root, box, nullptr);
        }
        const auto& inline_case = fixture["inline"];
        auto& line = s.html("div");
        {
            std::ostringstream style;
            style << "position:absolute;left:" << inline_case["x"].number << "px;top:" << inline_case["y"].number
                  << "px;width:" << inline_case["w"].number << "px;" << inline_case["style"].string;
            s.set(line, "style", style.str());
        }
        s.tree.insert(*s.root, line, nullptr);
        // Deals <span style="...">Fire Damage</span> on hit.
        const auto& html = inline_case["html"].string;
        const auto open = html.find("<span style=\"");
        const auto style_end = html.find("\">", open);
        const auto close = html.find("</span>", style_end);
        require(open != std::string::npos && style_end != std::string::npos && close != std::string::npos,
                "the inline case's span");
        const auto add_text = [&](weft::dom::Node& parent, std::string data) {
            auto& t = s.tree.create(weft::constants::text_node_type, "#text", "", s.document->identity);
            t.data = std::move(data);
            s.tree.insert(parent, t, nullptr);
        };
        add_text(line, html.substr(0, open));
        auto& span = s.html("span");
        const auto span_style = html.substr(open + 13, style_end - open - 13);
        s.set(span, "style", span_style);
        s.tree.insert(line, span, nullptr);
        add_text(span, html.substr(style_end + 2, close - style_end - 2));
        add_text(line, html.substr(close + 7));

        weft::SceneSolver solver("", "", scene_text);
        const auto frame = solver.frame(s.tree.snapshot(*s.root), float(width), float(height), 0);
        const auto* laid_span = find(frame.root, "span");
        require(laid_span && laid_span->fragments.size() == 1, "the keyword span on one line");
        const auto& line_node = frame.root.children.back();
        const auto& fragment = laid_span->fragments[0].rect;
        const std::array<double, 4> span_box{line_node.box[0] + laid_span->box[0] + fragment[0],
                                             line_node.box[1] + laid_span->box[1] + fragment[1], fragment[2],
                                             fragment[3]};
        std::cout << "keyword fragment (" << span_box[0] << ", " << span_box[1] << ", " << span_box[2] << ", "
                  << span_box[3] << ")\n";

        weft::SceneRaster raster(scene_text);
        std::vector<std::string> failures;
        std::size_t compared{};
        for (const unsigned scale : {1u, 2u}) {
            const auto chrome_png = weft::testing::read_png(directory / ("borders-" + std::to_string(scale) + ".png"));
            const Image chrome{chrome_png.width, chrome_png.height, chrome_png.rgba};
            require(chrome.width == width * scale && chrome.height == height * scale, "Chrome's screenshot size");
            const auto reference = raster.raster(frame, float(scale));
            const Image d2d{reference.width, reference.height, reference.rgba};
            weft::testing::D3D11Scene d3d11(width * scale, height * scale);
            const auto native = from_bgra(width * scale, height * scale, d3d11.draw(frame, float(scale)));
            d3d11.gpu.check_debug_messages();
            require(d2d.width == chrome.width && d2d.height == chrome.height, "reference raster size");
            weft::testing::write_png(out / ("reference-" + std::to_string(scale) + ".png"), d2d.width, d2d.height,
                                     d2d.rgba);
            weft::testing::write_png(out / ("native-" + std::to_string(scale) + ".png"), native.width, native.height,
                                     native.rgba);

            const auto check = [&](const std::string& name, const std::vector<Rgb>& pure, int x0, int y0, int x1,
                                   int y1, bool round) {
                for (const auto* which : {"reference", "native"}) {
                    const auto& image = std::string(which) == "native" ? native : d2d;
                    const auto score = compare(chrome, image, pure, x0, y0, x1, y1);
                    const double ink = score.native_ink / std::max(1.0, score.chrome_ink);
                    std::cout << "  scale " << scale << ' ' << std::setw(9) << which << "  " << std::setw(30) << name
                              << ": " << score.pixels << " px, crisp misses " << score.crisp_misses << ", soft misses "
                              << score.soft_misses << ", worst " << score.worst << ", ink " << std::fixed
                              << std::setprecision(3) << ink << std::defaultfloat << '\n';
                    // Round dots: Chrome's edge pixels are its own antialiasing; a pixel of a dot misplaced by one
                    // misses many times over, and each dot's centroid stays within a tenth of a pixel of Chrome's.
                    const std::size_t allowed = round ? score.pixels / 50 : 0;
                    bool placed = true;
                    if (round) {
                        const auto d = dots(chrome, image, pure[0], x0, y0, x1, y1);
                        std::cout << "  " << std::setw(52) << ' ' << d.count << " dots, centroids within " << d.worst
                                  << " px\n";
                        placed = d.count > 0 && d.worst <= .1;
                    }
                    if (score.crisp_misses > allowed || score.soft_misses > 0 || std::abs(ink - 1) > .08 || !placed) {
                        failures.push_back("scale " + std::to_string(scale) + ' ' + which + ' ' + name);
                    }
                    ++compared;
                }
            };
            for (const auto& c : fixture["cases"].array) {
                const auto& style = c["style"].string;
                const double sx = scale;
                const int x0 = int(std::lround(c["x"].number * sx));
                const int y0 = int(std::lround(c["y"].number * sx));
                const int x1 = int(std::lround((c["x"].number + c["w"].number) * sx));
                const int y1 = int(std::lround((c["y"].number + c["h"].number) * sx));
                // Round dots: dotted wider than 3 device px.
                bool round{};
                for (std::size_t at = 0; (at = style.find("dotted", at)) != std::string::npos; ++at) {
                    const auto px = style.rfind("px", at);
                    const auto start = style.find_last_of(":; ", px - 1);
                    const double w = std::stod(style.substr(start + 1, px - start - 1));
                    round = round || std::floor(w * sx) > 3;
                }
                check(c["name"].string, colors(style, page), x0 - 2, y0 - 2, x1 + 2, y1 + 2, round);
            }
            // The keyword's bottom border: its rows under the snapped fragment.
            const int kx0 = int(std::lround(span_box[0] * scale));
            const int kx1 = int(std::lround((span_box[0] + span_box[2]) * scale));
            const int ky1 = int(std::lround((span_box[1] + span_box[3]) * scale));
            check(inline_case["name"].string, {page, hex("#fb923c")}, kx0, ky1 - int(scale), kx1, ky1, false);
        }
        std::cout << compared << " comparisons against " << fixture["browser"].string << "; " << failures.size()
                  << " failures\n";
        for (const auto& f : failures) {
            std::cout << "  " << f << '\n';
        }
        require(failures.empty(), "dotted and dashed borders match Chrome");

        // The forms a page writes borders in. Tailwind's border-b border-dotted (one side's width, every side's style)
        // is border-bottom:1px dotted; a border without a style has none, so it draws nothing.
        const auto laid_out = [&](const std::string& style) {
            weft::testing::NativeScene t;
            auto& b = t.html("div");
            t.set(b, "style", "width:40px;height:10px;" + style);
            t.tree.insert(*t.root, b, nullptr);
            weft::SceneSolver solving("", "", scene_text);
            return solving.frame(t.tree.snapshot(*t.root), 100, 50, 0);
        };
        using P = weft::PropertyId;
        const auto tailwind = laid_out("border-style:dotted;border-bottom-width:1px");
        const auto& tw = tailwind.root.children.at(0).style;
        require(tw.text(P::BorderTopStyle) == "dotted" && tw.text(P::BorderBottomStyle) == "dotted" &&
                    tw.text(P::BorderBottomWidth) == "1px" && tw.text(P::BorderTopWidth) == "0",
                "border-style sets every side's style; border-bottom-width one side's width");
        const auto side = laid_out("border-bottom:1px dotted #d4a843");
        const auto& sd = side.root.children.at(0).style;
        require(sd.text(P::BorderBottomStyle) == "dotted" && sd.text(P::BorderBottomWidth) == "1px" &&
                    sd.text(P::BorderTopStyle) == "solid",
                "border-bottom sets the bottom side only");
        const auto unstyled = laid_out("border:2px #d4a843");
        const auto& un = unstyled.root.children.at(0).style;
        require(un.text(P::BorderTopStyle) == "none" && un.text(P::BorderLeftWidth) == "0",
                "a border without a style has none: no width, nothing drawn");
        const auto mixed = laid_out("border-style:dotted solid;border-width:1px");
        const auto& mx = mixed.root.children.at(0).style;
        require(mx.text(P::BorderTopStyle) == "dotted" && mx.text(P::BorderRightStyle) == "solid" &&
                    mx.text(P::BorderBottomStyle) == "dotted" && mx.text(P::BorderLeftStyle) == "solid",
                "two border styles: top and bottom, then right and left");

        // Outside the profile: diagnostics, not a solid border in their place.
        const auto refused = [&](const std::string& style, const std::string& expected) {
            try {
                laid_out(style);
            } catch (const weft::DiagnosticError& e) {
                require(e.code() == weft::Diag::SceneProfile &&
                            std::string(e.what()).find(expected) != std::string::npos,
                        "diagnostic: " + expected + " (got " + e.what() + ")");
                return;
            }
            require(false, "diagnostic: " + expected + " (none)");
        };
        refused("border:2px dotted #fff;border-radius:4px", "dotted or dashed border with rounded corners");
        refused("border:3px double #fff", "scene border style double outside scene profile");
        refused("border-bottom:1px groove #fff", "scene border style groove outside scene profile");

        // Off the pixel grid (where Chrome would snap the box): the native pass covers each pixel by the share of it
        // that the border box, its border and its padding box take, as a box filter does, corners included. The
        // background does not show under the antialiased outer edge of the border (Chrome avoids that bleed for
        // rounded boxes, the only ones it paints off the grid).
        {
            weft::testing::NativeScene t;
            t.set(*t.root, "style", "position:relative;background-color:#101010");
            auto& b = t.html("div");
            t.set(b, "style",
                  "position:absolute;left:10.6px;top:10.3px;width:30.5px;height:20.25px;border:2px solid #e0e0e0;"
                  "background-color:#3a2a10");
            t.tree.insert(*t.root, b, nullptr);
            weft::SceneSolver solving("", "", scene_text);
            const auto f = solving.frame(t.tree.snapshot(*t.root), 60, 40, 0);
            const Rgb border{0xe0, 0xe0, 0xe0};
            const Rgb inside{0x3a, 0x2a, 0x10};
            for (const unsigned scale : {1u, 2u}) {
                weft::testing::D3D11Scene gpu(60 * scale, 40 * scale);
                const auto drawn = from_bgra(60 * scale, 40 * scale, gpu.draw(f, float(scale)));
                gpu.gpu.check_debug_messages();
                // The share of device pixel (x, y) inside the rectangle from (l, t) to (r, b) in px.
                const auto share = [&](unsigned x, unsigned y, double l, double top, double r, double bottom) {
                    const auto along = [](double p, double lo, double hi) {
                        return std::clamp(std::min(p + 1, hi) - std::max(p, lo), 0.0, 1.0);
                    };
                    return along(x, l * scale, r * scale) * along(y, top * scale, bottom * scale);
                };
                int worst{};
                for (unsigned y = 0; y < 40 * scale; ++y) {
                    for (unsigned x = 0; x < 60 * scale; ++x) {
                        const double outer = share(x, y, 10.6, 10.3, 41.1, 30.55);
                        const double padding = share(x, y, 12.6, 12.3, 39.1, 28.55);
                        Rgb expected{};
                        for (std::size_t c = 0; c < 3; ++c) {
                            expected[c] = int(std::lround(page[c] * (1 - outer) + border[c] * (outer - padding) +
                                                          inside[c] * padding));
                        }
                        worst = std::max(worst, distance(expected, drawn.at(x, y)));
                    }
                }
                std::cout << "off-grid bordered box at scale " << scale << ": native within " << worst
                          << " of its exact shares\n";
                require(worst <= 2, "the native pass covers an off-grid box by its exact shares of each pixel");
            }
        }
        std::cout << "PASS scene dotted and dashed borders against Chrome; border forms; diagnostics; off-grid "
                     "coverage\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
