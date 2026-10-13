#include "paint/particles.hpp"

#include "constants/common.hpp"

#include <algorithm>
#include <cmath>

namespace weft::paint {
    namespace {
        // A particle's random values by index; the shader draws the same ones (scene.hlsl, WEFT_PARTICLES).
        enum Draw : std::uint32_t {
            DrawLife,
            DrawStartU,
            DrawStartV,
            DrawSpeed,
            DrawDirection,
            DrawScale,
            DrawAngle,
            DrawSpin,
            DrawFrame,
            DrawEmission,
            // Values per particle in the hash's input.
            Draws = 16
        };

        constexpr float pi = 3.14159265358979f;

        // lowbias32 (Chris Wellons): a full-avalanche 32-bit integer hash.
        std::uint32_t hash(std::uint32_t x) {
            x ^= x >> 16;
            x *= 0x7feb352du;
            x ^= x >> 15;
            x *= 0x846ca68bu;
            x ^= x >> 16;
            return x;
        }

        float mix(float a, float b, float t) {
            return a + (b - a) * t;
        }

        float radians(float degrees) {
            return degrees * (pi / 180);
        }

        // keys at 0, 1/3, 2/3 and 1, linear between, at fraction (clamped to [0, 1]).
        float curve(const std::array<float, 4>& keys, float fraction) {
            const float t = std::clamp(fraction, 0.f, 1.f) * 3;
            const float i = std::min(std::floor(t), 2.f);
            const auto at = std::size_t(i);
            return mix(keys[at], keys[at + 1], t - i);
        }
    } // namespace

    ParticleTiming particle_timing(const Particles& particles) {
        const auto& e = particles.emission;
        ParticleTiming t;
        if (e.kind == ParticleEmission::Kind::Burst) {
            t.count = e.count;
            t.age = float(e.elapsed);
            return t;
        }
        const double rate = particles.emitter.rate;
        if (!(rate > 0)) {
            return t;
        }
        // The newest particle emitted by the elapsed time, or by the stop; its age; and the oldest that may live.
        const double emitting = e.stopped ? std::min(*e.stopped, e.elapsed) : e.elapsed;
        const double newest = std::floor(emitting * rate);
        const double longest = std::max(particles.emitter.life[0], particles.emitter.life[1]);
        const double alive = std::min(newest + 1, std::ceil(longest * rate) + 1);
        t.count = std::uint32_t(std::min<double>(alive, constants::max_particles_per_emission));
        t.age = float(e.elapsed - newest / rate);
        t.interval = float(1 / rate);
        t.newest = std::uint32_t(std::fmod(newest, double(constants::particle_index_period)));
        return t;
    }

    float particle_random(std::uint32_t seed, std::uint32_t particle, std::uint32_t k) {
        return float(hash(seed ^ hash(particle * Draws + k)) >> 8) * (1.f / 16777216.f);
    }

    ParticleState particle_state(const Particles& particles, const ParticleTiming& timing, std::uint32_t instance) {
        const auto& m = particles.emitter;
        const auto& e = particles.emission;
        ParticleState s;
        // The particle's index (its identity for the random values) and age.
        std::uint32_t id{};
        float age{};
        const std::uint32_t seed = e.seed % constants::particle_index_period;
        if (e.kind == ParticleEmission::Kind::Burst) {
            id = instance;
            age = timing.age - e.duration * particle_random(seed, id, DrawEmission);
        } else {
            id = (timing.newest + constants::particle_index_period - instance) % constants::particle_index_period;
            age = timing.age + float(instance) * timing.interval;
        }
        const auto draw = [&](std::uint32_t k) {
            return particle_random(seed, id, k);
        };
        const float life = mix(m.life[0], m.life[1], draw(DrawLife));
        if (!(age >= 0 && age < life)) {
            return s;
        }
        s.alive = true;
        // Where it starts.
        const float u = draw(DrawStartU);
        const float v = draw(DrawStartV);
        std::array<float, 2> start{m.area[0], m.area[1]};
        switch (m.shape) {
            case ParticleEmitter::Shape::Point:
                break;
            case ParticleEmitter::Shape::Rect:
                start = {m.area[0] + m.area[2] * u, m.area[1] + m.area[3] * v};
                break;
            case ParticleEmitter::Shape::Circle: {
                const float r = m.area[2] * std::sqrt(v);
                const float a = 2 * pi * u;
                start = {m.area[0] + r * std::cos(a), m.area[1] + r * std::sin(a)};
                break;
            }
            case ParticleEmitter::Shape::Edge: {
                // Clockwise from the top left corner.
                const float w = m.area[2];
                const float h = m.area[3];
                const float d = u * 2 * (w + h);
                start = d < w           ? std::array<float, 2>{m.area[0] + d, m.area[1]}
                        : d < w + h     ? std::array<float, 2>{m.area[0] + w, m.area[1] + d - w}
                        : d < 2 * w + h ? std::array<float, 2>{m.area[0] + w - (d - w - h), m.area[1] + h}
                                        : std::array<float, 2>{m.area[0], m.area[1] + h - (d - 2 * w - h)};
                break;
            }
        }
        // How it moves.
        const float speed = mix(m.speed[0], m.speed[1], draw(DrawSpeed));
        const float heading = radians(m.direction + (draw(DrawDirection) - .5f) * m.spread);
        const std::array<float, 2> velocity{speed * std::cos(heading), speed * std::sin(heading)};
        const float fraction = age / life;
        if (e.target) {
            // A quadratic curve from the start, leaving along the velocity, to the target, eased in and out.
            const float f = fraction * fraction * (3 - 2 * fraction);
            const std::array<float, 2> control{start[0] + velocity[0] * life * .5f,
                                               start[1] + velocity[1] * life * .5f};
            for (unsigned c = 0; c < 2; ++c) {
                s.position[c] = (1 - f) * (1 - f) * start[c] + 2 * (1 - f) * f * control[c] + f * f * (*e.target)[c];
            }
        } else if (m.drag > constants::particle_drag_epsilon) {
            const float decay = (1 - std::exp(-m.drag * age)) / m.drag;
            for (unsigned c = 0; c < 2; ++c) {
                s.position[c] = start[c] + velocity[c] * decay + m.gravity[c] * (age - decay) / m.drag;
            }
        } else {
            for (unsigned c = 0; c < 2; ++c) {
                s.position[c] = start[c] + velocity[c] * age + .5f * m.gravity[c] * age * age;
            }
        }
        // How it looks.
        s.size = curve(m.size, fraction) * mix(m.scale[0], m.scale[1], draw(DrawScale));
        for (unsigned c = 0; c < 4; ++c) {
            s.color[c] = curve({m.color[0][c], m.color[1][c], m.color[2][c], m.color[3][c]}, fraction);
        }
        s.angle = mix(m.angle[0], m.angle[1], draw(DrawAngle)) + mix(m.spin[0], m.spin[1], draw(DrawSpin)) * age;
        const std::uint32_t frames = m.frames[0] * m.frames[1];
        if (m.sprite && frames > 1) {
            // In integers: a GPU's float division need not round correctly.
            const float pick = m.frame_rate > 0 ? std::floor(age * m.frame_rate) : std::floor(draw(DrawFrame) * frames);
            s.frame = std::uint32_t(pick) % frames;
        }
        return s;
    }
} // namespace weft::paint
