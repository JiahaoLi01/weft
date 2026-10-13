#define NOMINMAX
#include "../probes/gallery_cluster_input.hpp"
#include "../probes/gallery_scene_options.hpp"
#include "scene_test_image.hpp"
#include "support/check.hpp"
#include "support/d3d11_scene.hpp"
#include "support/files.hpp"
#include "support/json_value.hpp"
#ifdef WEFT_TEST_VULKAN
#include "render/backends/vulkan/scene_renderer.hpp"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// The gallery's translucent window page (11.5c) as scenes: examples/crpg-gallery/native/GLASS_TESTS.js with its
// stylesheet, one case each for frosted, plain translucent and blur-only windows. Each mounts four windows over a
// background that moves every frame; + and − change the count within 1 to 32; pressing a title bar brings its window to
// the front and drags it, within the page, until the pointer is released or leaves. Frosted and blur-only windows filter
// their backdrop, plain ones do not. Then the acceptance against Chrome (tools/browser_glass_page_reference.mjs,
// tests/fixtures/glass-reference): each case's frames and inputs (examples/crpg-gallery/glass-cases.json), and at each
// capture the windows' titles and rects as Chrome's, the frame by the gallery's scene rule, and the windows' clear
// glass (or tint) within a few levels of Chrome's. D3D11 WARP and Vulkan draw the page alike.
namespace {
    using weft::testing::require;

    constexpr float width = 1596;
    constexpr float height = 800;
    constexpr double frame_ms = 1000.0 / 60;
    constexpr const char* kinds[]{"frosted", "plain", "blur"};
    // The page's first windows and its bounds.
    constexpr int first_windows = 4;
    constexpr int max_windows = 32;
    // The windows' clear glass (or tint) against Chrome: the mean and, where it blurs, the worst RGB level. The blur's
    // scaling leaves a few levels (tests/scene_backdrop_reference_tests.cpp); the windows' text, controls and edges,
    // antialiased differently, are left out.
    constexpr double max_glass_mean = 2;
    constexpr int max_glass_worst = 16;
    // Around the windows' text and controls, and inside their edges, in px.
    constexpr double content_margin = 3;
    constexpr double edge_margin = 4;
    // The windows' corner radius (glass-utilities.css).
    constexpr double window_radius = 16;
    // Window rects as Chrome's: they are whole px, from the page's arithmetic.
    constexpr double max_rect_difference = .01;

    struct Window {
        std::string title;
        std::array<double, 4> rect{};
        // Its text and controls: the dots, the title, the rows and the meter.
        std::vector<std::array<double, 4>> content;
    };

    struct State {
        std::string kind;
        int count{};
        int drags{};
        std::vector<Window> windows;
    };

    State state(const weft::testing::JsonValue& v) {
        State s{v["kind"].string, int(v["count"].number), int(v["drags"].number), {}};
        for (std::size_t i = 0; i < v["windows"].size(); ++i) {
            const auto& w = v["windows"][i];
            s.windows.push_back({w["title"].string,
                                 {w["rect"][0].number, w["rect"][1].number, w["rect"][2].number, w["rect"][3].number}});
        }
        return s;
    }

    State state(GallerySceneSession& s) {
        return state(weft::testing::parse_json(s.vm.eval("__ceGlassPage.state()")));
    }

    // The windows' text and controls in page px (the page fills the scene), in the windows' order.
    void content(GallerySceneSession& s, State& st) {
        const auto v = weft::testing::parse_json(
            s.vm.eval("JSON.stringify([...document.querySelectorAll('.gw-window')].map(w=>[...w.querySelectorAll("
                      "'.gw-dot,.gw-name,.gw-row,.gw-meter,.gw-sheen')].map(n=>{const r=n.getBoundingClientRect();"
                      "return [r.x,r.y,r.width,r.height]})))"));
        require(v.size() == st.windows.size(), "each window's content");
        for (std::size_t i = 0; i < v.size(); ++i) {
            for (std::size_t k = 0; k < v[i].size(); ++k) {
                st.windows[i].content.push_back(
                    {v[i][k][0].number, v[i][k][1].number, v[i][k][2].number, v[i][k][3].number});
            }
        }
    }

    std::array<float, 4> rect(GallerySceneSession& s, const std::string& selector) {
        std::istringstream in(s.vm.eval("(()=>{const r=document.querySelector('" + selector +
                                        "').getBoundingClientRect();return r.x+' '+r.y+' '+r.width+' '+r.height})()"));
        std::array<float, 4> r{};
        in >> r[0] >> r[1] >> r[2] >> r[3];
        return r;
    }

    void click(GallerySceneSession& s, const std::string& selector) {
        const auto r = rect(s, selector);
        s.input_value(weft::MouseType::Down, r[0] + r[2] / 2, r[1] + r[3] / 2);
        s.input_value(weft::MouseType::Up, r[0] + r[2] / 2, r[1] + r[3] / 2);
        s.step(s.time + frame_ms);
    }

    std::unique_ptr<GallerySceneSession> session(unsigned kind) {
        return probes::gallery_scene_session(unsigned(weft::probe_constants::gallery::glass_page_scene) + kind);
    }

    weft::SceneBitmap image(const std::vector<std::uint8_t>& bgra) {
        weft::SceneBitmap out{unsigned(width), unsigned(height), bgra};
        for (std::size_t i = 0; i < out.rgba.size(); i += 4) {
            std::swap(out.rgba[i], out.rgba[i + 2]);
        }
        return out;
    }

    // The windows, frames, controls and backdrops of one case.
    void behaviour(unsigned kind, weft::testing::D3D11Scene& d3d11) {
        const std::string name = kinds[kind];
        auto s = session(kind);
        s->step(0);
        auto st = state(*s);
        require(st.kind == name && st.count == first_windows && st.windows.size() == std::size_t(first_windows),
                name + ": four windows at first");
        // Scattered over the page in their order, each inside it.
        for (std::size_t i = 0; i < st.windows.size(); ++i) {
            const auto& r = st.windows[i].rect;
            require(r[0] == 48 + double(i) * 157 && r[1] == 48 + double(i) * 71 && r[2] == 360 && r[3] == 232,
                    name + ": a window's place");
        }
        require(s->vm.eval("JSON.stringify(__ceGallery.state().errors)") == "[]", name + ": no errors");
        // The window probe's + (probes/gallery_constants.hpp) is this one's centre.
        const auto more = rect(*s, ".gw-more");
        require(std::abs(more[0] + more[2] / 2 - weft::probe_constants::gallery::glass_more_button[0]) <= 4 &&
                    std::abs(more[1] + more[3] / 2 - weft::probe_constants::gallery::glass_more_button[1]) <= 4,
                name + ": the + where the window probe clicks it");
        // Frosted and blur-only windows filter their backdrop, one pass each; plain ones none.
        const auto before = d3d11.scene.backdrop_passes;
        const auto first = d3d11.draw(s->current);
        require(d3d11.scene.backdrop_passes - before == (kind == 1 ? 0u : std::size_t(first_windows)),
                name + ": a backdrop for each glass window");
        // The background moves every frame; the windows stay.
        s->step(frame_ms);
        const auto next = d3d11.draw(s->current);
        std::size_t moved{};
        for (std::size_t i = 0; i < first.size(); i += 4) {
            moved += first[i] != next[i] || first[i + 1] != next[i + 1] || first[i + 2] != next[i + 2];
        }
        std::cout << name << ": " << moved << " pixels moved in one frame\n";
        require(moved > 2000, name + ": the background moves every frame");
        require(state(*s).windows.back().rect == st.windows.back().rect, name + ": the windows stay");
        // + and − within 1 to 32.
        click(*s, ".gw-more");
        require(state(*s).count == first_windows + 1 &&
                    s->vm.eval("document.querySelector('.gw-count').textContent") == "Windows 5",
                name + ": + adds a window");
        click(*s, ".gw-less");
        click(*s, ".gw-less");
        require(state(*s).count == first_windows - 1, name + ": − removes the newest");
        s->vm.eval("__ceGlassPage.setCount(1)");
        click(*s, ".gw-less");
        require(state(*s).count == 1, name + ": at least one window");
        s->vm.eval("__ceGlassPage.setCount(99)");
        s->step(s->time + frame_ms);
        require(state(*s).count == max_windows, name + ": at most 32");
        click(*s, ".gw-more");
        require(state(*s).count == max_windows, name + ": + stops at 32");
        s->vm.eval("__ceGlassPage.setCount(4)");
        s->step(s->time + frame_ms);
        // Pressing the bottom window's title bar brings it to the front; it follows the pointer until the release.
        s->input_value(weft::MouseType::Down, 264, 68);
        require(state(*s).windows.back().title == "Inventory 1", name + ": a pressed window comes to the front");
        s->input_value(weft::MouseType::Move, 384, 128, 1);
        s->input_value(weft::MouseType::Move, 504, 188, 1);
        s->input_value(weft::MouseType::Up, 504, 188);
        s->input_value(weft::MouseType::Move, 900, 600);
        s->step(s->time + frame_ms);
        st = state(*s);
        require(st.drags == 1 && st.windows.back().rect == std::array<double, 4>{288, 168, 360, 232},
                name + ": dragged by its title bar until released");
        // Pressing its body does not drag it.
        s->input_value(weft::MouseType::Down, 400, 300);
        s->input_value(weft::MouseType::Move, 500, 400, 1);
        s->input_value(weft::MouseType::Up, 500, 400);
        s->step(s->time + frame_ms);
        require(state(*s).windows.back().rect == std::array<double, 4>{288, 168, 360, 232} && state(*s).drags == 1,
                name + ": the body does not drag");
        // A drag stays within the page: at most half a window past either side, its title bar above the bottom.
        s->input_value(weft::MouseType::Down, 400, 188);
        s->input_value(weft::MouseType::Move, width - 6, height - 5, 1);
        s->input_value(weft::MouseType::Up, width - 6, height - 5);
        s->step(s->time + frame_ms);
        const auto at = state(*s).windows.back().rect;
        require(at == std::array<double, 4>{width - 180, height - 40, 360, 232}, name + ": a drag within the page");
        // A pointer that leaves the page ends the drag, and so does a cancelled one: moves after them move nothing.
        s->input_value(weft::MouseType::Down, float(at[0] + 100), float(at[1] + 20));
        s->input_value(weft::MouseType::Move, 3000, 3000, 1);
        s->input_value(weft::MouseType::Move, 800, 400, 1);
        s->input_value(weft::MouseType::Up, 800, 400);
        s->step(s->time + frame_ms);
        require(state(*s).windows.back().rect == at && state(*s).drags == 3, name + ": leaving ends the drag");
        s->input_value(weft::MouseType::Down, float(at[0] + 100), float(at[1] + 20));
        s->cancel_input();
        s->input_value(weft::MouseType::Move, 100, 100, 1);
        s->step(s->time + frame_ms);
        require(state(*s).windows.back().rect == at && state(*s).drags == 4, name + ": a cancel ends the drag");
        require(s->vm.eval("JSON.stringify(__ceGallery.state().errors)") == "[]", name + ": no errors after input");
    }

    struct Glass {
        double mean{};
        int worst{};
        std::size_t pixels{};
    };

    // The clear glass (or tint): the pixels of each window where it shows (no later window over it), inside its edges
    // and away from its text and controls.
    Glass glass(const State& st, const weft::SceneBitmap& ours, const weft::SceneBitmap& chrome) {
        Glass g;
        double sum{};
        for (unsigned y = 0; y < unsigned(height); ++y) {
            for (unsigned x = 0; x < unsigned(width); ++x) {
                const double px = x + .5;
                const double py = y + .5;
                int top = -1;
                for (std::size_t i = 0; i < st.windows.size(); ++i) {
                    const auto& r = st.windows[i].rect;
                    if (px >= r[0] && px < r[0] + r[2] && py >= r[1] && py < r[1] + r[3]) {
                        top = int(i);
                    }
                }
                if (top < 0) {
                    continue;
                }
                const auto& w = st.windows[std::size_t(top)];
                const auto& r = w.rect;
                if (px < r[0] + edge_margin || px > r[0] + r[2] - edge_margin || py < r[1] + edge_margin ||
                    py > r[1] + r[3] - edge_margin) {
                    continue;
                }
                // In a corner, as far inside its arc.
                const double cx = std::clamp(px, r[0] + window_radius, r[0] + r[2] - window_radius);
                const double cy = std::clamp(py, r[1] + window_radius, r[1] + r[3] - window_radius);
                if (std::hypot(px - cx, py - cy) > window_radius - edge_margin) {
                    continue;
                }
                if (std::any_of(w.content.begin(), w.content.end(), [&](const auto& c) {
                        return px > c[0] - content_margin && px < c[0] + c[2] + content_margin &&
                               py > c[1] - content_margin && py < c[1] + c[3] + content_margin;
                    })) {
                    continue;
                }
                const auto at = (std::size_t(y) * unsigned(width) + x) * 4;
                int worst{};
                for (unsigned c = 0; c < 3; ++c) {
                    const int d = std::abs(int(ours.rgba[at + c]) - int(chrome.rgba[at + c]));
                    sum += d;
                    worst = std::max(worst, d);
                }
                g.worst = std::max(g.worst, worst);
                ++g.pixels;
            }
        }
        g.mean = g.pixels ? sum / (3.0 * double(g.pixels)) : 0;
        return g;
    }

    // Each case's frames and inputs, compared with Chrome at its captures.
    void replay(const std::filesystem::path& out, weft::testing::D3D11Scene& d3d11) {
        const auto directory = weft::testing::source_path("tests/fixtures/glass-reference");
        const auto report = weft::testing::parse_json(weft::testing::read_file(directory / "report.json"));
        const auto spec = scene_read(WEFT_SOURCE_DIR "/examples/crpg-gallery/glass-cases.json");
        std::size_t compared{};
        require(report["cases"].size() == 3, "Chrome's three cases");
        for (std::size_t c = 0; c < report["cases"].size(); ++c) {
            const auto& chrome_case = report["cases"][c];
            const auto id = chrome_case["id"].string;
            const auto kind = unsigned(chrome_case["index"].number);
            auto s = session(kind);
            s->vm.eval("globalThis.__ceClusterSpec=" + spec + ";globalThis.__ceClusterInputs=__ceClusterSpec.inputs['" +
                       id + "']||[]");
            const auto& captures = chrome_case["captures"].array;
            std::size_t next{};
            for (unsigned frame = 0; frame < unsigned(report["frames"].number) && next < captures.size(); ++frame) {
                s->step(frame * frame_ms);
                gallery_cluster_inputs(*s, frame);
                if (unsigned(captures[next]["frame"].number) != frame) {
                    continue;
                }
                const auto& capture = captures[next++];
                const auto name = id + "-" + std::to_string(frame);
                // The windows: their titles, order and rects.
                auto ours = state(*s);
                content(*s, ours);
                const auto theirs = state(capture["state"]);
                require(ours.count == theirs.count && ours.drags == theirs.drags &&
                            ours.windows.size() == theirs.windows.size(),
                        name + ": the windows as Chrome's");
                double worst_rect{};
                for (std::size_t i = 0; i < ours.windows.size(); ++i) {
                    require(ours.windows[i].title == theirs.windows[i].title, name + ": the windows' order");
                    for (unsigned k = 0; k < 4; ++k) {
                        worst_rect =
                            std::max(worst_rect, std::abs(ours.windows[i].rect[k] - theirs.windows[i].rect[k]));
                    }
                }
                require(worst_rect <= max_rect_difference, name + ": the windows' rects as Chrome's");
                // The frame, and the glass.
                const auto drawn = image(d3d11.draw(s->current));
                png(out / (name + ".png"), drawn);
                const auto reference = scene_reference(directory / (name + ".png"));
                const auto d = scene_difference(drawn, reference);
                const auto g = glass(ours, drawn, reference);
                std::printf("%s: %zu windows; pixel means %.2f %.2f %.2f, soft IoU %.3f; clear glass %zu px, mean "
                            "%.2f, worst %d\n",
                            name.c_str(), ours.windows.size(), d.mean[0], d.mean[1], d.mean[2], d.soft_iou, g.pixels,
                            g.mean, g.worst);
                require(d.passes(), name + ": the native pass as Chrome's screenshot");
                // A plain window's tint shows what lies behind it sharp, whose edges antialias differently: its mean only.
                require(g.pixels > 50000 && g.mean <= max_glass_mean && (kind == 1 || g.worst <= max_glass_worst),
                        name + ": the windows' glass as Chrome's");
                ++compared;
            }
            require(next == captures.size(), id + ": every capture compared");
            require(s->vm.eval("JSON.stringify(__ceGallery.state().errors)") == "[]", id + ": no errors");
        }
        require(compared == 12, "twelve captures compared");
    }
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: weft_scene_glass_page_tests <artifact folder>");
        const std::filesystem::path folder = argv[1];
        std::filesystem::create_directories(folder);
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM init");
        weft::testing::D3D11Scene d3d11{unsigned(width), unsigned(height)};
        for (unsigned kind = 0; kind < 3; ++kind) {
            behaviour(kind, d3d11);
        }
        replay(folder, d3d11);
        // Vulkan draws each case as D3D11 does, by the gallery's scene rule (probes/scene_vulkan_probe.cpp).
        std::string vulkan = "skipped";
#ifdef WEFT_TEST_VULKAN
        {
            namespace vk = weft::render::vulkan;
            std::string unavailable;
            if (const auto device = vk::Device::open({}, unavailable)) {
                vk::SceneRenderer scene(*device, weft::testing::thread_scene_text());
                for (unsigned kind = 0; kind < 3; ++kind) {
                    auto s = session(kind);
                    s->step(600);
                    const auto drawn = d3d11.draw(s->current);
                    const auto pixels =
                        scene.draw(s->current, 1, unsigned(width), unsigned(height), std::nullopt, 0, {});
                    require(pixels.has_value(), "native Vulkan pass");
                    int worst{};
                    std::size_t beyond{};
                    for (std::size_t i = 0; i < drawn.size(); i += 4) {
                        int off{};
                        for (unsigned c = 0; c < 4; ++c) {
                            off = std::max(off, std::abs(int((*pixels)[i + c]) - int(drawn[i + c])));
                        }
                        worst = std::max(worst, off);
                        beyond += off > weft::probe_constants::gallery::vulkan_scene_near_level;
                    }
                    std::cout << kinds[kind] << ": Vulkan vs D3D11 WARP worst " << worst << ", pixels past "
                              << weft::probe_constants::gallery::vulkan_scene_near_level << " levels " << beyond
                              << '\n';
                    require(worst <= weft::probe_constants::gallery::max_vulkan_scene_difference &&
                                beyond <= weft::probe_constants::gallery::max_vulkan_scene_far_pixels,
                            std::string(kinds[kind]) + ": Vulkan draws the page as D3D11");
                }
                require(device->validation_messages == 0, "Vulkan validation");
                vulkan = device->name;
            } else {
                std::cout << "Vulkan skipped: " << unavailable << '\n';
            }
        }
#endif
        d3d11.gpu.check_debug_messages();
        std::cout << "PASS translucent window page: frosted, plain and blur-only windows, moving background, + and −, "
                     "drag to the front; 12 captures against Chrome; D3D11 WARP, Vulkan "
                  << vulkan << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
