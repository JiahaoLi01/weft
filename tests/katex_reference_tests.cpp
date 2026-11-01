#include "scene/scene.hpp"
#include "support/check.hpp"
#include "support/d3d11_scene.hpp"
#include "support/files.hpp"
#include "support/json_value.hpp"
#include "support/png.hpp"
#include "support/scene_fonts.hpp"
#include "view/view.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

// KaTeX's HTML output in a View against Chrome (tools/browser_katex_reference.mjs, tests/fixtures/katex-reference). The
// KaTeX page (examples/katex: the formulas of CaseGinkgo's camera tuning panel, inline and in display mode) loads with
// katex.min.js and katex.css as KaTeX ships them (third_party/katex), the stylesheet katex.css then page.css as a host
// joins them, its fonts the TrueType sources of katex.css's @font-face rules, which the View reads beside katex.css.
// - It loads and lays out with no diagnostic.
// - Every element's bounding client rect is Chrome's within a LayoutUnit (1/64 px): KaTeX's inline tables, struts
//   (vertical-align), min-content widths, relative offsets, svg viewBox, text runs and fonts.
// - Drawn by the native D3D11 pass at device scale 1 and 2, each row (a formula) stays within the limits the scene's
//   other text references keep (tests/scene_paint_reference_tests.cpp): a mean RGB difference of 3, 6% of pixels more
//   than 32 levels apart (glyph edges: FreeType's grayscale coverage against DirectWrite's), and 56 levels once both are
//   averaged over 5 x 5 pixels (a fraction bar between pixel rows, which Chrome snaps onto one).
// - Chrome lays a page out at its device scale (its lengths, LayoutUnits and font metrics in device px), weft at every
//   scale as at 1x: at 2x, wherever KaTeX's em lengths round, Chrome's own layout moves elements by half a CSS px from
//   where its 1x layout has them (its rects at each scale are in the reference). A row is compared with Chrome's frame
//   moved back by the offset that most of the row's elements have in Chrome's layout at that scale, in whole device px.
// Side-by-side pairs (weft left, Chrome right, unmoved) go to the output folder.
namespace {
    using weft::testing::JsonValue;
    using weft::testing::require;
    namespace fs = std::filesystem;

    struct Limits {
        double mean{};
        double far_share{};
        int smooth{};
    };

    constexpr Limits text_limits{3, .06, 56};
    constexpr int far_levels = 32;
    constexpr int smoothing = 5;

    struct Stats {
        double mean{};
        double far_share{};
        int smooth{};
    };

    // The page's rows of formulas in Chrome's 1x geometry: tops and heights in CSS px, and the range of elements each
    // holds (the row and its descendants, in tree order).
    struct Row {
        double top{};
        double height{};
        std::size_t first{};
        std::size_t end{};
    };

    std::vector<Row> rows(const JsonValue& rects) {
        std::vector<Row> out;
        for (std::size_t i = 0; i < rects.array.size(); ++i) {
            const auto& r = rects.array[i];
            if (r[0].string == "div" && r[1].string.starts_with("row")) {
                if (!out.empty()) {
                    out.back().end = i;
                }
                out.push_back({r[3].number, r[5].number, i, rects.array.size()});
            }
        }
        return out;
    }

    // The offset in whole device px that most of a row's elements have in Chrome's layout at scale against its 1x
    // layout (the smaller one of equally many).
    int row_offset(const Row& row, const JsonValue& one, const JsonValue& scaled, unsigned scale) {
        std::map<int, std::size_t> offsets;
        for (auto i = row.first; i < row.end; ++i) {
            ++offsets[int(std::lround((scaled[i][3].number - one[i][3].number) * scale))];
        }
        int best{};
        std::size_t most{};
        for (const auto& [offset, count] : offsets) {
            if (count > most || (count == most && std::abs(offset) < std::abs(best))) {
                best = offset;
                most = count;
            }
        }
        return best;
    }

    // The image moved up by dy pixels (down for a negative dy), white where it moves in past its edges: the page's
    // background.
    weft::testing::RgbaImage moved(const weft::testing::RgbaImage& image, int dy) {
        auto out = image;
        std::fill(out.rgba.begin(), out.rgba.end(), std::uint8_t(255));
        const auto row = std::size_t(image.width) * weft::testing::png_bytes_per_pixel;
        for (unsigned y = 0; y < image.height; ++y) {
            const auto from = std::int64_t(y) + dy;
            if (from >= 0 && from < std::int64_t(image.height)) {
                std::copy_n(&image.rgba[std::size_t(from) * row], row, &out.rgba[std::size_t(y) * row]);
            }
        }
        return out;
    }

    // weft's frame (encoded premultiplied BGRA over a transparent target; the page is opaque) against Chrome's RGBA
    // screenshot: per pixel, its largest channel difference and the largest difference of the channels' 5 x 5 means
    // (over the whole frame, clamped at its edges: summed area tables).
    struct Difference {
        std::vector<int> raw;
        std::vector<double> smooth;
        std::vector<double> sum;
    };

    Difference difference(const std::vector<std::uint8_t>& bgra, const weft::testing::RgbaImage& chrome) {
        const auto w = chrome.width;
        const auto h = chrome.height;
        Difference d;
        d.raw.resize(std::size_t(w) * h);
        d.smooth.resize(d.raw.size());
        d.sum.resize(d.raw.size());
        std::vector<double> sum_ours(std::size_t(w + 1) * (h + 1) * 3);
        std::vector<double> sum_chrome(sum_ours.size());
        const auto at = [&](std::vector<double>& t, unsigned x, unsigned y, unsigned c) -> double& {
            return t[(std::size_t(y) * (w + 1) + x) * 3 + c];
        };
        for (unsigned y = 0; y < h; ++y) {
            for (unsigned x = 0; x < w; ++x) {
                const auto p = (std::size_t(y) * w + x) * 4;
                for (unsigned c = 0; c < 3; ++c) {
                    const int ours = bgra[p + 2 - c];
                    const int theirs = chrome.rgba[p + c];
                    d.raw[p / 4] = std::max(d.raw[p / 4], std::abs(ours - theirs));
                    d.sum[p / 4] += std::abs(ours - theirs);
                    at(sum_ours, x + 1, y + 1, c) =
                        ours + at(sum_ours, x, y + 1, c) + at(sum_ours, x + 1, y, c) - at(sum_ours, x, y, c);
                    at(sum_chrome, x + 1, y + 1, c) =
                        theirs + at(sum_chrome, x, y + 1, c) + at(sum_chrome, x + 1, y, c) - at(sum_chrome, x, y, c);
                }
            }
        }
        constexpr int reach = smoothing / 2;
        for (unsigned y = 0; y < h; ++y) {
            for (unsigned x = 0; x < w; ++x) {
                const auto x0 = unsigned(std::max(0, int(x) - reach));
                const auto y0 = unsigned(std::max(0, int(y) - reach));
                const auto x1 = std::min(w, x + reach + 1);
                const auto y1 = std::min(h, y + reach + 1);
                const double area = double(x1 - x0) * (y1 - y0);
                for (unsigned c = 0; c < 3; ++c) {
                    const auto box = [&](std::vector<double>& t) {
                        return (at(t, x1, y1, c) - at(t, x0, y1, c) - at(t, x1, y0, c) + at(t, x0, y0, c)) / area;
                    };
                    auto& s = d.smooth[std::size_t(y) * w + x];
                    s = std::max(s, std::abs(box(sum_ours) - box(sum_chrome)));
                }
            }
        }
        return d;
    }

    // The measures over device rows [top, bottom).
    Stats measure(const Difference& d, unsigned width, unsigned top, unsigned bottom) {
        Stats s;
        std::size_t distant{};
        double smooth{};
        for (auto i = std::size_t(top) * width; i < std::size_t(bottom) * width; ++i) {
            s.mean += d.sum[i];
            distant += d.raw[i] > far_levels;
            smooth = std::max(smooth, d.smooth[i]);
        }
        const double pixels = double(bottom - top) * width;
        s.mean /= 3 * pixels;
        s.far_share = double(distant) / pixels;
        s.smooth = int(std::lround(smooth));
        return s;
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
        const auto out = fs::path(argc > 1 ? argv[1] : "artifacts/katex-reference");
        fs::create_directories(out);
        const auto fixture = weft::testing::source_path("tests/fixtures/katex-reference");
        const auto reference = weft::testing::parse_json(weft::testing::read_file(fixture / "reference.json"));
        const auto width = unsigned(reference["width"].number);
        const auto height = unsigned(reference["height"].number);

        // The page as a host joins it: KaTeX's script and the page's, katex.css then page.css with katex.css as the
        // base its url()s resolve against, and a reader of the files beside it.
        weft::ViewPage page;
        page.scripts.push_back({"katex.min.js", weft::testing::read_text_file(
                                                    weft::testing::source_path("third_party/katex/katex.min.js"))});
        page.scripts.push_back(
            {"page.js", weft::testing::read_text_file(weft::testing::source_path("examples/katex/page.js"))});
        const auto katex_css = weft::testing::source_path("third_party/katex/katex.css");
        page.stylesheet = weft::testing::read_text_file(katex_css) + "\n" +
                          weft::testing::read_text_file(weft::testing::source_path("examples/katex/page.css"));
        page.base = katex_css.generic_string();
        page.forgiving_css = true;
        const auto fonts = std::make_shared<weft::FontCatalog>(weft::testing::chrome_windows_font_catalog());
        auto& text = weft::testing::thread_scene_text();
        weft::View view{fonts, text, float(width), float(height), 1};
        std::vector<std::string> read;
        view.set_resource_reader([&read](std::string_view path) -> std::optional<std::vector<std::uint8_t>> {
            read.emplace_back(path);
            const fs::path file(std::u8string(path.begin(), path.end()));
            if (!fs::is_regular_file(file)) {
                return std::nullopt;
            }
            const auto bytes = weft::testing::read_file(file);
            return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
        });
        // What a host waits for: the page load (KaTeX's script, the page's 48 renders, its 20 fonts read) and the first
        // frame's scene.
        const auto started = std::chrono::steady_clock::now();
        view.load(page);
        view.advance(0);
        std::cout << "page load and first frame: "
                  << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count()
                  << " ms\n";
        const auto diagnostics = view.take_diagnostics();
        for (const auto& d : diagnostics) {
            std::cout << "  diagnostic: " << d.message << '\n';
        }
        require(diagnostics.empty(), "KaTeX loads and lays out with no diagnostic");
        // katex.css names a woff2, a woff and a ttf file for each of its 20 faces: the View reads the TrueType ones
        // (the formats it reads), and only those.
        require(read.size() == 20 && std::all_of(read.begin(), read.end(),
                                                 [](const std::string& p) {
                                                     return p.ends_with(".ttf");
                                                 }),
                "the View reads the TrueType source of each @font-face rule");
        require(view.fonts() != fonts, "the page's fonts are a catalog of its own");

        // Geometry: every element's bounding client rect, in tree order.
        const auto [ok, dump] =
            view.evaluate("JSON.stringify([...document.getElementById('root').querySelectorAll('*')].map((e) => {"
                          "const r = e.getBoundingClientRect();"
                          "return [e.localName, e.getAttribute('class') || '', r.left, r.top, r.width, r.height]; }))");
        require(ok, "the geometry read: " + dump);
        const auto ours = weft::testing::parse_json(dump);
        const auto& theirs = reference["rects"];
        require(ours.array.size() == theirs.array.size(), "an element for each of Chrome's");
        constexpr double tolerance = 1.0 / 64 + 1e-3;
        std::size_t differ{};
        double worst{};
        for (std::size_t i = 0; i < theirs.array.size(); ++i) {
            const auto& a = ours.array[i];
            const auto& b = theirs.array[i];
            require(a[0].string == b[0].string && a[1].string == b[1].string,
                    "element " + std::to_string(i) + ": " + a[0].string + "." + a[1].string + " where Chrome has " +
                        b[0].string + "." + b[1].string);
            double d{};
            for (std::size_t k = 2; k < 6; ++k) {
                d = std::max(d, std::abs(a[k].number - b[k].number));
            }
            worst = std::max(worst, d);
            if (d > tolerance) {
                if (++differ <= 20) {
                    std::cout << "  element " << i << " " << a[0].string << "." << a[1].string << ": (" << a[2].number
                              << ", " << a[3].number << ", " << a[4].number << ", " << a[5].number << "), Chrome ("
                              << b[2].number << ", " << b[3].number << ", " << b[4].number << ", " << b[5].number
                              << ")\n";
                }
            }
        }
        std::cout << theirs.array.size() << " elements against " << reference["browser"].string << ": " << differ
                  << " differ by more than a LayoutUnit, worst " << worst << " px\n";
        require(differ == 0, "every element where Chrome lays it out, within a LayoutUnit");

        // Pixels, each row of the page within the text limits.
        const auto bands = rows(theirs);
        require(bands.size() == 24, "24 rows of formulas");
        const auto& scaled_rects = reference["scaled_rects"];
        bool failed{};
        text.use(view.fonts());
        for (const auto& scale_value : reference["scales"].array) {
            const auto scale = unsigned(scale_value.number);
            weft::testing::D3D11Scene gpu(width * scale, height * scale);
            const auto drawn = gpu.draw(view.scene(), float(scale));
            gpu.gpu.check_debug_messages();
            const auto chrome = weft::testing::read_png(fixture / ("chrome-" + std::to_string(scale) + "x.png"));
            require(chrome.width == width * scale && chrome.height == height * scale, "the screenshot's size");
            weft::testing::write_png(out / ("katex-" + std::to_string(scale) + "x.png"), chrome.width * 2,
                                     chrome.height, pair(drawn, chrome));
            Stats most{};
            // Each row's offset in Chrome's layout at this scale, and the differences from Chrome's frame moved back by
            // each offset some row has.
            std::vector<int> offsets(bands.size());
            if (scale != 1) {
                const auto& at_scale = scaled_rects[std::to_string(scale)];
                require(at_scale.array.size() == theirs.array.size(), "Chrome's rects at each scale");
                double furthest{};
                for (std::size_t i = 0; i < theirs.array.size(); ++i) {
                    for (std::size_t k = 2; k < 6; ++k) {
                        furthest = std::max(furthest, std::abs(at_scale[i][k].number - theirs[i][k].number));
                    }
                }
                std::string moves;
                for (std::size_t k = 0; k < bands.size(); ++k) {
                    offsets[k] = row_offset(bands[k], theirs, at_scale, scale);
                    if (offsets[k]) {
                        moves += (moves.empty() ? "" : ", ") + std::to_string(k) + " (" + (offsets[k] > 0 ? "+" : "") +
                                 std::to_string(offsets[k]) + ")";
                    }
                }
                std::cout << "Chrome's layout at " << scale << "x: rects up to " << furthest
                          << " px from its 1x ones; rows it moves by whole device px: "
                          << (moves.empty() ? "none" : moves) << '\n';
            }
            std::map<int, Difference> differences;
            for (const auto offset : offsets) {
                if (!differences.contains(offset)) {
                    differences.emplace(offset, difference(drawn, offset ? moved(chrome, offset) : chrome));
                }
            }
            for (std::size_t k = 0; k < bands.size(); ++k) {
                const auto top = unsigned(std::lround(bands[k].top * scale));
                const auto bottom =
                    std::min(chrome.height, unsigned(std::lround((bands[k].top + bands[k].height) * scale)));
                const auto s = measure(differences.at(offsets[k]), chrome.width, top, bottom);
                const bool within = s.mean <= text_limits.mean && s.far_share <= text_limits.far_share &&
                                    s.smooth <= text_limits.smooth;
                failed = failed || !within;
                most = {std::max(most.mean, s.mean), std::max(most.far_share, s.far_share),
                        std::max(most.smooth, s.smooth)};
                if (!within) {
                    std::cout << "  row " << k << " at " << scale << "x: mean " << s.mean << ", >" << far_levels << " "
                              << s.far_share << ", " << smoothing << "x" << smoothing << " " << s.smooth
                              << ": outside the limits\n";
                }
            }
            std::cout << "scale " << scale << ": rows' worst mean " << std::fixed << std::setprecision(3) << most.mean
                      << ", >" << far_levels << " share " << most.far_share << ", " << smoothing << "x" << smoothing
                      << " " << most.smooth << " (limits " << text_limits.mean << ", " << text_limits.far_share << ", "
                      << text_limits.smooth << ")\n"
                      << std::defaultfloat;
        }
        require(!failed, "each row within the text limits of Chrome's pixels");
        std::cout << "PASS KaTeX: no diagnostics, " << theirs.array.size()
                  << " element rects within a LayoutUnit of Chrome's, rows within the text limits at 1x and 2x (where "
                     "Chrome's layout puts them)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
