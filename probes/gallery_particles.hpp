#pragma once

#include "scene/scene_particles.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <vector>

// The UI particles the gallery's particle page names (examples/crpg-gallery/native/PARTICLE_TESTS.js): flames, embers
// and smoke over a campfire, coins and sparks from a chest, an aura around a rune and motes drifting over the page. The coin and
// the spark are sprites drawn here.
namespace probes {
    namespace gallery_sprites {
        constexpr unsigned sprite_size = 32;

        // (x, y) of pixel i's centre in [-1, 1].
        inline std::array<float, 2> unit(unsigned x, unsigned y) {
            return {(x + .5f) / (sprite_size / 2.f) - 1, (y + .5f) / (sprite_size / 2.f) - 1};
        }

        // A gold coin, sRGB straight: a disc with a darker rim, its face lit from the top left, antialiased over a
        // pixel.
        inline std::shared_ptr<const weft::paint::Asset> coin() {
            std::vector<std::uint8_t> rgba(sprite_size * sprite_size * 4);
            constexpr std::array<float, 3> gold{1.f, .78f, .25f};
            for (unsigned y = 0; y < sprite_size; ++y) {
                for (unsigned x = 0; x < sprite_size; ++x) {
                    const auto [u, v] = unit(x, y);
                    const float r = std::hypot(u, v);
                    const float alpha = std::clamp((1 - r) * sprite_size / 2, 0.f, 1.f);
                    const float shade = r > .78f ? .62f : .88f + .3f * std::max(0.f, -(u + v) * .7f);
                    auto* p = rgba.data() + (std::size_t(y) * sprite_size + x) * 4;
                    for (unsigned c = 0; c < 3; ++c) {
                        p[c] = std::uint8_t(std::lround(std::clamp(gold[c] * shade, 0.f, 1.f) * 255));
                    }
                    p[3] = std::uint8_t(std::lround(alpha * 255));
                }
            }
            return std::make_shared<const weft::paint::Asset>(
                weft::paint::Asset{sprite_size, sprite_size, weft::paint::Encoding::SrgbStraight, std::move(rgba)});
        }

        // A four-pointed star with a soft core, coverage.
        inline std::shared_ptr<const weft::paint::Asset> spark() {
            std::vector<std::uint8_t> coverage(sprite_size * sprite_size);
            for (unsigned y = 0; y < sprite_size; ++y) {
                for (unsigned x = 0; x < sprite_size; ++x) {
                    const auto [u, v] = unit(x, y);
                    const float star = 1 - (std::sqrt(std::abs(u)) + std::sqrt(std::abs(v)));
                    const float core = 1 - std::hypot(u, v) * 2.5f;
                    coverage[std::size_t(y) * sprite_size + x] =
                        std::uint8_t(std::lround(std::clamp(std::max(star * 2.5f, core), 0.f, 1.f) * 255));
                }
            }
            return std::make_shared<const weft::paint::Asset>(
                weft::paint::Asset{sprite_size, sprite_size, weft::paint::Encoding::Coverage, std::move(coverage)});
        }
    } // namespace gallery_sprites

    // The registry, built once per process; scenes of every thread read it.
    inline const weft::SceneParticles& gallery_particles() {
        static const auto registry = [] {
            using weft::paint::Color;
            using Shape = weft::paint::ParticleEmitter::Shape;
            auto r = std::make_unique<weft::SceneParticles>();
            const auto add = [&](const char* name, weft::paint::ParticleEmitter e,
                                 std::shared_ptr<const weft::paint::Asset> sprite = {}) {
                if (const auto added = r->add(name, std::move(e), std::move(sprite)); !added) {
                    throw std::runtime_error(added.error().message);
                }
            };
            // Flames: large soft discs rising fast from the fire's base, white-hot to red, shrinking as they fade.
            weft::paint::ParticleEmitter flames;
            flames.shape = Shape::Circle;
            flames.area = {.5f, .82f, .6f, 0};
            flames.rate = 60;
            flames.life = {.45f, .9f};
            flames.speed = {50, 95};
            flames.direction = -90;
            flames.spread = 18;
            flames.gravity = {0, -30};
            flames.size = {36, 30, 18, 6};
            flames.scale = {.7f, 1.2f};
            flames.color = {Color{1, .82f, .4f, .85f}, Color{1, .45f, .1f, .7f}, Color{.75f, .14f, .03f, .4f},
                            Color{.3f, .04f, .02f, 0}};
            flames.softness = 1;
            add("flames", flames);
            // Embers rising from the fire's base, white-hot to red, fading.
            weft::paint::ParticleEmitter embers;
            embers.shape = Shape::Circle;
            embers.area = {.5f, .85f, .55f, 0};
            embers.rate = 70;
            embers.life = {.7f, 1.5f};
            embers.speed = {40, 110};
            embers.direction = -90;
            embers.spread = 36;
            embers.gravity = {0, -60};
            embers.drag = .6f;
            embers.size = {7, 6, 4, 1.5f};
            embers.scale = {.6f, 1.3f};
            embers.color = {Color{1, .9f, .55f, 1}, Color{1, .55f, .12f, .9f}, Color{.85f, .18f, .04f, .6f},
                            Color{.3f, .04f, .02f, 0}};
            embers.softness = .7f;
            add("embers", embers);
            // Smoke above it: large soft grey discs that grow and fade.
            weft::paint::ParticleEmitter smoke;
            smoke.shape = Shape::Rect;
            smoke.area = {.3f, .45f, .4f, .2f};
            smoke.rate = 9;
            smoke.life = {2.2f, 3.2f};
            smoke.speed = {18, 34};
            smoke.direction = -90;
            smoke.spread = 24;
            smoke.gravity = {6, 0};
            smoke.size = {24, 46, 70, 96};
            smoke.color = {Color{.3f, .28f, .27f, 0}, Color{.24f, .22f, .21f, .22f}, Color{.18f, .17f, .17f, .12f},
                           Color{.12f, .12f, .12f, 0}};
            smoke.softness = 1;
            add("smoke", smoke);
            // Coins thrown up from the chest; a burst flies them to the gold counter.
            weft::paint::ParticleEmitter coins;
            coins.area = {.5f, .4f, 0, 0};
            coins.life = {.9f, 1.1f};
            coins.speed = {160, 260};
            coins.direction = -90;
            coins.spread = 110;
            coins.size = {18, 18, 16, 12};
            coins.angle = {0, 360};
            coins.spin = {-540, 540};
            add("coins", coins, gallery_sprites::coin());
            // Sparks around the chest as it opens.
            weft::paint::ParticleEmitter sparks;
            sparks.shape = Shape::Circle;
            sparks.area = {.5f, .45f, .3f, 0};
            sparks.life = {.35f, .75f};
            sparks.speed = {90, 260};
            sparks.spread = 360;
            sparks.drag = 3;
            sparks.size = {14, 12, 8, 0};
            sparks.angle = {0, 90};
            sparks.spin = {-180, 180};
            sparks.color = {Color{1, .95f, .7f, 1}, Color{1, .8f, .3f, 1}, Color{1, .6f, .15f, .8f},
                            Color{1, .4f, .1f, 0}};
            add("sparks", sparks, gallery_sprites::spark());
            // The rune's aura: motes leaving its disc outward.
            weft::paint::ParticleEmitter aura;
            aura.shape = Shape::Circle;
            aura.area = {.5f, .5f, .95f, 0};
            aura.rate = 60;
            aura.life = {1, 2};
            aura.speed = {6, 24};
            aura.spread = 360;
            aura.gravity = {0, -8};
            aura.size = {4, 7, 6, 0};
            aura.color = {Color{.55f, .85f, 1, 0}, Color{.4f, .7f, 1, .9f}, Color{.55f, .35f, 1, .7f},
                          Color{.4f, .2f, .9f, 0}};
            aura.softness = .8f;
            add("aura", aura);
            // Motes drifting over the page; the page sets their rate (the density).
            weft::paint::ParticleEmitter motes;
            motes.shape = Shape::Rect;
            motes.area = {0, 0, 1, 1};
            motes.rate = 300;
            motes.life = {3, 5};
            motes.speed = {4, 18};
            motes.direction = -90;
            motes.spread = 360;
            motes.size = {2, 3.5f, 3.5f, 0};
            motes.scale = {.7f, 1.4f};
            motes.color = {Color{1, .9f, .7f, 0}, Color{1, .85f, .6f, .45f}, Color{.9f, .75f, .55f, .35f},
                           Color{.8f, .65f, .5f, 0}};
            motes.softness = 1;
            add("motes", motes);
            return r;
        }();
        return *registry;
    }
} // namespace probes
