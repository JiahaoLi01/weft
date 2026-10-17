#include "scene/scene.hpp"
#include "support/check.hpp"
#include "support/d3d11_scene.hpp"
#include "support/files.hpp"
#include "support/json_value.hpp"
#include "support/png.hpp"
#include "support/scene_fonts.hpp"
#include "view/view.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// The logo pages (examples/logo) against Chrome's pixels: each variant's page.json loaded into a View of the cases'
// size (examples/logo/reference-cases.json), advanced to each case frame (1/60 s each, its animations started at the
// load as the browser reference starts them) and drawn by the native D3D11 pass, against the frames
// tools/browser_logo_reference.mjs captured (tests/fixtures/logo-reference). The logo is SVG strokes only, so no font
// is involved. A frame passes when at most 0.5% of its pixels differ by more than 32 levels in a channel: the two
// rasterizers' antialiasing of the strokes' curved edges differs by a little, a wrong colour, stroke, layer or moment
// by much more. Side-by-side pairs (weft left, Chrome right) and the measures go to the output folder.
namespace {
    using weft::testing::require;
    namespace fs = std::filesystem;

    constexpr int level = 32;
    constexpr double share = 0.005;

    struct Measure {
        double over{};
        double mean{};
        int max{};
    };

    // weft's frame (encoded premultiplied BGRA rows over a transparent target; the logo's root is opaque) against
    // Chrome's RGBA screenshot.
    Measure compare(const std::vector<std::uint8_t>& bgra, const weft::testing::RgbaImage& chrome) {
        Measure m;
        std::size_t over{};
        const auto pixels = std::size_t(chrome.width) * chrome.height;
        for (std::size_t i = 0; i < pixels; ++i) {
            int worst{};
            for (int c = 0; c < 3; ++c) {
                worst = std::max(worst, std::abs(int(bgra[i * 4 + 2 - c]) - int(chrome.rgba[i * 4 + c])));
            }
            over += worst > level;
            m.mean += worst;
            m.max = std::max(m.max, worst);
        }
        m.over = double(over) / double(pixels);
        m.mean /= double(pixels);
        return m;
    }

    std::vector<std::uint8_t> pair(const std::vector<std::uint8_t>& bgra, const weft::testing::RgbaImage& chrome) {
        const auto w = chrome.width;
        const auto h = chrome.height;
        std::vector<std::uint8_t> out(std::size_t(w) * 2 * h * 4);
        for (unsigned y = 0; y < h; ++y) {
            for (unsigned x = 0; x < w; ++x) {
                const auto from = (std::size_t(y) * w + x) * 4;
                const auto left = (std::size_t(y) * w * 2 + x) * 4;
                const auto right = left + std::size_t(w) * 4;
                out[left] = bgra[from + 2];
                out[left + 1] = bgra[from + 1];
                out[left + 2] = bgra[from];
                out[left + 3] = 255;
                std::copy_n(&chrome.rgba[from], 4, &out[right]);
            }
        }
        return out;
    }
} // namespace

int main(int argc, char** argv) {
    try {
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM init");
        const auto out = fs::path(argc > 1 ? argv[1] : "artifacts/logo-reference");
        fs::create_directories(out);
        const auto logo = weft::testing::source_path("examples/logo");
        const auto fixtures = weft::testing::source_path("tests/fixtures/logo-reference");
        const auto cases = weft::testing::parse_json(weft::testing::read_file(logo / "reference-cases.json"));
        const auto width = unsigned(cases["width"].number);
        const auto height = unsigned(cases["height"].number);
        const auto fonts = std::make_shared<weft::FontCatalog>(weft::testing::chrome_windows_font_catalog());
        weft::SceneText text{*fonts};
        weft::testing::D3D11Scene gpu(width, height);
        int failed{};
        int compared{};
        for (const auto& variant : cases["variants"].array) {
            const auto folder = logo / variant.string;
            const auto manifest = weft::testing::parse_json(weft::testing::read_file(folder / "page.json"));
            weft::ViewPage page;
            for (const auto& script : manifest["scripts"].array) {
                page.scripts.push_back({script.string, weft::testing::read_text_file(folder / script.string)});
            }
            page.stylesheet = weft::testing::read_text_file(folder / manifest["stylesheet"].string);
            page.keyframes = weft::testing::read_text_file(folder / manifest["keyframes"].string);
            page.base = (folder / manifest["stylesheet"].string).generic_string();
            weft::View view{fonts, text, float(width), float(height), 1};
            view.load(page);
            view.advance(0);
            for (const auto& frame : cases["frames"].array) {
                const auto at = int(frame.number);
                view.advance(at * 1000.0 / 60.0);
                const auto diagnostics = view.take_diagnostics();
                require(diagnostics.empty(), variant.string + ": " +
                                                 (diagnostics.empty() ? std::string() : diagnostics.front().message));
                const auto ours = gpu.draw(view.scene());
                std::ostringstream name;
                name << variant.string << '-' << std::setw(3) << std::setfill('0') << at;
                const auto chrome = weft::testing::read_png(fixtures / (name.str() + ".png"));
                require(chrome.width == width && chrome.height == height, name.str() + ": the reference's size");
                const auto m = compare(ours, chrome);
                const bool passed = m.over <= share;
                ++compared;
                failed += !passed;
                weft::testing::write_png(out / (name.str() + ".png"), width * 2, height, pair(ours, chrome));
                std::cout << (passed ? "ok   " : "FAIL ") << name.str() << ": " << std::fixed << std::setprecision(3)
                          << m.over * 100 << "% over " << level << ", mean " << std::setprecision(2) << m.mean
                          << ", max " << m.max << '\n';
            }
        }
        require(compared > 0, "frames compared");
        require(failed == 0, std::to_string(failed) + " of " + std::to_string(compared) + " frames differ from Chrome");
        std::cout << "PASS logo reference (" << compared << " frames)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
