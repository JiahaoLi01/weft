#define NOMINMAX
#include "../probes/gallery_scene_options.hpp"
#include "paint/particles.hpp"
#include "support/check.hpp"
#include "support/d3d11_scene.hpp"
#include "support/png.hpp"
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

// The gallery's UI particle page (11.4c) as a scene: examples/crpg-gallery/native/PARTICLE_TESTS.js with its stylesheet
// and the particles of probes/gallery_particles.hpp. At mount the campfire's flames, embers and smoke, the rune's aura and the
// motes stream from CSS; the chest opens itself at 2.5 s, throwing sparks and coins that fly to the gold counter, which
// counts them as they arrive; a click opens it too; hovering the rune raises its aura's rate; the density spreads the
// motes over layers. The native pass draws the page alike on D3D11 WARP and Vulkan and from two runs of the same clock,
// and the particles show (the frame against itself without them).
namespace {
    using weft::testing::require;

    constexpr float width = 1596;
    constexpr float height = 800;
    constexpr double frame_ms = 1000.0 / 60;
    // The page opens the chest every 2.5 s; the gold counter counts its 24 coins 1.45 s after.
    constexpr double auto_loot_ms = 2500;
    constexpr double coin_flight_ms = 1450;

    struct Rect {
        float x{};
        float y{};
        float w{};
        float h{};
    };

    Rect rect(GallerySceneSession& s, const std::string& selector) {
        std::istringstream in(s.vm.eval("(()=>{const r=document.querySelector('" + selector +
                                        "').getBoundingClientRect();return r.x+' '+r.y+' '+r.width+' '+r.height})()"));
        Rect r;
        in >> r.x >> r.y >> r.w >> r.h;
        return r;
    }

    void run(GallerySceneSession& s, double until) {
        while (s.time + frame_ms <= until + 1e-6) {
            s.step(s.time + frame_ms);
        }
    }

    // The scene nodes of class cls, in tree order.
    void collect(const weft::SceneNode& n, const std::string& cls, std::vector<const weft::SceneNode*>& out) {
        if (const auto c = n.attributes.find("class"); c != n.attributes.end() && c->second == cls) {
            out.push_back(&n);
        }
        for (const auto& child : n.children) {
            collect(child, cls, out);
        }
    }

    std::vector<const weft::SceneNode*> nodes(const weft::SceneFrame& f, const std::string& cls) {
        std::vector<const weft::SceneNode*> out;
        collect(f.root, cls, out);
        return out;
    }

    const std::vector<weft::SceneEmission>& emissions(const weft::SceneFrame& f, const std::string& cls) {
        const auto found = nodes(f, cls);
        require(!found.empty(), "the page has a ." + cls);
        return found.front()->particles;
    }

    std::uint64_t particle_count(const weft::SceneNode& n) {
        std::uint64_t count{};
        for (const auto& e : n.particles) {
            count += weft::paint::particle_timing({e.emitter->emitter, e.emission}).count;
        }
        for (const auto& c : n.children) {
            count += particle_count(c);
        }
        return count;
    }

    void strip(weft::SceneNode& n) {
        n.particles.clear();
        for (auto& c : n.children) {
            strip(c);
        }
    }

    bool close_to(double a, double b, double tolerance = .01) {
        return std::abs(a - b) <= tolerance;
    }

    int difference(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
        int worst{};
        for (std::size_t i = 0; i < a.size(); ++i) {
            worst = std::max(worst, std::abs(int(a[i]) - int(b[i])));
        }
        return worst;
    }

    // Encoded premultiplied BGRA rows as straight RGBA, for evidence.
    void save(const std::filesystem::path& path, const std::vector<std::uint8_t>& bgra) {
        std::vector<std::uint8_t> rgba(bgra.size());
        for (std::size_t i = 0; i < bgra.size(); i += 4) {
            const int a = bgra[i + 3];
            for (unsigned c = 0; c < 3; ++c) {
                rgba[i + c] = std::uint8_t(a ? std::min(255, bgra[i + 2 - c] * 255 / a) : 0);
            }
            rgba[i + 3] = std::uint8_t(a);
        }
        weft::testing::write_png(path, unsigned(width), unsigned(height), rgba);
    }

    // The page stepped to the chest's first opening and past it, as the test below and its twin run it.
    std::unique_ptr<GallerySceneSession> opened() {
        auto s = probes::gallery_scene_session(unsigned(weft::probe_constants::gallery::particle_page_scene));
        run(*s, auto_loot_ms + 30 * frame_ms);
        return s;
    }
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: weft_scene_particle_page_tests <artifact folder>");
        const std::filesystem::path folder = argv[1];
        std::filesystem::create_directories(folder);
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM init");
        const auto& registry = probes::gallery_particles();
        auto s = probes::gallery_scene_session(unsigned(weft::probe_constants::gallery::particle_page_scene));
        require(s->width == width && s->height == height, "the page at its slot's size");

        // At mount, the streams of CSS: the fire's embers, the smoke, the rune's aura and the first motes layer (the
        // others have no rate at the first density).
        run(*s, 1000);
        const auto streams = [&](const std::string& cls, const char* name) {
            const auto& p = emissions(s->current, cls);
            return p.size() == 1 && p[0].emitter == registry.resolve(name, {}) &&
                   p[0].emission.kind == weft::paint::ParticleEmission::Kind::Stream && !p[0].emission.stopped;
        };
        require(streams("pp-fire", "flames") && streams("pp-embers", "embers") && streams("pp-smoke", "smoke") &&
                    streams("pp-rune", "aura"),
                "the campfire's and the rune's streams");
        const auto motes = nodes(s->current, "pp-motes");
        require(motes.size() == 4 && motes[0]->particles.size() == 1 &&
                    motes[0]->particles[0].emitter->emitter.rate == 300 && motes[1]->particles.empty() &&
                    motes[3]->particles.empty() && s->current.animated,
                "the motes at the first density");
        std::cout << "streams at 1 s: " << particle_count(s->current.root) << " particles\n";

        // Where the gallery window's test clicks the chest and looks for the fire (probes/gallery_preview_probe.cpp).
        {
            const auto chest = rect(*s, ".pp-chest");
            const auto fire = rect(*s, ".pp-fire");
            std::cout << "chest (" << chest.x << ", " << chest.y << ", " << chest.w << ", " << chest.h << "), fire ("
                      << fire.x << ", " << fire.y << ", " << fire.w << ", " << fire.h << ")\n";
            require(close_to(chest.x + chest.w / 2, 798, .5) && close_to(chest.y + chest.h / 2, 426, .5) &&
                        close_to(fire.x, 220.33, .5) && close_to(fire.y, 460.5, .5) && fire.w == 110 && fire.h == 120,
                    "the chest and the fire where the window's test expects them");
        }

        // The chest opens itself: sparks around it, and coins that fly to the gold counter's coin.
        run(*s, auto_loot_ms + 30 * frame_ms);
        {
            const auto& p = emissions(s->current, "pp-chest");
            const auto chest = rect(*s, ".pp-chest");
            const auto coin = rect(*s, ".pp-coin");
            require(p.size() == 2 && p[0].emitter == registry.resolve("sparks", {}) && p[0].emission.count == 40 &&
                        !p[0].emission.target && p[1].emitter == registry.resolve("coins", {}) &&
                        p[1].emission.count == 24 && p[1].emission.target,
                    "the chest's sparks and coins");
            const std::array<double, 2> expected{coin.x + coin.w / 2 - chest.x, coin.y + coin.h / 2 - chest.y};
            std::cout << "coins fly to (" << (*p[1].emission.target)[0] << ", " << (*p[1].emission.target)[1]
                      << ") of the chest, expected (" << expected[0] << ", " << expected[1] << ")\n";
            require(close_to((*p[1].emission.target)[0], expected[0]) &&
                        close_to((*p[1].emission.target)[1], expected[1]),
                    "coins fly to the counter's coin");
            require(s->vm.eval("document.querySelector('.pp-count').textContent") == "0", "no coins counted yet");
        }

        // The native pass at this frame: D3D11 WARP, and Vulkan alike; the particles show.
        weft::testing::D3D11Scene d3d11{unsigned(width), unsigned(height)};
        const auto drawn = d3d11.draw(s->current);
        save(folder / "loot.png", drawn);
        auto bare = s->current;
        strip(bare.root);
        const auto without = d3d11.draw(bare);
        d3d11.gpu.check_debug_messages();
        std::size_t shown{};
        for (std::size_t i = 0; i < drawn.size(); i += 4) {
            shown += std::abs(int(drawn[i]) - int(without[i])) + std::abs(int(drawn[i + 1]) - int(without[i + 1])) +
                         std::abs(int(drawn[i + 2]) - int(without[i + 2])) >
                     24;
        }
        std::cout << "pixels the particles change: " << shown << '\n';
        require(shown > 5000, "the particles show");
        // Each coin in flight where the model puts it: the frame differs there from the frame without particles, but
        // under the chest's lid and lock, its content, which covers its particles.
        {
            const auto chest = rect(*s, ".pp-chest");
            const auto lid = rect(*s, ".pp-lid");
            const auto lock = rect(*s, ".pp-lock");
            const std::array<Rect, 2> content{lid, lock};
            const auto covered = [&](int x, int y) {
                return std::any_of(content.begin(), content.end(), [&](const Rect& r) {
                    return x >= r.x - 1 && x <= r.x + r.w + 1 && y >= r.y - 1 && y <= r.y + r.h + 1;
                });
            };
            const auto& coins = emissions(s->current, "pp-chest")[1];
            weft::paint::Particles model{coins.emitter->emitter, coins.emission};
            model.emitter.area = weft::scene_emitter_area(model.emitter, chest.w, chest.h);
            const auto timing = weft::paint::particle_timing(model);
            unsigned alive{};
            unsigned seen{};
            for (std::uint32_t i = 0; i < timing.count; ++i) {
                const auto state = weft::paint::particle_state(model, timing, i);
                const auto x = int(chest.x + state.position[0]);
                const auto y = int(chest.y + state.position[1]);
                if (!state.alive || x < 0 || y < 0 || x >= int(width) || y >= int(height) || covered(x, y)) {
                    continue;
                }
                ++alive;
                const auto at = (std::size_t(y) * std::size_t(width) + std::size_t(x)) * 4;
                int change{};
                for (unsigned c = 0; c < 3; ++c) {
                    change += std::abs(int(drawn[at + c]) - int(without[at + c]));
                }
                seen += change > 60;
            }
            std::cout << "coins in flight " << alive << ", drawn where the model puts them " << seen << '\n';
            require(alive >= 12 && seen == alive, "the coins drawn in flight");
        }
        // Where the fire burns, embers brighten the stage.
        {
            const auto fire = rect(*s, ".pp-fire");
            int brightest{};
            for (int y = int(fire.y); y < int(fire.y + fire.h); ++y) {
                for (int x = int(fire.x); x < int(fire.x + fire.w); ++x) {
                    const auto* p = drawn.data() + (std::size_t(y) * std::size_t(width) + std::size_t(x)) * 4;
                    brightest = std::max(brightest, int(p[2]));
                }
            }
            std::cout << "brightest red in the fire: " << brightest << '\n';
            require(brightest > 200, "embers in the fire");
        }
        std::string vulkan = "skipped";
#ifdef WEFT_TEST_VULKAN
        {
            namespace vk = weft::render::vulkan;
            std::string unavailable;
            if (const auto device = vk::Device::open({}, unavailable)) {
                vk::SceneRenderer scene(*device, weft::testing::thread_scene_text());
                const auto pixels = scene.draw(s->current, 1, unsigned(width), unsigned(height), std::nullopt, 0, {});
                require(pixels.has_value(), "native Vulkan pass");
                // As the gallery's scenes compare (probes/scene_vulkan_probe.cpp): rounding apart, a few pixels of
                // gradients and antialiased edges differ more.
                const auto d = difference(*pixels, drawn);
                std::size_t beyond{};
                for (std::size_t i = 0; i < drawn.size(); i += 4) {
                    int worst{};
                    for (unsigned c = 0; c < 4; ++c) {
                        worst = std::max(worst, std::abs(int((*pixels)[i + c]) - int(drawn[i + c])));
                    }
                    beyond += worst > weft::probe_constants::gallery::vulkan_scene_near_level;
                }
                std::cout << "Vulkan vs D3D11 WARP: worst " << d << ", pixels past "
                          << weft::probe_constants::gallery::vulkan_scene_near_level << " levels " << beyond << '\n';
                require(d <= weft::probe_constants::gallery::max_vulkan_scene_difference &&
                            beyond <= weft::probe_constants::gallery::max_vulkan_scene_far_pixels,
                        "Vulkan draws the page as D3D11");
                require(device->validation_messages == 0, "Vulkan validation");
                vulkan = device->name;
            } else {
                std::cout << "Vulkan skipped: " << unavailable << '\n';
            }
        }
#endif
        // The same clock gives the same page: another session, stepped alike, builds an equal frame.
        {
            const auto twin = opened();
            require(twin->current.root == s->current.root, "two runs of the same clock alike");
            require(d3d11.draw(twin->current) == drawn, "and draw alike");
        }

        // The counter counts the coins as they arrive.
        run(*s, auto_loot_ms + coin_flight_ms + 10 * frame_ms);
        require(s->vm.eval("document.querySelector('.pp-count').textContent") == "24", "the coins counted");
        // A click opens the chest too.
        {
            const auto chest = rect(*s, ".pp-chest");
            s->input_value(weft::MouseType::Down, chest.x + chest.w / 2, chest.y + chest.h / 2);
            s->input_value(weft::MouseType::Up, chest.x + chest.w / 2, chest.y + chest.h / 2);
            run(*s, s->time + frame_ms);
            const auto& p = emissions(s->current, "pp-chest");
            // The first opening's particles have died.
            require(p.size() == 2 && close_to(p[1].emission.elapsed, 0, 1e-9) && p[1].emission.target &&
                        s->vm.eval("__ceParticlePage.clicks()") == "1",
                    "a click throws coins");
        }
        // Hovering the rune raises its aura's rate: the old stream lives out its motes beside the new one.
        {
            const auto rune = rect(*s, ".pp-rune");
            s->input_value(weft::MouseType::Move, rune.x + rune.w / 2, rune.y + rune.h / 2);
            run(*s, s->time + 2 * frame_ms);
            const auto& p = emissions(s->current, "pp-rune");
            require(p.size() == 2 && p[0].emission.stopped && p[1].emitter->emitter.rate == 240,
                    "hovering the rune raises its rate");
            s->input_value(weft::MouseType::Move, 4, 4);
        }
        // The density spreads the motes over layers of at most 12000 per second.
        s->vm.eval("__ceParticlePage.setDensity(30000)");
        run(*s, s->time + 6000);
        {
            const auto layers = nodes(s->current, "pp-motes");
            require(layers[0]->particles.back().emitter->emitter.rate == 12000 &&
                        layers[1]->particles.back().emitter->emitter.rate == 12000 &&
                        layers[2]->particles.back().emitter->emitter.rate == 6000 && layers[3]->particles.empty() &&
                        s->vm.eval("document.querySelector('.pp-density').textContent") == "Motes 30000 / s",
                    "the density over layers");
            const auto count = particle_count(s->current.root);
            std::cout << "at 30000 motes per second: " << count << " particles drawn\n";
            require(count > 140000 && count <= weft::constants::scene::max_particles_per_frame,
                    "the frame's particles at the density");
            save(folder / "dense.png", d3d11.draw(s->current));
        }
        std::cout << "PASS UI particle page: streams, auto and clicked loot flying to the counter, hover, density; "
                     "D3D11 WARP, Vulkan "
                  << vulkan << ", same clock same pixels\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
