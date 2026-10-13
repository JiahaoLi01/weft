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
#include <iostream>
#include <map>
#include <string>

// backdrop-filter against Chrome (11.5b): each case of tests/fixtures/backdrop-reference/cases.json, the same elements
// with the same inline styles, through the scene solver and the native D3D11 pass at the case's device scale, against
// Chrome's screenshot of it (tools/browser_backdrop_reference.mjs), whose manifest must name the cases as they are.
// What lies behind the glass matches Chrome within two levels; inside the glass, the colors stay within the case's mean
// and worst difference, and few pixels differ by more than 16 levels.
namespace {
    using weft::testing::require;

    struct Stats {
        double mean{};
        int worst{};
        double far_share{};
        std::size_t pixels{};
    };

    // How far a case's glass may differ from Chrome: the mean and the worst RGB level, and the share of its pixels more
    // than 16 levels off. Chrome's GPU blur scales a wide blur's source down its own way (ours by area, to the size Skia
    // picks), which leaves a few levels; at rounded corners the edge's antialiasing differs too.
    struct Limits {
        double mean{};
        int worst{};
        double far_share{};
    };

    const std::map<std::string, Limits>& limits() {
        static const std::map<std::string, Limits> cases{
            {"blur-3", {.5, 3, 0}},    {"blur-20", {3.5, 14, 0}},    {"blur-8-scale-2", {2.5, 10, 0}},
            {"rounded", {2.5, 12, 0}}, {"saturate", {3.5, 16, 0}},   {"grayscale-first", {2.5, 10, 0}},
            {"edge", {2.5, 8, 0}},     {"opacity", {1.5, 6, 0}},     {"group", {.5, 4, 0}},
            {"nested", {.5, 4, 0}},    {"frosted", {1.5, 32, .005}},
        };
        return cases;
    }

    // RGB differences between the native pass's encoded premultiplied BGRA (on an opaque backdrop) and Chrome's RGBA,
    // over the device pixels of [x0, x1) x [y0, y1) for which inside says so.
    template <class Inside>
    Stats compare(const std::vector<std::uint8_t>& native, const weft::testing::RgbaImage& chrome, Inside inside,
                  int far_level) {
        Stats s;
        std::size_t beyond{};
        double sum{};
        for (unsigned y = 0; y < chrome.height; ++y) {
            for (unsigned x = 0; x < chrome.width; ++x) {
                if (!inside(int(x), int(y))) {
                    continue;
                }
                const auto* n = native.data() + (std::size_t(y) * chrome.width + x) * 4;
                const auto* c = chrome.rgba.data() + (std::size_t(y) * chrome.width + x) * 4;
                int worst{};
                for (unsigned k = 0; k < 3; ++k) {
                    const int d = std::abs(int(n[2 - k]) - int(c[k]));
                    sum += d;
                    worst = std::max(worst, d);
                }
                s.worst = std::max(s.worst, worst);
                beyond += worst > far_level;
                ++s.pixels;
            }
        }
        s.mean = s.pixels ? sum / (3.0 * double(s.pixels)) : 0;
        s.far_share = s.pixels ? double(beyond) / double(s.pixels) : 0;
        return s;
    }
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 3, "usage: weft_scene_backdrop_reference_tests <artifact folder> <cases.json sha256>");
        const std::filesystem::path folder = argv[1];
        std::filesystem::create_directories(folder);
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM init");
        const std::filesystem::path fixtures =
            std::filesystem::path(WEFT_SOURCE_DIR) / "tests/fixtures/backdrop-reference";
        const auto spec = weft::testing::parse_json(weft::testing::read_file((fixtures / "cases.json").string()));
        // Chrome's captures are of these cases: the manifest records the hash of the cases they were taken from, and
        // one capture for each case, at its scale.
        const auto manifest =
            weft::testing::parse_json(weft::testing::read_file((fixtures / "manifest.json").string()));
        require(manifest["cases_sha256"].string == argv[2],
                "Chrome's captures are of other cases: record them again with tools/browser_backdrop_reference.mjs");
        require(manifest["captures"].size() == spec["cases"].size(), "a capture for each case");
        const auto width = unsigned(spec["width"].number);
        const auto height = unsigned(spec["height"].number);
        auto& text = weft::testing::thread_scene_text();
        std::cout << "case                 behind: mean worst | glass: mean worst >16 share (limits)\n";
        for (std::size_t i = 0; i < spec["cases"].size(); ++i) {
            const auto& c = spec["cases"][i];
            const auto name = c["name"].string;
            const auto scale = float(c["scale"].number);
            require(manifest["captures"][i]["name"].string == name && manifest["captures"][i]["scale"].number == scale,
                    name + ": Chrome's capture of the case");
            const auto limit = limits().find(name);
            require(limit != limits().end(), name + ": the case's limits");
            weft::testing::NativeScene s;
            s.set(*s.root, "style", spec["root"].string);
            std::map<std::string, weft::dom::Node*> nodes;
            const auto add = [&](const weft::testing::JsonValue& n) {
                auto& e = s.html("div");
                s.set(e, "style", n["style"].string);
                const auto parent = n.find("parent") ? nodes.at(n["parent"].string) : s.root;
                s.tree.insert(*parent, e, nullptr);
                nodes[n["id"].string] = &e;
            };
            for (std::size_t k = 0; k < spec["background"].size(); ++k) {
                add(spec["background"][k]);
            }
            for (std::size_t k = 0; k < c["nodes"].size(); ++k) {
                add(c["nodes"][k]);
            }
            weft::SceneSolver solver("", "", text);
            const auto frame = solver.frame(s.tree.snapshot(*s.root), float(width), float(height), 0);
            const auto w = unsigned(width * scale);
            const auto h = unsigned(height * scale);
            weft::testing::D3D11Scene d3d11{w, h};
            const auto native = d3d11.draw(frame, scale);
            d3d11.gpu.check_debug_messages();
            const auto chrome = weft::testing::read_png(fixtures / (name + ".png"));
            require(chrome.width == w && chrome.height == h, name + ": Chrome's capture at the case's scale");
            // The glass's box in device px: the element the case names, in the page (its parents' offsets added).
            std::array<float, 4> glass{};
            for (const auto& e : frame.elements) {
                if (e.identity == nodes.at(c["glass"].string)->identity) {
                    glass = {float(e.rect[0]) * scale, float(e.rect[1]) * scale, float(e.rect[0] + e.rect[2]) * scale,
                             float(e.rect[1] + e.rect[3]) * scale};
                }
            }
            const auto in_glass = [&](int x, int y, float margin) {
                return x + .5f > glass[0] + margin && x + .5f < glass[2] - margin && y + .5f > glass[1] + margin &&
                       y + .5f < glass[3] - margin;
            };
            const auto behind = compare(
                native, chrome,
                [&](int x, int y) {
                    return !in_glass(x, y, -2 * scale);
                },
                2);
            const auto inside = compare(
                native, chrome,
                [&](int x, int y) {
                    return in_glass(x, y, 2 * scale);
                },
                16);
            const auto& l = limit->second;
            std::printf("%-20s %6.2f %4d | %6.2f %4d %.4f (%.2f %d %.4f)\n", name.c_str(), behind.mean, behind.worst,
                        inside.mean, inside.worst, inside.far_share, l.mean, l.worst, l.far_share);
            // Evidence: ours beside Chrome's.
            std::vector<std::uint8_t> pair(std::size_t(w) * 2 * h * 4);
            for (unsigned y = 0; y < h; ++y) {
                for (unsigned x = 0; x < w; ++x) {
                    const auto* n = native.data() + (std::size_t(y) * w + x) * 4;
                    auto* left = pair.data() + (std::size_t(y) * w * 2 + x) * 4;
                    left[0] = n[2];
                    left[1] = n[1];
                    left[2] = n[0];
                    left[3] = 255;
                    const auto* cr = chrome.rgba.data() + (std::size_t(y) * w + x) * 4;
                    std::copy(cr, cr + 4, pair.data() + (std::size_t(y) * w * 2 + w + x) * 4);
                }
            }
            weft::testing::write_png(folder / (name + ".png"), w * 2, h, pair);
            require(behind.pixels > 0 && behind.worst <= 2, name + ": what lies behind the glass as Chrome draws it");
            require(inside.pixels > 0 && inside.mean <= l.mean && inside.worst <= l.worst &&
                        inside.far_share <= l.far_share,
                    name + ": the glass within its limits of Chrome's");
        }
        std::cout << "PASS backdrop-filter against Chrome\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
