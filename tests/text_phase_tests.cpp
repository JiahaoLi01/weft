#include "constants/text.hpp"
#include "scene/scene.hpp"
#include "support/check.hpp"
#include "support/d3d11_scene.hpp"
#include "support/files.hpp"
#include "support/json_value.hpp"
#include "support/native_scene.hpp"
#include "support/png.hpp"
#include "support/scene_fonts.hpp"
#include "view/view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// Still text on the device's pixels, where Chrome draws it (paint::Raster::still, ScenePreparer, scene_text_image's
// phase). The page of tests/fixtures/text-phase-reference ("H" in Segoe UI 20px at 16 phases of a CSS px: the first
// row moves its top, the second its left) drawn by the native D3D11 pass at device scale 1 and 2 against Chrome's
// screenshots (tools/browser_text_phase_reference.mjs):
// - Chrome puts a glyph's baseline on a whole pixel row and its pen on a quarter of a pixel, each rounded half up: the
//   cells down draw one raster at whole pixel steps, the cells across four a pixel. The native pass's cells are alike
//   (byte for byte, their ink trimmed to its bounds) exactly where Chrome's are, the rows of the cells down step where
//   Chrome's do, and each cell's ink centroid less its row's mean is Chrome's within 0.15 device px (FreeType's raster
//   of a quarter step against DirectWrite's).
// - Only still text goes on the pixels: of three labels at the same fractional place, the one that layout alone
//   puts there, not the one under a transform nor the one in a scrolled container (they move smoothly, drawn between
//   pixels as Chrome draws a moving layer).
// - A frame draws alike whatever the renderer drew before. Still text moving across draws from at most
//   text_phase_steps images of its line (forty frames a tenth of a px apart, and none more the second time), moving
//   down from one.
// The reference raster's cells are alike where Chrome's are too, its still text's baseline on a pixel row (its ink down
// where Chrome's is) and DirectWrite's pens on quarters of a pixel (its glyphs at each quarter drawn apart from Chrome's,
// their centroids listed). Side-by-side pairs (native pass left, Chrome right) and the reference raster's drawing go to
// the output folder.
namespace {
    using weft::testing::require;
    namespace fs = std::filesystem;

    constexpr unsigned cells = 16;
    constexpr double centroid_tolerance = .15;

    // A cell's ink (255 less the green channel; text is black on white): its bounds, the ink inside them, and its
    // centroid in the cell's device px.
    struct Cell {
        int top{};
        int left{};
        std::string ink;
        double x{};
        double y{};
    };

    // The cell i of row (each 40 CSS px square, from 4 px before the cell's place) in an image of 4 bytes a pixel whose
    // green is channel 1.
    Cell cell(const std::uint8_t* pixels, unsigned width, unsigned row, unsigned i, unsigned scale) {
        const int x0 = int((10 + i * 40 - 4) * scale);
        const int y0 = int((10 + row * 60 - 4) * scale);
        const int size = int(40 * scale);
        const auto ink = [&](int x, int y) {
            return std::uint8_t(255 - pixels[(std::size_t(y0 + y) * width + std::size_t(x0 + x)) * 4 + 1]);
        };
        int top = size;
        int bottom = -1;
        int left = size;
        int right = -1;
        double sum{};
        double sx{};
        double sy{};
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                if (const auto v = ink(x, y)) {
                    top = std::min(top, y);
                    bottom = std::max(bottom, y);
                    left = std::min(left, x);
                    right = std::max(right, x);
                    sum += v;
                    sx += v * (x + .5);
                    sy += v * (y + .5);
                }
            }
        }
        require(bottom >= 0, "ink in cell " + std::to_string(i) + " of row " + std::to_string(row));
        Cell c{top, left, {}, sx / sum, sy / sum};
        for (int y = top; y <= bottom; ++y) {
            for (int x = left; x <= right; ++x) {
                c.ink.push_back(char(ink(x, y)));
            }
            c.ink.push_back('\n');
        }
        return c;
    }

    struct Row {
        // Each cell's first alike cell (its index when none before is).
        std::vector<unsigned> alike;
        // Each cell's ink bounds' top row and its centroid less the row's mean, in device px.
        std::vector<int> tops;
        std::vector<double> offsets;
    };

    Row row(const std::uint8_t* pixels, unsigned width, unsigned r, unsigned scale) {
        std::vector<Cell> all;
        for (unsigned i = 0; i < cells; ++i) {
            all.push_back(cell(pixels, width, r, i, scale));
        }
        Row out;
        double mean{};
        for (unsigned i = 0; i < cells; ++i) {
            unsigned first = i;
            for (unsigned j = 0; j < i; ++j) {
                if (all[j].ink == all[i].ink) {
                    first = j;
                    break;
                }
            }
            out.alike.push_back(first);
            out.tops.push_back(all[i].top - all[0].top);
            mean += r == 0 ? all[i].y : all[i].x;
        }
        mean /= cells;
        for (const auto& c : all) {
            out.offsets.push_back((r == 0 ? c.y : c.x) - mean);
        }
        return out;
    }

    std::string listed(const std::vector<unsigned>& v) {
        std::string out;
        for (const auto x : v) {
            out += (out.empty() ? "" : " ") + std::to_string(x);
        }
        return out;
    }

    // RGBA rows of BGRA ones (weft's frames), opaque.
    std::vector<std::uint8_t> rgba(const std::vector<std::uint8_t>& bgra) {
        std::vector<std::uint8_t> out(bgra.size());
        for (std::size_t i = 0; i < bgra.size(); i += 4) {
            out[i] = bgra[i + 2];
            out[i + 1] = bgra[i + 1];
            out[i + 2] = bgra[i];
            out[i + 3] = 255;
        }
        return out;
    }

    void against_chrome(const fs::path& out) {
        const auto fixture = weft::testing::source_path("tests/fixtures/text-phase-reference");
        const auto reference = weft::testing::parse_json(weft::testing::read_file(fixture / "reference.json"));
        const auto width = unsigned(reference["width"].number);
        const auto height = unsigned(reference["height"].number);
        weft::ViewPage page;
        page.scripts.push_back({"page.js", weft::testing::read_text_file(fixture / "page.js")});
        page.stylesheet = weft::testing::read_text_file(fixture / "page.css");
        auto& text = weft::testing::thread_scene_text();
        const auto fonts = std::make_shared<weft::FontCatalog>(weft::testing::chrome_windows_font_catalog());
        weft::View view{fonts, text, float(width), float(height), 1};
        view.load(page);
        view.advance(0);
        require(view.take_diagnostics().empty(), "the page loads without a diagnostic");
        for (const auto& scale_value : reference["scales"].array) {
            const auto scale = unsigned(scale_value.number);
            const auto chrome = weft::testing::read_png(fixture / ("chrome-" + std::to_string(scale) + "x.png"));
            require(chrome.width == width * scale && chrome.height == height * scale, "the screenshot's size");
            weft::testing::D3D11Scene gpu(chrome.width, chrome.height);
            const auto native = rgba(gpu.draw(view.scene(), float(scale)));
            gpu.gpu.check_debug_messages();
            weft::SceneRaster raster(text);
            const auto drawn = raster.raster(view.scene(), float(scale));
            std::vector<std::uint8_t> pair(std::size_t(chrome.width) * 2 * chrome.height * 4);
            for (unsigned y = 0; y < chrome.height; ++y) {
                const auto row_bytes = std::size_t(chrome.width) * 4;
                std::copy_n(&native[y * row_bytes], row_bytes, &pair[y * row_bytes * 2]);
                std::copy_n(&chrome.rgba[y * row_bytes], row_bytes, &pair[y * row_bytes * 2 + row_bytes]);
            }
            weft::testing::write_png(out / ("text-phase-" + std::to_string(scale) + "x.png"), chrome.width * 2,
                                     chrome.height, pair);
            weft::testing::write_png(out / ("text-phase-reference-" + std::to_string(scale) + "x.png"), chrome.width,
                                     chrome.height, drawn.rgba);
            for (unsigned r = 0; r < 2; ++r) {
                const auto theirs = row(chrome.rgba.data(), chrome.width, r, scale);
                const auto ours = row(native.data(), chrome.width, r, scale);
                const auto reference_raster = row(drawn.rgba.data(), chrome.width, r, scale);
                const auto what = std::string(r == 0 ? "down" : "across") + " at " + std::to_string(scale) + "x";
                std::cout << what << ": alike cells, Chrome " << listed(theirs.alike) << "; native "
                          << listed(ours.alike) << "; reference raster " << listed(reference_raster.alike) << '\n';
                for (const auto& [drawing, name] :
                     {std::pair{&ours, "the native pass"}, std::pair{&reference_raster, "the reference raster"}}) {
                    require(drawing->alike == theirs.alike,
                            what + ": " + name + " draws cells alike where Chrome does");
                    if (r == 0) {
                        require(drawing->tops == theirs.tops, what + ": " + name + "'s rows step where Chrome's do");
                    }
                    double worst{};
                    for (unsigned i = 0; i < cells; ++i) {
                        worst = std::max(worst, std::abs(drawing->offsets[i] - theirs.offsets[i]));
                    }
                    std::cout << "  " << name << "'s centroids less the row's mean: at most " << std::fixed
                              << std::setprecision(3) << worst << " device px from Chrome's\n"
                              << std::defaultfloat;
                    // The reference raster's glyphs across are DirectWrite's in Direct2D, drawn apart from Chrome's at
                    // a quarter of a pixel (its pens are where Chrome's are: its cells are alike as Chrome's).
                    if (drawing == &ours || r == 0) {
                        require(worst <= centroid_tolerance, what + ": " + name + "'s ink where Chrome's is");
                    }
                }
            }
        }
    }

    void contexts() {
        auto& text = weft::testing::thread_scene_text();
        weft::testing::NativeScene s;
        s.set(*s.root, "style", "position:relative;width:300px;height:60px;background-color:#ffffff");
        const auto label = [&](weft::dom::Node& parent, const std::string& style) -> weft::dom::Node& {
            auto& e = s.html("div");
            s.set(e, "style", style);
            s.tree.insert(parent, e, nullptr);
            auto& words = s.tree.create(weft::constants::text_node_type, "#text", "", s.document->identity);
            words.data = "Still H";
            s.tree.insert(e, words, nullptr);
            return e;
        };
        const std::string font = ";font-family:Segoe UI;font-size:20px;line-height:30px;color:#000000";
        // Still; under a transform; in a container scrolled a quarter px down (its content 100px tall).
        auto& still = label(*s.root, "position:absolute;left:10.3px;top:10.4px" + font);
        label(*s.root, "position:absolute;left:110px;top:10px;transform:translate(.3px, .4px)" + font);
        auto& box = s.html("div");
        s.set(box, "style", "position:absolute;left:210px;top:0px;width:80px;height:60px;overflow:hidden");
        s.tree.insert(*s.root, box, nullptr);
        label(box, "position:absolute;left:.3px;top:10.65px" + font);
        auto& tall = s.html("div");
        s.set(tall, "style", "position:absolute;left:0px;top:0px;width:10px;height:100px");
        s.tree.insert(box, tall, nullptr);
        box.scroll_top = .25;
        s.tree.changed(box);
        weft::SceneSolver solver("", "", text);
        const auto frame = solver.frame(s.tree.snapshot(*s.root), 300, 60, 0);
        weft::testing::D3D11Scene gpu(300, 60);
        const auto first = gpu.draw(frame);
        require(gpu.scene.text_on_pixels == 1,
                "only still text on the pixels (" + std::to_string(gpu.scene.text_on_pixels) + " of 3)");

        // Still text moving: across, at most text_phase_steps images of its line; down, none more.
        const auto at = [&](double left, double top) {
            std::ostringstream style;
            style << "position:absolute;left:" << left << "px;top:" << top << "px" << font;
            s.set(still, "style", style.str());
            return solver.frame(s.tree.snapshot(*s.root), 300, 60, 0);
        };
        const auto sweep = [&] {
            const auto builds = gpu.scene.text_builds;
            for (int f = 1; f <= 40; ++f) {
                gpu.draw(at(10.3 + f * .1, 10.4));
            }
            return gpu.scene.text_builds - builds;
        };
        const auto across = sweep();
        const auto again = sweep();
        const auto builds = gpu.scene.text_builds;
        for (int f = 1; f <= 20; ++f) {
            gpu.draw(at(14.3, 10.4 + f * .1));
        }
        const auto down = gpu.scene.text_builds - builds;
        std::cout << "still text moving: 40 frames across built " << across << " images, the same frames again "
                  << again << ", 20 frames down " << down << '\n';
        require(across > 0 && across <= std::size_t(weft::constants::text::text_phase_steps) && again == 0 && down == 0,
                "still text moving draws from at most a line's phase images");
        // A frame draws alike whatever the renderer drew before.
        weft::testing::D3D11Scene fresh(300, 60);
        const auto start = at(10.3, 10.4);
        require(gpu.draw(start) == fresh.draw(start) && fresh.draw(start) == first,
                "a frame draws alike whatever the renderer drew before");
        gpu.gpu.check_debug_messages();
    }
} // namespace

int main(int argc, char** argv) {
    try {
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM init");
        const auto out = fs::path(argc > 1 ? argv[1] : "artifacts/text-phase");
        fs::create_directories(out);
        against_chrome(out);
        contexts();
        std::cout << "PASS text phase: still glyphs on the pixels where Chrome draws them at 1x and 2x; text under a "
                     "transform or scrolled between them; a line's phase images\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
