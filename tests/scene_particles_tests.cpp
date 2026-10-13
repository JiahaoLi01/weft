#include "base/diagnostic.hpp"
#include "paint/particles.hpp"
#include "render/srgb.hpp"
#include "scene/scene.hpp"
#include "scene/scene_geometry.hpp"
#include "script/script_dom.hpp"
#include "script/script_platform.hpp"
#include "support/check.hpp"
#include "support/native_scene.hpp"
#include "support/scene_fonts.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <string>

// UI particles as scene elements show them (SceneParticles, SceneSolver::set_particles and emit_particles,
// weft.emitParticles): registration and its refusals, the --particles-* overrides, -weft-particles streams that start,
// keep, restart, stop and die, hidden elements, bursts and their flights (placed in the frame's layout, through the
// emitting element's transform), the per-frame bound, frames that keep coming and static subtrees, the reach that
// damage takes, and the script API.
namespace {
    using weft::testing::require;
    namespace paint = weft::paint;
    using Kind = paint::ParticleEmission::Kind;

    bool near(double a, double b, double tolerance = 1e-4) {
        return std::abs(a - b) <= tolerance;
    }

    // The diagnostic code and message of what throws, or none.
    template <class F>
    std::pair<weft::Diag, std::string> diagnostic(F&& f) {
        try {
            f();
        } catch (const weft::DiagnosticError& e) {
            return {e.code(), e.what()};
        }
        return {weft::Diag{}, {}};
    }

    void refused(const std::pair<weft::Diag, std::string>& d, const std::string& expected, const std::string& what) {
        if (d.first != weft::Diag::SceneProfile || d.second.find(expected) == std::string::npos) {
            std::cerr << what << ": " << d.second << '\n';
            require(false, what);
        }
    }

    void refused(const weft::Diagnostic& d, const std::string& expected, const std::string& what) {
        refused(std::pair<weft::Diag, std::string>{d.code, d.message}, expected, what);
    }

    // Embers rising from a disc at the box's centre, half as wide as the box's shorter side.
    paint::ParticleEmitter embers() {
        paint::ParticleEmitter e;
        e.shape = paint::ParticleEmitter::Shape::Circle;
        e.area = {.5f, .5f, .5f, 0};
        e.rate = 20;
        e.life = {1, 1.5f};
        e.speed = {20, 40};
        e.direction = -90;
        e.spread = 60;
        e.gravity = {0, -10};
        e.size = {4, 6, 6, 2};
        e.color = {paint::Color{1, .6f, .2f, 1}, paint::Color{1, .4f, .1f, .8f}, paint::Color{.6f, .1f, .1f, .5f},
                   paint::Color{.2f, 0, 0, 0}};
        e.softness = .5f;
        return e;
    }

    // Coins from the box's centre, which a burst sends to a target.
    paint::ParticleEmitter coins() {
        paint::ParticleEmitter e;
        e.area = {.5f, .5f, 0, 0};
        e.life = {.8f, .8f};
        e.speed = {30, 60};
        e.direction = -90;
        e.spread = 90;
        e.size = {6, 6, 6, 6};
        return e;
    }

    const weft::SceneNode* find(const weft::SceneNode& n, weft::NodeIdentity id) {
        if (n.identity == id) {
            return &n;
        }
        for (const auto& c : n.children) {
            if (const auto* found = find(c, id)) {
                return found;
            }
        }
        return nullptr;
    }

    const std::vector<weft::SceneEmission>& shown(const weft::SceneFrame& f, const weft::dom::Node& n) {
        const auto* node = find(f.root, n.identity);
        require(node != nullptr, "the element is in the frame");
        return node->particles;
    }

    std::uint64_t particles(const weft::SceneNode& n) {
        std::uint64_t count{};
        for (const auto& e : n.particles) {
            count += paint::particle_timing({e.emitter->emitter, e.emission}).count;
        }
        for (const auto& c : n.children) {
            count += particles(c);
        }
        return count;
    }

    void registry_and_overrides(weft::SceneParticles& registry) {
        require(bool(registry.add("embers", embers())), "register particles");
        require(bool(registry.add("coins", coins())), "register more particles");
        refused(registry.add("embers", embers()).error(), "registered twice", "a name registered twice");
        const auto distance =
            std::make_shared<const paint::Asset>(paint::Asset{2, 2, paint::Encoding::SignedDistance, {1, 2, 3, 4}, 4});
        refused(registry.add("glyph", embers(), distance).error(), "a distance field", "a distance field as a sprite");
        auto backwards = embers();
        backwards.rate = -1;
        require(!registry.add("backwards", backwards), "a negative rate");
        // A stream draws its longest life times its rate, and one more: past the bound of an emission.
        auto flood = embers();
        flood.rate = 60000;
        flood.life = {2, 2};
        require(!registry.add("flood", flood), "a stream past the bound of an emission");
        const auto sprite = std::make_shared<const paint::Asset>(
            paint::Asset{4, 4, paint::Encoding::Coverage, std::vector<std::uint8_t>(16, 255)});
        require(bool(registry.add("sparks", coins(), sprite)), "particles with a coverage sprite");

        // Without overrides, the registered emitter itself; a guaranteed-invalid value overrides nothing.
        const auto plain = registry.resolve("embers", {});
        require(plain && plain->emitter == embers() && registry.resolve("embers", {}) == plain &&
                    registry.resolve("embers", {{"--particles-rate", std::nullopt}, {"--other", "1"}}) == plain,
                "the registered emitter, shared");
        refused(diagnostic([&] {
                    registry.resolve("nope", {});
                }),
                "unknown particles nope", "an unknown name");
        // Every override; a range of one value is both ends; a color keeps each key's alpha, times its own.
        const auto set = registry.resolve("embers", {{"--particles-rate", "12"},
                                                     {"--particles-life", "0.5 1.25"},
                                                     {"--particles-speed", "30"},
                                                     {"--particles-direction", "45"},
                                                     {"--particles-spread", " 10 "},
                                                     {"--particles-gravity", "0 -30"},
                                                     {"--particles-scale", "2"},
                                                     {"--particles-color", "rgba(255, 0, 0, 0.5)"},
                                                     {"--particles-drag", "5"}});
        const auto& e = set->emitter;
        require(set != plain && e.rate == 12 && e.life == std::array<float, 2>{.5f, 1.25f} &&
                    e.speed == std::array<float, 2>{30, 30} && e.direction == 45 && e.spread == 10 &&
                    e.gravity == std::array<float, 2>{0, -30} && e.scale == std::array<float, 2>{2, 2} && e.drag == 0 &&
                    e.area == embers().area && e.size == embers().size,
                "the overrides");
        for (std::size_t k = 0; k < 4; ++k) {
            require(e.color[k][0] == 1 && e.color[k][1] == 0 && e.color[k][2] == 0 &&
                        near(e.color[k][3], embers().color[k][3] * .5f, 1e-6),
                    "a color override");
        }
        for (const auto& [property, value, expected] : std::vector<std::array<std::string, 3>>{
                 {"--particles-rate", "fast", "--particles-rate is not a number"},
                 {"--particles-life", "1 2 3", "--particles-life is not a number or two"},
                 {"--particles-gravity", "10", "--particles-gravity is not two numbers"},
                 {"--particles-color", "1", "--particles-color is not a color"},
                 {"--particles-speed", "1px", "--particles-speed is not a number or two"},
                 // Values the paint checks refuse.
                 {"--particles-rate", "-1", "particles embers:"},
                 {"--particles-life", "2 1", "particles embers:"}}) {
            refused(diagnostic([&] {
                        registry.resolve("embers", {{property, value}});
                    }),
                    expected, property + ": " + value);
        }
    }

    // Every particle the model draws lies inside the reach, at every time of its life: random emitters of each shape,
    // with and without drag, flying to targets or not.
    void reach() {
        std::mt19937 random(0x5eed);
        std::uniform_real_distribution<float> unit(0, 1);
        std::size_t checked{};
        for (unsigned trial = 0; trial < 160; ++trial) {
            paint::ParticleEmitter m;
            m.shape = paint::ParticleEmitter::Shape(trial % 4);
            m.area = {unit(random), unit(random), unit(random), unit(random)};
            m.life = {.2f + unit(random), 1.5f + unit(random)};
            m.speed = {unit(random) * 40, 40 + unit(random) * 80};
            m.direction = unit(random) * 720 - 360;
            m.spread = trial % 3 == 0 ? 400 : unit(random) * 120;
            m.gravity = {unit(random) * 200 - 100, unit(random) * 200 - 100};
            m.drag = trial % 2 ? unit(random) * 3 : 0;
            m.size = {unit(random) * 10, unit(random) * 10, unit(random) * 10, unit(random) * 10};
            m.scale = {.5f, .5f + unit(random) * 2};
            m.angle = {0, 360};
            weft::SceneEmission e{std::make_shared<const weft::SceneEmitter>(weft::SceneEmitter{m, {}}), {}};
            e.emission.kind = Kind::Burst;
            e.emission.count = 64;
            e.emission.duration = .5f;
            e.emission.seed = trial;
            if (trial % 5 == 0) {
                e.emission.target = std::array<float, 2>{unit(random) * 400 - 200, unit(random) * 400 - 200};
            }
            const float width = 10 + unit(random) * 90;
            const float height = 10 + unit(random) * 90;
            const auto r = weft::scene_particle_reach(e, width, height);
            for (double t = 0; t <= .5 + m.life[1]; t += .05) {
                paint::Particles p{m, e.emission};
                p.emitter.area = weft::scene_emitter_area(m, width, height);
                p.emission.elapsed = t;
                const auto timing = paint::particle_timing(p);
                for (std::uint32_t i = 0; i < timing.count; ++i) {
                    const auto s = paint::particle_state(p, timing, i);
                    if (!s.alive) {
                        continue;
                    }
                    const double half = s.size * std::sqrt(2.) / 2;
                    constexpr double slack = .01;
                    if (!(s.position[0] - half >= r[0] - slack && s.position[0] + half <= r[2] + slack &&
                          s.position[1] - half >= r[1] - slack && s.position[1] + half <= r[3] + slack)) {
                        std::cerr << "trial " << trial << " t " << t << " particle " << i << " at " << s.position[0]
                                  << ", " << s.position[1] << " size " << s.size << " outside " << r[0] << ' ' << r[1]
                                  << ' ' << r[2] << ' ' << r[3] << '\n';
                        require(false, "a particle outside its reach");
                    }
                    ++checked;
                }
            }
        }
        require(checked > 100000, "particles checked against their reach");
        // Tight where it can be: from a point, straight right under drag, as far as min(life, 1 / drag) times the
        // speed, and no higher or lower than half the particle's diagonal; headings within a quadrant, inside it.
        paint::ParticleEmitter m;
        m.life = {1, 1};
        m.speed = {10, 10};
        m.drag = 2;
        m.size = {2, 2, 2, 2};
        weft::SceneEmission e{std::make_shared<const weft::SceneEmitter>(weft::SceneEmitter{m, {}}), {}};
        const double half = std::sqrt(2.);
        auto r = weft::scene_particle_reach(e, 100, 100);
        require(near(r[0], -half) && near(r[1], -half) && near(r[2], 5 + half) && near(r[3], half),
                "a tight reach under drag");
        m.drag = 0;
        m.direction = 45;
        m.spread = 90;
        e.emitter = std::make_shared<const weft::SceneEmitter>(weft::SceneEmitter{m, {}});
        r = weft::scene_particle_reach(e, 100, 100);
        require(near(r[0], -half) && near(r[1], -half) && near(r[2], 10 + half) && near(r[3], 10 + half),
                "a tight reach within a quadrant");
        std::cout << "reach: " << checked << " particle positions inside\n";
    }
} // namespace

int main() {
    try {
        weft::SceneParticles registry;
        registry_and_overrides(registry);
        reach();
        auto& text = weft::testing::thread_scene_text();

        // A scene: a wrapper (to hide) holding a panel that streams embers, and a target.
        weft::testing::NativeScene s;
        s.set(*s.root, "style", "position:relative;width:400px;height:300px");
        auto& wrap = s.html("div");
        s.set(wrap, "style", "position:absolute;left:0;top:0;width:400px;height:300px");
        s.tree.insert(*s.root, wrap, nullptr);
        auto& panel = s.html("div");
        const std::string at = "position:absolute;left:100px;top:100px;width:40px;height:20px;";
        s.set(panel, "style", at + "-weft-particles:embers");
        s.tree.insert(wrap, panel, nullptr);
        auto& target = s.html("div");
        s.set(target, "style", "position:absolute;left:300px;top:200px;width:20px;height:20px");
        s.tree.insert(wrap, target, nullptr);

        refused(diagnostic([&] {
                    weft::SceneSolver bare("", "", text);
                    bare.frame(s.tree.snapshot(*s.root), 400, 300, 0);
                }),
                "particles embers without registered particles", "-weft-particles without registered particles");

        // A stream starts at the frame its element names it and keeps its emission (and seed) while nothing changes;
        // only paint changes, and frames keep coming.
        weft::SceneSolver solver("", "", text);
        solver.set_particles(&registry);
        weft::SceneFrame f;
        solver.update(s.tree.snapshot(*s.root), f, 400, 300, 1000);
        {
            const auto& p = shown(f, panel);
            require(p.size() == 1 && p[0].emitter == registry.resolve("embers", {}) &&
                        p[0].emission.kind == Kind::Stream && p[0].emission.elapsed == 0 && !p[0].emission.stopped &&
                        !p[0].emission.target && f.animated,
                    "a stream starts");
        }
        const auto first_seed = shown(f, panel)[0].emission.seed;
        solver.update(s.tree.snapshot(*s.root), f, 400, 300, 1500);
        {
            const auto& p = shown(f, panel);
            require(p.size() == 1 && near(p[0].emission.elapsed, .5) && p[0].emission.seed == first_seed &&
                        solver.work().paint_changed && !solver.work().geometry_changed && f.animated,
                    "a stream runs");
        }
        // Other override values restart it: the old stream stops and lives out its particles beside the new one.
        s.set(panel, "style", at + "-weft-particles:embers;--particles-rate:40");
        solver.update(s.tree.snapshot(*s.root), f, 400, 300, 2000);
        {
            const auto& p = shown(f, panel);
            require(p.size() == 2 && p[0].emission.stopped && near(*p[0].emission.stopped, 1) &&
                        near(p[0].emission.elapsed, 1) && p[1].emitter->emitter.rate == 40 &&
                        p[1].emission.elapsed == 0 && !p[1].emission.stopped && p[1].emission.seed != first_seed,
                    "changed overrides restart the stream");
        }
        // none stops it; stopped streams last their longest life (1.5 s) after their stop.
        s.set(panel, "style", at + "-weft-particles:none;--particles-rate:40");
        solver.update(s.tree.snapshot(*s.root), f, 400, 300, 2500);
        require(shown(f, panel).size() == 2 && shown(f, panel)[1].emission.stopped &&
                    near(*shown(f, panel)[1].emission.stopped, .5) && f.animated,
                "none stops a stream");
        solver.update(s.tree.snapshot(*s.root), f, 400, 300, 3499);
        require(shown(f, panel).size() == 2, "a stopped stream lives out its particles");
        solver.update(s.tree.snapshot(*s.root), f, 400, 300, 3500);
        require(shown(f, panel).size() == 1 && near(*shown(f, panel)[0].emission.stopped, .5),
                "a stream whose particles died is gone");
        solver.update(s.tree.snapshot(*s.root), f, 400, 300, 4000);
        require(shown(f, panel).empty() && !f.animated, "no particles, no more frames");
        // Without particles the subtree is static again: after a new layout, the next frame takes it whole.
        solver.update(s.tree.snapshot(*s.root), f, 401, 300, 4010);
        const auto reuses = solver.work().static_subtree_reuses;
        solver.update(s.tree.snapshot(*s.root), f, 401, 300, 4020);
        require(solver.work().static_subtree_reuses > reuses && !solver.work().paint_changed,
                "a subtree without particles is static");
        // A rate of 0 streams nothing.
        s.set(panel, "style", at + "-weft-particles:embers;--particles-rate:0");
        solver.update(s.tree.snapshot(*s.root), f, 400, 300, 4050);
        require(shown(f, panel).empty() && !f.animated, "a stream of no particles");
        // A hidden element (here its parent is display: none) drops its particles at once; shown again, it starts a
        // new stream.
        s.set(panel, "style", at + "-weft-particles:embers");
        solver.update(s.tree.snapshot(*s.root), f, 400, 300, 4100);
        require(shown(f, panel).size() == 1, "a stream again");
        s.set(wrap, "style", "position:absolute;left:0;top:0;width:400px;height:300px;display:none");
        solver.update(s.tree.snapshot(*s.root), f, 400, 300, 4200);
        require(shown(f, panel).empty() && !f.animated, "a hidden element drops its particles");
        s.set(wrap, "style", "position:absolute;left:0;top:0;width:400px;height:300px");
        solver.update(s.tree.snapshot(*s.root), f, 400, 300, 4300);
        require(shown(f, panel).size() == 1 && shown(f, panel)[0].emission.elapsed == 0, "shown, a new stream");
        // An element the frame no longer has takes its particles along; added again, it starts anew.
        s.tree.detach(panel);
        solver.update(s.tree.snapshot(*s.root), f, 400, 300, 4400);
        require(!find(f.root, panel.identity) && !f.animated, "a removed element's particles are gone");
        s.tree.insert(wrap, panel, &target);
        solver.update(s.tree.snapshot(*s.root), f, 400, 300, 4500);
        require(shown(f, panel).size() == 1 && shown(f, panel)[0].emission.elapsed == 0, "added again, a new stream");

        // Damage: what the particles reach, through the element's map; under a transform with the maps of
        // SceneGeometry; anywhere inside a perspective plane.
        weft::SceneGeometry geometry(text);
        {
            auto before = f;
            solver.update(s.tree.snapshot(*s.root), f, 400, 300, 4516);
            const auto damage = weft::scene_damage(before, f, 1);
            const auto r = weft::scene_particle_reach(shown(f, panel)[0], 40, 20);
            require(damage.changed && !damage.full && damage.rect[0] <= std::max(0.f, 100 + r[0]) &&
                        damage.rect[1] <= std::max(0.f, 100 + r[1]) && damage.rect[2] >= 100 + r[2] &&
                        damage.rect[3] >= 100 + r[3] && damage.rect[0] >= 100 + r[0] - 3 &&
                        damage.rect[2] <= 100 + r[2] + 3 && damage.rect[3] <= 100 + r[3] + 3,
                    "damage is what the particles reach");
            s.set(panel, "style", at + "-weft-particles:embers;transform:rotate(90deg)");
            solver.update(s.tree.snapshot(*s.root), f, 400, 300, 4532);
            f.elements = geometry.geometry(f);
            before = f;
            solver.update(s.tree.snapshot(*s.root), f, 400, 300, 4548);
            f.elements = geometry.geometry(f);
            const auto turned = weft::scene_damage(before, f, 1);
            // rotate(90deg) about the box's centre (120, 110): (x, y) -> (120 - (y - 10), 110 + (x - 20)).
            double left = 1e9;
            double top = 1e9;
            double right = -1e9;
            double bottom = -1e9;
            for (const auto& e : shown(f, panel)) {
                const auto q = weft::scene_particle_reach(e, 40, 20);
                for (const auto& [x, y] :
                     std::array<std::array<float, 2>, 4>{{{q[0], q[1]}, {q[2], q[1]}, {q[0], q[3]}, {q[2], q[3]}}}) {
                    left = std::min(left, 120. - (y - 10));
                    right = std::max(right, 120. - (y - 10));
                    top = std::min(top, 110. + (x - 20));
                    bottom = std::max(bottom, 110. + (x - 20));
                }
            }
            require(turned.changed && !turned.full && turned.rect[0] <= left && turned.rect[1] <= top &&
                        turned.rect[2] >= right && turned.rect[3] >= bottom && turned.rect[0] >= left - 3 &&
                        turned.rect[2] <= right + 3,
                    "damage through the element's transform");
            s.set(wrap, "style",
                  "position:absolute;left:0;top:0;width:400px;height:300px;transform:perspective(400px) "
                  "rotateY(20deg)");
            solver.update(s.tree.snapshot(*s.root), f, 400, 300, 4564);
            f.elements = geometry.geometry(f);
            before = f;
            solver.update(s.tree.snapshot(*s.root), f, 400, 300, 4580);
            f.elements = geometry.geometry(f);
            require(weft::scene_damage(before, f, 1).full, "particles inside a perspective plane damage everything");
            s.set(wrap, "style", "position:absolute;left:0;top:0;width:400px;height:300px");
            s.set(panel, "style", at);
            solver.update(s.tree.snapshot(*s.root), f, 400, 300, 4600);
            solver.update(s.tree.snapshot(*s.root), f, 400, 300, 6200);
            require(shown(f, panel).empty() && !f.animated, "the stream has ended");
        }

        // -weft-particles names one registered set of particles, on an HTML element or an svg root.
        for (const auto& [value, expected] : std::vector<std::pair<std::string, std::string>>{
                 {"embers coins", "-weft-particles takes none or one particle name"},
                 {"nope", "unknown particles nope"},
                 {"embers;--particles-rate:fast", "--particles-rate is not a number"}}) {
            s.set(panel, "style", at + "-weft-particles:" + value);
            weft::SceneSolver refusing("", "", text);
            refusing.set_particles(&registry);
            refused(diagnostic([&] {
                        refusing.frame(s.tree.snapshot(*s.root), 400, 300, 0);
                    }),
                    expected, "refused " + value);
        }
        s.set(panel, "style", at);
        {
            auto& svg = s.svg("svg");
            s.set(svg, "style", "position:absolute;left:0;top:0;width:50px;height:50px;-weft-particles:embers");
            s.tree.insert(wrap, svg, nullptr);
            weft::SceneSolver svg_solver("", "", text);
            svg_solver.set_particles(&registry);
            require(shown(svg_solver.frame(s.tree.snapshot(*s.root), 400, 300, 0), svg).size() == 1,
                    "an svg root streams");
            auto& circle = s.svg("circle");
            s.set(circle, "style", "-weft-particles:embers");
            s.tree.insert(svg, circle, nullptr);
            weft::SceneSolver inside("", "", text);
            inside.set_particles(&registry);
            refused(diagnostic([&] {
                        inside.frame(s.tree.snapshot(*s.root), 400, 300, 0);
                    }),
                    "particles on an SVG element inside svg", "particles inside svg");
            s.tree.detach(svg);
        }

        // Bursts: refusals; one starts at the next frame with the element's overrides and lasts its duration and the
        // longest life.
        {
            weft::SceneSolver bare("", "", text);
            refused(diagnostic([&] {
                        bare.emit_particles(panel.identity, "coins", {1});
                    }),
                    "weft.emitParticles: coins without registered particles", "a burst without registered particles");
        }
        refused(diagnostic([&] {
                    solver.emit_particles(panel.identity, "nope", {1});
                }),
                "unknown particles nope", "a burst of unknown particles");
        constexpr float nan = std::numeric_limits<float>::quiet_NaN();
        for (const auto& burst : std::vector<weft::SceneBurst>{{0},
                                                               {65537},
                                                               {1, -1},
                                                               {1, nan},
                                                               {1, std::numeric_limits<float>::infinity()},
                                                               {1, 0, std::nullopt, std::array<float, 2>{nan, 0}}}) {
            refused(diagnostic([&] {
                        solver.emit_particles(panel.identity, "coins", burst);
                    }),
                    "weft.emitParticles: a count of 1 to 65536", "a refused burst");
        }
        s.set(panel, "style", at + "--particles-color:#00ff00");
        solver.emit_particles(panel.identity, "coins", {12, .25f});
        // On an element the frame does not show, and on a hidden one, a burst is dropped.
        auto& gone = s.html("div");
        solver.emit_particles(gone.identity, "coins", {3});
        auto& hidden = s.html("div");
        s.set(hidden, "style", "display:none");
        s.tree.insert(wrap, hidden, nullptr);
        solver.emit_particles(hidden.identity, "coins", {3});
        solver.update(s.tree.snapshot(*s.root), f, 400, 300, 7000);
        {
            const auto& p = shown(f, panel);
            require(p.size() == 1 && p[0].emission.kind == Kind::Burst && p[0].emission.count == 12 &&
                        p[0].emission.duration == .25f && p[0].emission.elapsed == 0 && !p[0].emission.target &&
                        p[0].emitter->emitter.color[0] == paint::Color{0, 1, 0, 1} && f.animated &&
                        shown(f, hidden).empty(),
                    "a burst starts at the next frame with the element's overrides");
        }
        // A new layout while the burst lives makes it part of the solver's reference frame.
        solver.update(s.tree.snapshot(*s.root), f, 401, 300, 7500);
        solver.update(s.tree.snapshot(*s.root), f, 401, 300, 8049);
        require(shown(f, panel).size() == 1 && near(shown(f, panel)[0].emission.elapsed, 1.049), "a burst lasts");
        // The life of .8f is a little over .8 s.
        solver.update(s.tree.snapshot(*s.root), f, 401, 300, 8051);
        require(shown(f, panel).empty() && !f.animated, "a burst lasts its duration and its longest life");
        // Later frames do not take the reference frame's particles back.
        solver.update(s.tree.snapshot(*s.root), f, 401, 300, 8100);
        require(shown(f, panel).empty() && !f.animated, "ended particles stay ended");

        // Flights: a target's centre (310, 210) in the panel's px; a point; through the panel's transform; a hidden
        // target flies nowhere; later frames keep where the flight was placed.
        solver.emit_particles(panel.identity, "coins", {6, 0, target.identity});
        solver.emit_particles(panel.identity, "coins", {6, 0, std::nullopt, std::array<float, 2>{50, 60}});
        solver.update(s.tree.snapshot(*s.root), f, 400, 300, 9000);
        {
            const auto& p = shown(f, panel);
            require(p.size() == 2 && p[0].emission.target && near((*p[0].emission.target)[0], 210) &&
                        near((*p[0].emission.target)[1], 110) && p[1].emission.target &&
                        near((*p[1].emission.target)[0], -50) && near((*p[1].emission.target)[1], -40),
                    "flights to a target's centre and to a point");
        }
        s.set(target, "style", "position:absolute;left:10px;top:10px;width:20px;height:20px");
        solver.update(s.tree.snapshot(*s.root), f, 400, 300, 9100);
        require(near((*shown(f, panel)[0].emission.target)[0], 210), "a flight keeps where it was placed");
        s.set(panel, "style", at + "transform:rotate(90deg)");
        s.set(target, "style", "position:absolute;left:300px;top:200px;width:20px;height:20px");
        solver.emit_particles(panel.identity, "coins", {6, 0, target.identity});
        solver.update(s.tree.snapshot(*s.root), f, 400, 300, 9200);
        {
            // (310, 210) is (190, 100) from the centre (120, 110); turned back a quarter, (100, -190) from the box's
            // centre (20, 10).
            const auto& p = shown(f, panel);
            require(p.size() == 3 && near((*p[2].emission.target)[0], 120, 1e-3) &&
                        near((*p[2].emission.target)[1], -180, 1e-3),
                    "a flight through the element's transform");
        }
        s.set(target, "style", "position:absolute;left:300px;top:200px;width:20px;height:20px;display:none");
        solver.emit_particles(panel.identity, "coins", {6, 0, target.identity});
        solver.update(s.tree.snapshot(*s.root), f, 400, 300, 9300);
        require(shown(f, panel).size() == 4 && !shown(f, panel)[3].emission.target,
                "a hidden target: the burst flies nowhere");
        s.set(target, "style", "position:absolute;left:300px;top:200px;width:20px;height:20px");
        s.set(panel, "style", at);
        solver.update(s.tree.snapshot(*s.root), f, 400, 300, 11000);
        require(shown(f, panel).empty(), "flights end");

        // Bounds: bursts waiting for a frame, and particles in a frame.
        for (unsigned k = 0; k < 1024; ++k) {
            solver.emit_particles(panel.identity, "coins", {1});
        }
        refused(diagnostic([&] {
                    solver.emit_particles(panel.identity, "coins", {1});
                }),
                "more than 1024 bursts before a frame", "the bound of waiting bursts");
        solver.update(s.tree.snapshot(*s.root), f, 400, 300, 11100);
        require(shown(f, panel).size() == 1024 && particles(f.root) == 1024, "1024 bursts in a frame");
        solver.emit_particles(panel.identity, "coins", {1});
        solver.update(s.tree.snapshot(*s.root), f, 400, 300, 11200);
        require(shown(f, panel).size() == 1025, "an update takes the waiting bursts");
        solver.update(s.tree.snapshot(*s.root), f, 400, 300, 13000);
        for (unsigned k = 0; k < 4; ++k) {
            solver.emit_particles(panel.identity, "coins", {65536});
        }
        solver.update(s.tree.snapshot(*s.root), f, 400, 300, 13100);
        require(particles(f.root) == 262144, "a frame of the bound's particles");
        solver.emit_particles(target.identity, "coins", {1});
        refused(diagnostic([&] {
                    solver.update(s.tree.snapshot(*s.root), f, 400, 300, 13200);
                }),
                "particles: 262145 in a frame, more than 262144", "the bound of particles in a frame");
        solver.reset();
        f = {};

        // A burst on an element the frames took whole starts at the next frame.
        {
            weft::SceneSolver quiet("", "", text);
            quiet.set_particles(&registry);
            weft::SceneFrame q;
            s.set(panel, "style", at);
            quiet.update(s.tree.snapshot(*s.root), q, 400, 300, 0);
            const auto before = quiet.work().static_subtree_reuses;
            quiet.update(s.tree.snapshot(*s.root), q, 400, 300, 16);
            require(quiet.work().static_subtree_reuses > before && !q.animated, "a static scene");
            quiet.emit_particles(panel.identity, "coins", {4});
            quiet.update(s.tree.snapshot(*s.root), q, 400, 300, 32);
            require(shown(q, panel).size() == 1 && q.animated, "a burst on a static element");
            // Without the registry, emissions that started live out their particles.
            quiet.set_particles(nullptr);
            quiet.update(s.tree.snapshot(*s.root), q, 400, 300, 48);
            require(shown(q, panel).size() == 1, "emissions outlive the registry");
        }

        // Fixed clock: two solvers given the same input build equal frames, seeds and all.
        {
            const auto run = [&] {
                weft::SceneSolver again("", "", text);
                again.set_particles(&registry);
                s.set(panel, "style", at + "-weft-particles:embers");
                weft::SceneFrame g;
                again.update(s.tree.snapshot(*s.root), g, 400, 300, 0);
                again.emit_particles(panel.identity, "coins", {9, .2f, target.identity});
                again.update(s.tree.snapshot(*s.root), g, 400, 300, 400);
                again.update(s.tree.snapshot(*s.root), g, 400, 300, 700);
                return g;
            };
            const auto a = run();
            const auto b = run();
            require(a.root == b.root && shown(a, panel).size() == 2 &&
                        shown(a, panel)[0].emission.seed != shown(a, panel)[1].emission.seed,
                    "the same input, the same particles");
            s.set(panel, "style", at);
        }

        // The script API: weft.emitParticles hands each burst to the owner's host, which refuses what the solver
        // refuses; bad arguments throw.
        {
            weft::ScriptDom vm;
            weft::install_script_platform(vm, weft::ScriptPlatformModule::WebHost);
            weft::install_script_platform(vm, weft::ScriptPlatformModule::AnimationFrame);
            weft::install_script_platform(vm, weft::ScriptPlatformModule::ScenePlatform);
            // The document's #root.
            vm.eval("globalThis.root=document.querySelector('#root');"
                    "root.setAttribute('style','position:relative;width:200px;height:100px');"
                    "globalThis.a=document.createElement('div');a.setAttribute('id','a');root.appendChild(a);"
                    "a.setAttribute('style','position:absolute;left:10px;top:20px;width:40px;height:20px');"
                    "globalThis.b=document.createElement('div');b.setAttribute('id','b');root.appendChild(b);"
                    "b.setAttribute('style','position:absolute;left:150px;top:60px;width:20px;height:20px')");
            require(vm.eval("try{weft.emitParticles(a,'coins');'none'}catch(e){e.message}")
                            .find("WEFT_PARTICLES: no particle host") != std::string::npos,
                    "no particle host");
            std::vector<std::pair<weft::NodeIdentity, weft::ParticleRequest>> requests;
            weft::SceneSolver scene("", "", text);
            scene.set_particles(&registry);
            vm.install_particle_host([&](weft::NodeIdentity element, const weft::ParticleRequest& request) {
                requests.emplace_back(element, request);
                std::optional<std::array<float, 2>> point;
                if (request.point) {
                    point = std::array<float, 2>{float((*request.point)[0]), float((*request.point)[1])};
                }
                scene.emit_particles(element, request.name,
                                     {request.count, float(request.duration), request.target, point});
            });
            vm.eval("weft.emitParticles(a,'coins');weft.emitParticles(a,'coins',{count:30,duration:0.5,target:b});"
                    "weft.emitParticles(a,'coins',{count:3,target:{x:10,y:20}})");
            const auto a = vm.node_identity("#a");
            const auto b = vm.node_identity("#b");
            require(requests.size() == 3 && requests[0].first == a && requests[0].second.name == "coins" &&
                        requests[0].second.count == 1 && requests[0].second.duration == 0 &&
                        !requests[0].second.target && !requests[0].second.point && requests[1].second.count == 30 &&
                        requests[1].second.duration == .5 && requests[1].second.target == b &&
                        requests[2].second.point == std::array<double, 2>{10, 20},
                    "weft.emitParticles hands bursts to the host");
            for (const auto& [call, expected] : std::vector<std::pair<std::string, std::string>>{
                     {"weft.emitParticles(null,'coins')", "weft.emitParticles: an element"},
                     {"weft.emitParticles(a,'coins',{count:0})", "WEFT_PARTICLES: a whole count"},
                     {"weft.emitParticles(a,'coins',{count:1.5})", "WEFT_PARTICLES: a whole count"},
                     {"weft.emitParticles(a,'coins',{duration:-1})", "WEFT_PARTICLES: a whole count"},
                     {"weft.emitParticles(a,'coins',{target:{x:'left',y:0}})", "weft.emitParticles: a target"},
                     {"weft.emitParticles(a,'nope')", "unknown particles nope"}}) {
                const auto message = vm.eval("try{" + call + ";'none'}catch(e){e.message}");
                if (message.find(expected) == std::string::npos) {
                    std::cerr << call << ": " << message << '\n';
                    require(false, "a refused call");
                }
            }
            require(requests.size() == 4, "only the unknown name reached the host");
            // The next frame shows them, the flight placed: b's centre (160, 70) in a's px.
            weft::SceneFrame g;
            scene.update(vm.snapshot_subtree("#root"), g, 200, 100, 0);
            const auto* node = find(g.root, a);
            require(node && node->particles.size() == 3 && node->particles[1].emission.target &&
                        near((*node->particles[1].emission.target)[0], 150) &&
                        near((*node->particles[1].emission.target)[1], 50) &&
                        near((*node->particles[2].emission.target)[0], 0) &&
                        near((*node->particles[2].emission.target)[1], 0),
                    "the script's bursts in the next frame");
        }

        std::cout
            << "PASS scene particles: registration, overrides, streams, hidden elements, bursts, flights, bounds, "
               "static subtrees, reach and damage, fixed clock, script API\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
