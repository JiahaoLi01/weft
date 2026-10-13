#include "paint/particles.hpp"
#include "render/rounded_clip.hpp"
#include "render/srgb.hpp"
#include "scene/scene_display_list.hpp"
#include "support/check.hpp"
#include "support/d3d11_scene.hpp"
#include "support/scene_fonts.hpp"
#ifdef WEFT_TEST_VULKAN
#include "render/backends/vulkan/scene_renderer.hpp"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <string>

// UI particles in the native scene passes, which composite encoded premultiplied colors: the D3D11 pass on WARP and the
// Vulkan pass on this machine's device (skipped without one), against a CPU raster of the shared model
// (paint/particles.hpp) over the same display list: soft discs of a burst under gravity and drag, a stream and a
// flight to a target, at scale 1 and 2; hard discs under a rotation, a scale and opacity inside a rounded overflow
// clip; sprite frames of a color atlas and a coverage sprite; the two passes against each other.
namespace {
    using weft::testing::require;
    namespace paint = weft::paint;
    using Pixels = std::vector<std::uint8_t>;
    constexpr unsigned width = 96;
    constexpr unsigned height = 64;
    // The CPU raster and the passes differ in rounding alone, away from antialiased edges.
    constexpr int max_raster_difference = 3;

    weft::SceneNode node(std::uint32_t slot, std::array<float, 4> box,
                         std::initializer_list<std::pair<const char*, const char*>> style) {
        weft::SceneNode n;
        n.identity = {1, 1, slot};
        n.tag = "div";
        n.box = box;
        for (const auto& [k, v] : style) {
            n.style[k] = v;
        }
        return n;
    }

    weft::SceneEmission emission(const paint::ParticleEmitter& e, const paint::ParticleEmission& at,
                                 std::shared_ptr<const paint::Asset> sprite = {}) {
        return {std::make_shared<const weft::SceneEmitter>(weft::SceneEmitter{e, std::move(sprite)}), at};
    }

    paint::ParticleEmission burst(std::uint32_t count, double elapsed, std::uint32_t seed) {
        paint::ParticleEmission e;
        e.kind = paint::ParticleEmission::Kind::Burst;
        e.count = count;
        e.elapsed = elapsed;
        e.seed = seed;
        return e;
    }

    // A burst of soft discs from a disc under gravity and drag, a stream from a rectangle, and a flight to a target
    // beyond its element.
    weft::SceneFrame soft() {
        weft::SceneFrame f;
        f.root = node(1, {0, 0, float(width), float(height)}, {});
        paint::ParticleEmitter fire;
        fire.shape = paint::ParticleEmitter::Shape::Circle;
        fire.area = {20, 15, 6, 0};
        fire.life = {1.2f, 1.6f};
        fire.speed = {15, 35};
        fire.direction = -90;
        fire.spread = 300;
        fire.gravity = {0, 25};
        fire.drag = .4f;
        fire.size = {4, 7, 9, 3};
        fire.scale = {.8f, 1.2f};
        fire.color = {paint::Color{1, .55f, .1f, 1}, paint::Color{.9f, .25f, .05f, .8f},
                      paint::Color{.4f, .05f, .3f, .5f}, paint::Color{.1f, 0, .1f, 0}};
        fire.softness = .6f;
        auto a = node(2, {6, 6, 40, 30}, {});
        a.particles = {emission(fire, burst(24, .7, 0x89abcdefu))};
        paint::ParticleEmitter mist;
        mist.shape = paint::ParticleEmitter::Shape::Rect;
        mist.area = {0, 18, 36, 6};
        mist.rate = 40;
        mist.life = {.4f, .6f};
        mist.speed = {10, 30};
        mist.direction = -90;
        mist.spread = 30;
        mist.size = {3, 5, 5, 2};
        mist.color = {paint::Color{.2f, .9f, 1, .9f}, paint::Color{.1f, .5f, 1, .7f}, paint::Color{0, .2f, .8f, .4f},
                      paint::Color{0, 0, .5f, 0}};
        mist.softness = 1;
        auto b = node(3, {52, 6, 36, 24}, {});
        paint::ParticleEmission stream;
        stream.elapsed = 1.7;
        stream.seed = 3;
        b.particles = {emission(mist, stream)};
        paint::ParticleEmitter coins;
        coins.shape = paint::ParticleEmitter::Shape::Point;
        coins.area = {10, 10, 0, 0};
        coins.life = {1, 1};
        coins.speed = {30, 50};
        coins.direction = -45;
        coins.spread = 60;
        coins.size = {5, 5, 5, 5};
        coins.color = {paint::Color{.2f, 1, .3f, 1}, paint::Color{.2f, 1, .3f, 1}, paint::Color{.2f, 1, .3f, 1},
                       paint::Color{.2f, 1, .3f, 1}};
        coins.softness = .5f;
        auto c = node(4, {6, 40, 20, 20}, {});
        auto flight = burst(8, .55, 21);
        flight.duration = .3f;
        flight.target = std::array<float, 2>{70, -10};
        c.particles = {emission(coins, flight)};
        f.root.children = {a, b, c};
        return f;
    }

    // Hard discs of a rotated, scaled element at opacity .6, inside a rounded overflow clip.
    weft::SceneFrame clipped() {
        weft::SceneFrame f;
        f.root = node(1, {0, 0, float(width), float(height)}, {});
        auto clip = node(2, {10, 6, 50, 40},
                         {{"overflow", "hidden"},
                          {"border-top-left-radius", "12px"},
                          {"border-top-right-radius", "12px"},
                          {"border-bottom-right-radius", "12px"},
                          {"border-bottom-left-radius", "12px"}});
        auto spinning = node(3, {5, 5, 30, 20}, {{"transform", "rotate(25deg) scale(1.5)"}, {"opacity", "0.6"}});
        paint::ParticleEmitter sparks;
        sparks.shape = paint::ParticleEmitter::Shape::Edge;
        sparks.area = {0, 0, 30, 20};
        sparks.life = {2, 2};
        sparks.speed = {20, 60};
        sparks.spread = 360;
        sparks.size = {5, 8, 8, 6};
        sparks.color = {paint::Color{1, .9f, .2f, 1}, paint::Color{1, .8f, .2f, 1}, paint::Color{1, .6f, .1f, 1},
                        paint::Color{1, .4f, 0, 1}};
        spinning.particles = {emission(sparks, burst(30, .4, 99))};
        clip.children = {spinning};
        f.root.children = {clip};
        return f;
    }

    // A 16 x 16 atlas of four 8 x 8 frames: red, green, blue and white, opaque.
    std::shared_ptr<const paint::Asset> atlas() {
        std::vector<std::uint8_t> bytes(16 * 16 * 4);
        constexpr std::array<std::array<std::uint8_t, 3>, 4> colors{
            {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}}};
        for (unsigned y = 0; y < 16; ++y) {
            for (unsigned x = 0; x < 16; ++x) {
                const auto& c = colors[(y / 8) * 2 + x / 8];
                std::copy(c.begin(), c.end(), bytes.begin() + (y * 16 + x) * 4);
                bytes[(y * 16 + x) * 4 + 3] = 255;
            }
        }
        return std::make_shared<const paint::Asset>(
            paint::Asset{16, 16, paint::Encoding::SrgbStraight, std::move(bytes)});
    }

    // Coverage: the left half full, the right half a quarter.
    std::shared_ptr<const paint::Asset> halves() {
        std::vector<std::uint8_t> bytes(16 * 16);
        for (unsigned y = 0; y < 16; ++y) {
            for (unsigned x = 0; x < 16; ++x) {
                bytes[y * 16 + x] = x < 8 ? 255 : 64;
            }
        }
        return std::make_shared<const paint::Asset>(paint::Asset{16, 16, paint::Encoding::Coverage, std::move(bytes)});
    }

    // A still particle of an atlas at each of its frames (10 frames per second, ages .05 to .35 s), and one of the
    // coverage sprite, green.
    weft::SceneFrame sprites() {
        weft::SceneFrame f;
        f.root = node(1, {0, 0, float(width), float(height)}, {});
        paint::ParticleEmitter still;
        still.area = {8, 8, 0, 0};
        still.life = {10, 10};
        still.size = {12, 12, 12, 12};
        still.frames = {2, 2};
        still.frame_rate = 10;
        const auto sheet = atlas();
        for (unsigned k = 0; k < 4; ++k) {
            auto n = node(2 + k, {4 + 20.f * k, 4, 16, 16}, {});
            n.particles = {emission(still, burst(1, .05 + .1 * k, 1), sheet)};
            f.root.children.push_back(n);
        }
        paint::ParticleEmitter mask = still;
        mask.size = {16, 16, 16, 16};
        mask.frames = {1, 1};
        mask.frame_rate = 0;
        mask.color = {paint::Color{0, 1, 0, 1}, paint::Color{0, 1, 0, 1}, paint::Color{0, 1, 0, 1},
                      paint::Color{0, 1, 0, 1}};
        auto n = node(6, {4, 30, 16, 16}, {});
        n.particles = {emission(mask, burst(1, .1, 1), halves())};
        f.root.children.push_back(n);
        // The same turned a quarter clockwise: its full half above, its quarter below.
        auto turned = mask;
        turned.angle = {90, 90};
        auto t = node(7, {24, 30, 16, 16}, {});
        t.particles = {emission(turned, burst(1, .1, 1), halves())};
        f.root.children.push_back(t);
        // A box after the particles, in the same pass: drawn as a box.
        f.root.children.push_back(node(8, {44, 30, 8, 8}, {{"background-color", "#0000ff"}}));
        return f;
    }

    // The model's particles of a frame's display list as the passes draw them: in item order, each emission's in
    // instance order, composited as encoded premultiplied 8-bit color, clipped by the item's rectangle and rounded
    // clips, times its opacity. Pixels where the passes' antialiasing decides (a hard disc's rim, a rounded clip's
    // edge) are uncertain.
    struct Raster {
        std::vector<std::array<float, 4>> rgba;
        std::vector<std::uint8_t> uncertain;
    };

    Raster raster(const weft::SceneFrame& frame, unsigned w, unsigned h, float scale) {
        const auto list = weft::scene_display_list(frame, {}, &weft::testing::thread_scene_text(), scale).list;
        require(bool(paint::check(list, 4)), "a well-formed list");
        Raster out{std::vector<std::array<float, 4>>(std::size_t(w) * h),
                   std::vector<std::uint8_t>(std::size_t(w) * h)};
        for (const auto& item : list.items) {
            if (item.kind != paint::Item::Kind::Particles) {
                continue;
            }
            const auto& p = list.particles[item.payload];
            const auto timing = paint::particle_timing(p);
            const auto& m = item.transform;
            const double det = double(m[0]) * m[3] - double(m[1]) * m[2];
            std::vector<weft::RoundedClip> clips;
            for (auto c = item.clips_begin; c < item.clips_begin + item.clips_count; ++c) {
                clips.push_back(weft::normalize_rounded(list.clips[c]));
            }
            const auto inside = [&](double x, double y) {
                return std::all_of(clips.begin(), clips.end(), [&](const auto& c) {
                    return weft::contains_rounded(c, float(x), float(y));
                });
            };
            const bool disc = !p.emitter.sprite;
            for (std::uint32_t instance = 0; instance < timing.count; ++instance) {
                const auto s = paint::particle_state(p, timing, instance);
                if (!s.alive || !(s.size > 0)) {
                    continue;
                }
                const double half = s.size / 2.0;
                const double reach = half + (disc ? 1 / (std::sqrt(std::abs(det)) * scale) : 0);
                const double angle = s.angle * 3.14159265358979 / 180;
                // The disc's coordinate d at a device position, through the inverse of the item's map.
                const auto at = [&](double x, double y, double& d, bool& quad) {
                    const double tx = x / scale - m[4];
                    const double ty = y / scale - m[5];
                    const double ix = (m[3] * tx - m[2] * ty) / det - s.position[0];
                    const double iy = (-m[1] * tx + m[0] * ty) / det - s.position[1];
                    const double qx = (ix * std::cos(angle) + iy * std::sin(angle)) / half;
                    const double qy = (-ix * std::sin(angle) + iy * std::cos(angle)) / half;
                    quad = std::abs(qx) <= reach / half && std::abs(qy) <= reach / half;
                    d = std::hypot(qx, qy);
                };
                for (unsigned y = 0; y < h; ++y) {
                    for (unsigned x = 0; x < w; ++x) {
                        double d{};
                        bool quad{};
                        at(x + .5, y + .5, d, quad);
                        const double tx = (x + .5) / scale;
                        const double ty = (y + .5) / scale;
                        if (!quad || tx < item.clip[0] || ty < item.clip[1] || tx >= item.clip[2] ||
                            ty >= item.clip[3]) {
                            continue;
                        }
                        // Fine derivatives, as the passes take them: across the pixel's 2 x 2 quad, its even pixel
                        // to its odd one.
                        double left{};
                        double right{};
                        double top{};
                        double bottom{};
                        bool unused{};
                        const unsigned qx = x & ~1u;
                        const unsigned qy = y & ~1u;
                        at(qx + .5, y + .5, left, unused);
                        at(qx + 1.5, y + .5, right, unused);
                        at(x + .5, qy + .5, top, unused);
                        at(x + .5, qy + 1.5, bottom, unused);
                        const double aa = std::max(std::abs(right - left) + std::abs(bottom - top), 1e-4);
                        auto& u = out.uncertain[std::size_t(y) * w + x];
                        double shape = 1;
                        if (disc) {
                            const double soft = p.emitter.softness;
                            shape = soft > 0 ? std::clamp((1 - d) / std::max(soft, aa), 0.0, 1.0)
                                             : std::clamp(.5 - (d - 1) / aa, 0.0, 1.0);
                            if (soft == 0 && std::abs(d - 1) < 1.5 * aa) {
                                u = 1;
                            }
                        }
                        if (!clips.empty()) {
                            const bool here = inside(tx, ty);
                            for (const auto& [ox, oy] :
                                 std::array<std::array<double, 2>, 4>{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}}) {
                                if (inside(tx + ox / scale, ty + oy / scale) != here) {
                                    u = 1;
                                }
                            }
                            if (!here) {
                                continue;
                            }
                        }
                        const double alpha = s.color[3] * shape * item.opacity;
                        auto& dst = out.rgba[std::size_t(y) * w + x];
                        std::array<double, 4> src{};
                        for (unsigned c = 0; c < 3; ++c) {
                            src[c] = weft::render::linear_to_srgb(std::clamp(s.color[c], 0.f, 1.f)) * alpha;
                        }
                        src[3] = alpha;
                        for (unsigned c = 0; c < 4; ++c) {
                            dst[c] = float(std::nearbyint((src[c] + dst[c] * (1 - src[3])) * 255) / 255);
                        }
                    }
                }
            }
        }
        return out;
    }

    // The pass's BGRA pixels within max_raster_difference of the raster away from uncertain pixels; the pixels the
    // raster covers.
    std::size_t matches(const char* what, const Pixels& pixels, const Raster& expected, unsigned w) {
        std::size_t covered{};
        for (std::size_t i = 0; i < expected.rgba.size(); ++i) {
            const auto& e = expected.rgba[i];
            covered += e[3] > 0;
            if (expected.uncertain[i]) {
                continue;
            }
            const std::array<int, 4> want{int(std::lround(e[2] * 255)), int(std::lround(e[1] * 255)),
                                          int(std::lround(e[0] * 255)), int(std::lround(e[3] * 255))};
            for (unsigned c = 0; c < 4; ++c) {
                if (std::abs(int(pixels[i * 4 + c]) - want[c]) > max_raster_difference) {
                    std::cerr << what << ": pixel (" << i % w << ", " << i / w << ") is " << int(pixels[i * 4]) << ' '
                              << int(pixels[i * 4 + 1]) << ' ' << int(pixels[i * 4 + 2]) << ' '
                              << int(pixels[i * 4 + 3]) << ", expected " << want[0] << ' ' << want[1] << ' ' << want[2]
                              << ' ' << want[3] << '\n';
                    require(false, std::string(what) + " against the model");
                }
            }
        }
        return covered;
    }

    std::array<int, 4> pixel(const Pixels& pixels, unsigned w, unsigned x, unsigned y) {
        const auto* p = pixels.data() + (std::size_t(y) * w + x) * 4;
        return {p[0], p[1], p[2], p[3]};
    }

    bool close(const std::array<int, 4>& a, const std::array<int, 4>& b, int levels) {
        for (unsigned c = 0; c < 4; ++c) {
            if (std::abs(a[c] - b[c]) > levels) {
                return false;
            }
        }
        return true;
    }

    int difference(const Pixels& a, const Pixels& b) {
        int worst{};
        for (std::size_t i = 0; i < a.size(); ++i) {
            worst = std::max(worst, std::abs(int(a[i]) - int(b[i])));
        }
        return worst;
    }

    using Draw = std::function<Pixels(const weft::SceneFrame&, float scale)>;

    // The checks every pass passes; the frames it drew, for the comparison of passes.
    std::vector<Pixels> check(const char* pass, const Draw& draw) {
        std::vector<Pixels> drawn;
        for (const float scale : {1.f, 2.f}) {
            const auto w = unsigned(width * scale);
            const auto h = unsigned(height * scale);
            const auto f = soft();
            drawn.push_back(draw(f, scale));
            const auto covered = matches(pass, drawn.back(), raster(f, w, h, scale), w);
            std::cout << pass << " soft discs, a stream and a flight at scale " << scale << ": " << covered
                      << " pixels as the model\n";
            require(covered > 300 * scale * scale, std::string(pass) + " particles drawn");
        }
        const auto f = clipped();
        drawn.push_back(draw(f, 1));
        const auto covered = matches(pass, drawn.back(), raster(f, width, height, 1), width);
        std::cout << pass << " hard discs, turned, scaled, at opacity .6 in a rounded clip: " << covered
                  << " pixels as the model\n";
        require(covered > 150, std::string(pass) + " clipped particles drawn");
        // Outside the clip nothing is drawn.
        for (unsigned y = 0; y < height; ++y) {
            for (unsigned x = 0; x < width; ++x) {
                if (x < 10 || x >= 60 || y < 6 || y >= 46) {
                    require(pixel(drawn.back(), width, x, y)[3] == 0, std::string(pass) + " particles clipped");
                }
            }
        }
        // Each frame of the atlas at its particle's centre (BGRA, encoded), and the coverage sprite's two halves.
        drawn.push_back(draw(sprites(), 1));
        const auto& s = drawn.back();
        constexpr std::array<std::array<int, 4>, 4> frames{
            {{0, 0, 255, 255}, {0, 255, 0, 255}, {255, 0, 0, 255}, {255, 255, 255, 255}}};
        for (unsigned k = 0; k < 4; ++k) {
            require(close(pixel(s, width, 12 + 20 * k, 12), frames[k], 2), std::string(pass) + " sprite frame");
        }
        const int quarter = int(std::lround(64 / 255.0 * 255));
        require(close(pixel(s, width, 9, 38), {0, 255, 0, 255}, 2) &&
                    close(pixel(s, width, 15, 38), {0, quarter, 0, quarter}, 2),
                std::string(pass) + " coverage sprite tinted by the color");
        require(close(pixel(s, width, 32, 33), {0, 255, 0, 255}, 2) &&
                    close(pixel(s, width, 32, 43), {0, quarter, 0, quarter}, 2),
                std::string(pass) + " a sprite turned by its angle");
        require(close(pixel(s, width, 48, 34), {255, 0, 0, 255}, 0), std::string(pass) + " a box after particles");
        return drawn;
    }
} // namespace

int main() {
    try {
        // The D3D11 pass keeps the D2D reference raster for color glyph images, which needs COM.
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM init");
        weft::testing::D3D11Scene one(width, height);
        weft::testing::D3D11Scene two(width * 2, height * 2);
        const auto warp = check("D3D11 WARP", [&](const weft::SceneFrame& f, float scale) {
            return scale == 1 ? one.draw(f) : two.draw(f, scale);
        });
        one.gpu.check_debug_messages();
        two.gpu.check_debug_messages();
        std::string vulkan = "skipped";
#ifdef WEFT_TEST_VULKAN
        namespace vk = weft::render::vulkan;
        std::string unavailable;
        if (const auto device = vk::Device::open({}, unavailable)) {
            vk::SceneRenderer scene(*device, weft::testing::thread_scene_text());
            const auto drawn = check("Vulkan", [&](const weft::SceneFrame& f, float scale) {
                auto pixels =
                    scene.draw(f, scale, unsigned(width * scale), unsigned(height * scale), std::nullopt, 0, {});
                require(pixels.has_value(), "native Vulkan pass");
                return std::move(*pixels);
            });
            for (std::size_t i = 0; i < drawn.size(); ++i) {
                const auto d = difference(drawn[i], warp[i]);
                std::cout << "Vulkan vs D3D11 WARP, frame " << i << ": worst " << d << '\n';
                require(d <= 2, "Vulkan against D3D11 WARP");
            }
            require(device->validation_messages == 0, "Vulkan validation");
            vulkan = device->name;
        } else {
            std::cout << "Vulkan skipped: " << unavailable << '\n';
        }
#endif
        // A subtree with particles draws in the pass, never as a cached layer; changed emissions damage the whole
        // target (particles reach past their element); the reference raster refuses particles.
        {
            const auto f = soft();
            const auto listed =
                weft::scene_display_list(f, {f.root.children[0].identity}, &weft::testing::thread_scene_text(), 1).list;
            require(std::none_of(listed.items.begin(), listed.items.end(),
                                 [](const paint::Item& i) {
                                     return i.kind == paint::Item::Kind::Layer;
                                 }) &&
                        listed.particles.size() == 3,
                    "a layer with particles draws in the pass");
            // With each element's geometry, as a solver gives it.
            auto still = f;
            still.elements.push_back({still.root.identity, {0, 0, width, height}});
            for (const auto& child : still.root.children) {
                still.elements.push_back({child.identity, {child.box[0], child.box[1], child.box[2], child.box[3]}});
            }
            auto later = still;
            later.root.children[0].particles[0].emission.elapsed += .1;
            const auto damage = weft::scene_damage(still, later, 1);
            require(damage.changed && damage.full, "moving particles damage the whole target");
            require(!weft::scene_damage(still, still, 1).changed, "still particles damage nothing");
            weft::SceneRaster reference(weft::testing::thread_scene_text());
            bool refused{};
            try {
                (void)reference.raster(f, 1);
            } catch (const std::exception& e) {
                refused = std::string(e.what()).find("particles") != std::string::npos;
            }
            require(refused, "the reference raster refuses particles");
        }
        std::cout << "PASS UI particles in native scene passes: D3D11 WARP, Vulkan " << vulkan
                  << "; soft and hard discs, stream, burst, flight, transform, opacity, rounded clip, sprites\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
