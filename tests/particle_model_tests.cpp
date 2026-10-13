#include "constants/common.hpp"
#include "paint/particles.hpp"
#include "support/check.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <set>

// The particle model (src/paint/particles.hpp), which the scene shader computes alike: random values, the timing of
// streams and bursts (exact far into a long stream), which particles live, a particle's identity from frame to frame,
// its motion against a numerical integration, where it starts in each shape, its curves, frames and flight to a target.
namespace {
    using weft::testing::require;
    namespace paint = weft::paint;

    paint::Particles stream(float rate, double elapsed) {
        paint::Particles p;
        p.emitter.rate = rate;
        p.emitter.life = {1, 1};
        p.emission.kind = paint::ParticleEmission::Kind::Stream;
        p.emission.elapsed = elapsed;
        p.emission.seed = 77;
        return p;
    }

    paint::Particles burst(std::uint32_t count, double elapsed) {
        paint::Particles p;
        p.emitter.life = {2, 2};
        p.emission.kind = paint::ParticleEmission::Kind::Burst;
        p.emission.count = count;
        p.emission.elapsed = elapsed;
        p.emission.seed = 5;
        return p;
    }

    paint::ParticleState state(const paint::Particles& p, std::uint32_t instance) {
        return paint::particle_state(p, paint::particle_timing(p), instance);
    }

    bool near(float a, float b, float tolerance) {
        return std::abs(a - b) <= tolerance;
    }

    void random_values() {
        double sum{};
        double squares{};
        constexpr unsigned draws = 100000;
        std::set<float> distinct;
        for (unsigned i = 0; i < draws; ++i) {
            const float v = paint::particle_random(1234, i, 3);
            require(v >= 0 && v < 1, "random values in [0, 1)");
            require(v == paint::particle_random(1234, i, 3), "random values repeat");
            sum += v;
            squares += double(v) * v;
            distinct.insert(v);
        }
        const double mean = sum / draws;
        const double variance = squares / draws - mean * mean;
        std::cout << "random: mean " << mean << ", variance " << variance << ", distinct " << distinct.size() << '\n';
        require(std::abs(mean - .5) < .005 && std::abs(variance - 1.0 / 12) < .002 &&
                    distinct.size() > draws * 99 / 100,
                "uniform random values");
        require(paint::particle_random(1234, 7, 3) != paint::particle_random(1234, 7, 4) &&
                    paint::particle_random(1234, 7, 3) != paint::particle_random(1235, 7, 3) &&
                    paint::particle_random(1234, 7, 3) != paint::particle_random(1234, 8, 3),
                "each value, seed and particle draws its own");
    }

    void timing() {
        // 24 emitted by 2.35 s at 10 per second, the newest 50 ms old; one-second lives keep 11 at most.
        auto s = stream(10, 2.35);
        auto t = paint::particle_timing(s);
        require(t.count == 11 && t.newest == 23 && near(t.age, .05f, 1e-6f) && near(t.interval, .1f, 1e-7f),
                "a stream's timing");
        require(state(s, 0).alive && state(s, 9).alive && !state(s, 10).alive,
                "the newest particles live, the one emitted a second before them is gone");
        // Early on, only those emitted so far.
        require(paint::particle_timing(stream(10, .25)).count == 3, "particles emitted so far");
        // Stopped at 1 s: no newer particles, the last one ages.
        s.emission.stopped = 1.0;
        t = paint::particle_timing(s);
        require(t.newest == 10 && near(t.age, 1.35f, 1e-5f) && t.count == 11 && !state(s, 0).alive,
                "a stopped stream emits no more and its particles age");
        s.emission.elapsed = 1.5;
        require(state(s, 0).alive && !state(s, 5).alive && state(s, 4).alive, "a stopped stream's last particles");
        // Far into a long stream the newest index wraps and the age stays exact.
        auto long_running = stream(100, 1e6 + .0123);
        t = paint::particle_timing(long_running);
        require(t.newest == 100000001u % weft::constants::particle_index_period && near(t.age, .0023f, 1e-6f),
                "a day-long stream's timing stays exact");
        require(paint::particle_timing(stream(0, 5)).count == 0, "a stream of rate 0 draws nothing");
        require(paint::particle_timing(burst(9, .5)).count == 9 && paint::particle_timing(burst(9, .5)).age == .5f,
                "a burst's timing");
        // A burst lives until its particles' lives end.
        require(state(burst(4, 1.9), 3).alive && !state(burst(4, 2.1), 3).alive, "a burst's particles die");
    }

    // A stream's particle keeps its identity: at the next emission it is the next instance, a step further on.
    void identity() {
        auto p = stream(20, 3.01);
        p.emitter.life = {3, 3};
        p.emitter.speed = {10, 40};
        p.emitter.spread = 360;
        p.emitter.size = {4, 4, 4, 4};
        p.emitter.scale = {.5f, 2};
        const auto before = state(p, 5);
        p.emission.elapsed += .05;
        const auto after = state(p, 6);
        require(before.alive && after.alive && after.size == before.size && after.color == before.color,
                "the same particle draws the same values");
        const float moved = std::hypot(after.position[0] - before.position[0], after.position[1] - before.position[1]);
        require(moved > .4f && moved < 2.1f, "and moves on along its path");
    }

    // Drag and gravity: the closed form against an integration of dv = (g - k v) dt.
    void motion() {
        auto p = burst(1, 0);
        p.emitter.life = {5, 5};
        p.emitter.speed = {80, 80};
        p.emitter.direction = -60;
        p.emitter.gravity = {10, 98};
        for (const float drag : {0.f, .8f}) {
            p.emitter.drag = drag;
            std::array<double, 2> x{0, 0};
            std::array<double, 2> v{80 * std::cos(-60 * 3.14159265358979 / 180),
                                    80 * std::sin(-60 * 3.14159265358979 / 180)};
            const double dt = 1e-5;
            double t = 0;
            for (const double at : {.5, 1.5, 3.0}) {
                while (t < at - dt / 2) {
                    for (unsigned c = 0; c < 2; ++c) {
                        const double a = p.emitter.gravity[c] - drag * v[c];
                        x[c] += v[c] * dt + .5 * a * dt * dt;
                        v[c] += a * dt;
                    }
                    t += dt;
                }
                p.emission.elapsed = at;
                const auto s = state(p, 0);
                std::cout << "motion drag " << drag << " at " << at << ": (" << s.position[0] << ", " << s.position[1]
                          << ") integrated (" << x[0] << ", " << x[1] << ")\n";
                require(s.alive && near(s.position[0], float(x[0]), .02f) && near(s.position[1], float(x[1]), .02f),
                        "the closed form follows the motion");
            }
        }
    }

    void shapes() {
        auto p = burst(400, 1e-3);
        p.emitter.area = {10, 20, 30, 40};
        for (const auto shape : {paint::ParticleEmitter::Shape::Point, paint::ParticleEmitter::Shape::Rect,
                                 paint::ParticleEmitter::Shape::Circle, paint::ParticleEmitter::Shape::Edge}) {
            p.emitter.shape = shape;
            float left = 1e9f;
            float right = -1e9f;
            for (std::uint32_t i = 0; i < 400; ++i) {
                const auto s = state(p, i);
                const float x = s.position[0];
                const float y = s.position[1];
                left = std::min(left, x);
                right = std::max(right, x);
                if (shape == paint::ParticleEmitter::Shape::Point) {
                    require(x == 10 && y == 20, "a point's particles start at it");
                } else if (shape == paint::ParticleEmitter::Shape::Rect) {
                    require(x >= 10 && x <= 40 && y >= 20 && y <= 60, "a rectangle's particles start in it");
                } else if (shape == paint::ParticleEmitter::Shape::Circle) {
                    require(std::hypot(x - 10, y - 20) <= 30.001f, "a disc's particles start in it");
                } else {
                    const bool on = (near(y, 20, 1e-3f) || near(y, 60, 1e-3f))   ? x >= 9.999f && x <= 40.001f
                                    : (near(x, 10, 1e-3f) || near(x, 40, 1e-3f)) ? y >= 19.999f && y <= 60.001f
                                                                                 : false;
                    require(on, "an outline's particles start on it");
                }
            }
            if (shape != paint::ParticleEmitter::Shape::Point) {
                require(right - left > 20, "particles spread over the shape");
            }
        }
    }

    void looks() {
        auto p = burst(1, 0);
        p.emitter.life = {3, 3};
        p.emitter.size = {3, 6, 9, 0};
        p.emitter.color = {paint::Color{1, 0, 0, 1}, paint::Color{0, 1, 0, 1}, paint::Color{0, 0, 1, .5f},
                           paint::Color{0, 0, 0, 0}};
        p.emitter.angle = {30, 30};
        p.emitter.spin = {90, 90};
        for (const auto& [age, size, color] :
             std::array<std::tuple<double, float, paint::Color>, 4>{{{0, 3, {1, 0, 0, 1}},
                                                                     {1, 6, {0, 1, 0, 1}},
                                                                     {1.5, 7.5f, {0, .5f, .5f, .75f}},
                                                                     {2, 9, {0, 0, 1, .5f}}}}) {
            p.emission.elapsed = age;
            const auto s = state(p, 0);
            require(near(s.size, size, 1e-4f) && near(s.angle, 30 + 90 * float(age), 1e-3f),
                    "size and angle over the life");
            for (unsigned c = 0; c < 4; ++c) {
                require(near(s.color[c], color[c], 1e-5f), "color over the life");
            }
        }
        // Frames at 4 per second, looping over 2 x 2; at rate 0 each particle shows one at random.
        p.emitter.sprite = 0;
        p.emitter.frames = {2, 2};
        p.emitter.frame_rate = 4;
        for (const auto& [age, frame] :
             std::array<std::pair<double, std::uint32_t>, 4>{{{.1, 0}, {.3, 1}, {.8, 3}, {1.1, 0}}}) {
            p.emission.elapsed = age;
            require(state(p, 0).frame == frame, "frames over the life");
        }
        p.emitter.frame_rate = 0;
        auto many = p;
        many.emission.count = 200;
        std::set<std::uint32_t> frames;
        for (std::uint32_t i = 0; i < 200; ++i) {
            frames.insert(state(many, i).frame);
        }
        require(frames == std::set<std::uint32_t>{0, 1, 2, 3}, "random frames cover the grid");
    }

    void flight() {
        auto p = burst(1, 0);
        p.emitter.life = {2, 2};
        p.emitter.speed = {50, 50};
        p.emitter.direction = -90;
        p.emitter.gravity = {0, 500};
        p.emission.target = std::array<float, 2>{100, 40};
        const auto start = state(p, 0).position;
        p.emission.elapsed = 1.99999;
        const auto end = state(p, 0).position;
        p.emission.elapsed = .3;
        const auto early = state(p, 0).position;
        require(start == std::array<float, 2>{0, 0} && near(end[0], 100, .01f) && near(end[1], 40, .01f) &&
                    early[1] < 0,
                "a flight leaves along its velocity, without gravity, and arrives at the target");
    }
} // namespace

int main() {
    try {
        random_values();
        timing();
        identity();
        motion();
        shapes();
        looks();
        flight();
        std::cout << "PASS particle model: random values, stream and burst timing, identity, motion, shapes, curves, "
                     "frames, flights\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
