#include "bridge/engine_channel.hpp"
#include "scene/scene_anchor.hpp"
#include "view/view.hpp"
#if defined(_WIN32)
#include "platform/win32/system_fonts.hpp"
#elif defined(__ANDROID__)
#include "platform/android/system_fonts.hpp"
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include "platform/macos/system_fonts.hpp"
#define WEFT_MACOS_FONTS 1
#endif
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// What a View costs its owner thread each frame (docs/engine-communication.md, 13.3): the HUD of the gallery
// (examples/crpg-gallery/native/HUD_TESTS.js, anchored nameplates) in a View of 1920 x 1080, the engine writing every
// unit's anchor each frame and, at the second level, pushing 20 health changes a frame as a transform (no layout).
// Timed per frame: the advance (engine inputs, tasks, animation frames, the scene) and, within it, the copy of the
// scene a changed frame publishes for the render thread.
namespace {
    using Clock = std::chrono::steady_clock;
    constexpr float width = 1920;
    constexpr float height = 1080;

    std::string read(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw std::runtime_error("cannot read " + path.string());
        }
        return {std::istreambuf_iterator<char>(in), {}};
    }

    double percentile(std::vector<double> v, double p) {
        std::sort(v.begin(), v.end());
        return v.empty() ? 0 : v[std::min(v.size() - 1, std::size_t(std::ceil(p * double(v.size()))) - 1)];
    }

    std::string stats(const std::vector<double>& v) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(3) << "{\"p50\":" << percentile(v, .5)
            << ",\"p99\":" << percentile(v, .99) << ",\"max\":" << percentile(v, 1) << '}';
        return out.str();
    }

    double since(Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    struct Level {
        unsigned units{};
        bool health{};
        std::vector<double> advance;
        std::vector<double> copy;
        std::size_t published{};
        std::size_t nodes{};
    };

    std::size_t count(const weft::SceneNode& n) {
        std::size_t total = 1;
        for (const auto& c : n.children) {
            total += count(c);
        }
        return total;
    }

    void run(Level& level, const std::filesystem::path& root, weft::SceneText& text,
             const std::shared_ptr<const weft::FontCatalog>& fonts, unsigned warmup, unsigned samples) {
        weft::View view(fonts, text, width, height, 1);
        weft::ViewPage page;
        page.scripts = {{"HUD_TESTS.js", read(root / "examples/crpg-gallery/native/HUD_TESTS.js")},
                        {"mount.js", "__ceGallery.mount('HUD_TESTS',0);__ceHud.setCount(" +
                                         std::to_string(level.units) + ");__ceHud.anchor(true)"}};
        page.stylesheet = read(root / "examples/crpg-gallery/hud-utilities.css");
        page.keyframes = read(root / "examples/crpg-gallery/hud-keyframes.css");
        view.load(page);
        std::vector<weft::SceneAnchor> anchors(level.units);
        auto last = view.shared()->frame();
        for (unsigned frame = 0; frame < warmup + samples; ++frame) {
            const double t = frame / 60.0;
            for (unsigned i = 0; i < level.units; ++i) {
                const double a = t * (0.6 + (i % 5) * 0.25) + i * 0.7;
                anchors[i] = {i + 1,
                              float(80 + std::fmod(i * 197.0, width - 260) + (20 + (i % 7) * 9) * std::cos(a)),
                              float(60 + std::fmod(i * 113.0, height - 160) + (20 + (i % 7) * 9) * std::sin(a)),
                              1,
                              float(i),
                              0,
                              0};
            }
            view.shared()->write_anchors(anchors);
            if (level.health) {
                std::string changes = "[";
                // Each frame the next 20 units, each visit a value other than the last.
                for (unsigned k = 0; k < 20; ++k) {
                    const unsigned n = frame * 20 + k;
                    const unsigned i = n % level.units;
                    const unsigned visit = n / level.units;
                    changes += (k ? "," : "") + std::string("[") + std::to_string(i) + "," +
                               std::to_string(0.05 + 0.9 * ((visit + i) % 17) / 17.0) + "]";
                }
                view.script().eval("__ceHud.health('" + changes + "]', true)");
            }
            const auto start = Clock::now();
            view.advance(frame * 1000.0 / 60);
            const double advance = since(start);
            const auto published = view.shared()->frame();
            // The copy a changed frame published, made again alone.
            double copy{};
            if (published != last) {
                const auto copied = Clock::now();
                auto scene = std::make_shared<weft::SceneFrame>();
                scene->root = view.scene().root;
                copy = since(copied);
                ++level.published;
            }
            last = published;
            if (frame >= warmup) {
                level.advance.push_back(advance);
                level.copy.push_back(copy);
            }
        }
        level.nodes = count(view.scene().root);
        if (!view.take_diagnostics().empty()) {
            throw std::runtime_error("the HUD has diagnostics");
        }
    }
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3 || (std::string_view(argv[1]) != "--quick" && std::string_view(argv[1]) != "--full")) {
            std::cerr << "usage: weft_view_benchmark --quick|--full <folder>\n";
            return 2;
        }
        const bool full = std::string_view(argv[1]) == "--full";
        const unsigned warmup = full ? 30 : 2;
        const unsigned samples = full ? 240 : 4;
        const std::filesystem::path root = WEFT_SOURCE_DIR;
#if defined(_WIN32)
        auto catalog = weft::platform::win32::system_font_catalog();
#elif defined(__ANDROID__)
        auto catalog = weft::platform::android::system_font_catalog();
#elif defined(WEFT_MACOS_FONTS)
        auto catalog = weft::platform::macos::system_font_catalog();
#else
        weft::FontCatalog catalog;
#endif
        auto fonts = std::make_shared<const weft::FontCatalog>(std::move(catalog));
        weft::SceneText text(*fonts);
        std::vector<Level> levels = full ? std::vector<Level>{{100}, {400}, {400, true}}
                                         : std::vector<Level>{{100}, {100, true}};
        std::ostringstream out;
        out << "{\"format\":\"weft-view-benchmark-v1\",\"build\":"
#ifdef NDEBUG
            << "\"Release\""
#else
            << "\"Debug\""
#endif
            << ",\"view\":[1920,1080],\"warmup_frames\":" << warmup << ",\"sample_frames\":" << samples
            << ",\"levels\":[";
        for (std::size_t i = 0; i < levels.size(); ++i) {
            auto& level = levels[i];
            run(level, root, text, fonts, warmup, samples);
            out << (i ? "," : "") << "{\"units\":" << level.units << ",\"health\":" << (level.health ? "true" : "false")
                << ",\"nodes\":" << level.nodes << ",\"published\":" << level.published
                << ",\"advance_ms\":" << stats(level.advance) << ",\"copy_ms\":" << stats(level.copy) << '}';
            std::cout << level.units << " units" << (level.health ? ", 20 health changes" : ", anchors only") << ": "
                      << level.nodes << " nodes, advance p50 / p99 " << std::fixed << std::setprecision(3)
                      << percentile(level.advance, .5) << " / " << percentile(level.advance, .99)
                      << " ms, frames published " << level.published << ", copy p50 "
                      << percentile(level.copy, .5) << " ms\n";
        }
        out << "]}";
        const std::filesystem::path folder = argv[2];
        std::filesystem::create_directories(folder);
        std::ofstream(folder / "view.json", std::ios::binary) << out.str();
        std::cout << "PASS view benchmark: " << levels.size() << " levels, " << samples << " frames each\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "view benchmark failed: " << e.what() << '\n';
        return 1;
    }
}
