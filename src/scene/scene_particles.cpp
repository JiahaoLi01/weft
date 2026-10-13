#include "scene/scene_particles.hpp"

#include "base/diagnostic.hpp"
#include "constants/common.hpp"
#include "scene/scene_effects.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace weft {
    std::array<float, 4> scene_emitter_area(const paint::ParticleEmitter& emitter, float width, float height) {
        const auto& a = emitter.area;
        switch (emitter.shape) {
            case paint::ParticleEmitter::Shape::Point:
                return {a[0] * width, a[1] * height, 0, 0};
            case paint::ParticleEmitter::Shape::Circle:
                return {a[0] * width, a[1] * height, a[2] * std::min(width, height) / 2, 0};
            case paint::ParticleEmitter::Shape::Rect:
            case paint::ParticleEmitter::Shape::Edge:
                break;
        }
        return {a[0] * width, a[1] * height, a[2] * width, a[3] * height};
    }

    std::array<float, 4> scene_particle_reach(const SceneEmission& emission, float width, float height) {
        const auto& m = emission.emitter->emitter;
        const auto a = scene_emitter_area(m, width, height);
        // Where particles start.
        std::array<double, 4> start{a[0], a[1], a[0], a[1]};
        switch (m.shape) {
            case paint::ParticleEmitter::Shape::Point:
                break;
            case paint::ParticleEmitter::Shape::Circle:
                start = {a[0] - a[2], a[1] - a[2], a[0] + a[2], a[1] + a[2]};
                break;
            case paint::ParticleEmitter::Shape::Rect:
            case paint::ParticleEmitter::Shape::Edge:
                start = {std::min(a[0], a[0] + a[2]), std::min(a[1], a[1] + a[3]), std::max(a[0], a[0] + a[2]),
                         std::max(a[1], a[1] + a[3])};
                break;
        }
        // The velocity's least and greatest x and y: speeds in their range, headings within the spread.
        constexpr double pi = 3.14159265358979323846;
        const double heading = m.direction * pi / 180;
        const double half = std::abs(double(m.spread)) * pi / 360;
        // cos(t - phase) over the headings: its ends, and 1 or -1 where a peak or a trough lies between them.
        const auto wave = [&](double phase) {
            const double from = heading - half - phase;
            const double to = heading + half - phase;
            std::array<double, 2> r{std::min(std::cos(from), std::cos(to)), std::max(std::cos(from), std::cos(to))};
            if (to - from >= 2 * pi || std::ceil(from / (2 * pi)) * 2 * pi <= to) {
                r[1] = 1;
            }
            if (to - from >= 2 * pi || std::ceil((from - pi) / (2 * pi)) * 2 * pi + pi <= to) {
                r[0] = -1;
            }
            return r;
        };
        const std::array<std::array<double, 2>, 2> unit{wave(0), wave(pi / 2)};
        const double slow = std::min(m.speed[0], m.speed[1]);
        const double fast = std::max(m.speed[0], m.speed[1]);
        std::array<std::array<double, 2>, 2> velocity{};
        for (unsigned c = 0; c < 2; ++c) {
            velocity[c] = {unit[c][0] < 0 ? fast * unit[c][0] : slow * unit[c][0],
                           unit[c][1] > 0 ? fast * unit[c][1] : slow * unit[c][1]};
        }
        const double life = std::max(m.life[0], m.life[1]);
        std::array<double, 4> reach = start;
        const auto extend = [&](unsigned c, double low, double high) {
            reach[c] = std::min(reach[c], start[c] + low);
            reach[c + 2] = std::max(reach[c + 2], start[c + 2] + high);
        };
        if (emission.emission.target) {
            // A curve inside the triangle of its start, its control point (the start moved along the velocity for half
            // the life) and the target.
            for (unsigned c = 0; c < 2; ++c) {
                extend(c, std::min(0., velocity[c][0] * life / 2), std::max(0., velocity[c][1] * life / 2));
                reach[c] = std::min(reach[c], double((*emission.emission.target)[c]));
                reach[c + 2] = std::max(reach[c + 2], double((*emission.emission.target)[c]));
            }
        } else {
            // The velocity carries a particle at most decay = min(age, 1 / drag) and gravity at most
            // min(age^2 / 2, age / drag) (no further without drag).
            const bool drag = m.drag > constants::particle_drag_epsilon;
            const double carry = drag ? std::min(life, 1 / double(m.drag)) : life;
            const double fall = drag ? std::min(life * life / 2, life / double(m.drag)) : life * life / 2;
            for (unsigned c = 0; c < 2; ++c) {
                const double g = m.gravity[c] * fall;
                extend(c, std::min(0., velocity[c][0] * carry) + std::min(0., g),
                       std::max(0., velocity[c][1] * carry) + std::max(0., g));
            }
        }
        // Half the diagonal of the largest particle, which may turn.
        const double size = *std::max_element(m.size.begin(), m.size.end()) *
                            std::max(std::abs(m.scale[0]), std::abs(m.scale[1])) * std::sqrt(2.) / 2;
        return {float(reach[0] - size), float(reach[1] - size), float(reach[2] + size), float(reach[3] + size)};
    }

    namespace {
        using Type = paint::EffectParameter::Type;

        // emitter and its sprite as the paint checks take them, as a stream draws them.
        Status checked(const paint::ParticleEmitter& emitter, const std::shared_ptr<const paint::Asset>& sprite) {
            paint::DisplayList list;
            paint::Particles p{emitter, {}};
            p.emitter.sprite.reset();
            if (sprite) {
                list.assets.push_back(sprite);
                p.emitter.sprite = 0;
            }
            list.particles.push_back(std::move(p));
            paint::Item item;
            item.kind = paint::Item::Kind::Particles;
            item.width = item.height = 1;
            item.clip = {0, 0, 1, 1};
            list.items.push_back(item);
            return paint::check(list, 0);
        }
    } // namespace

    Status SceneParticles::add(std::string name, paint::ParticleEmitter emitter,
                               std::shared_ptr<const paint::Asset> sprite) {
        const auto refuse = [&](const std::string& why) {
            return make_diagnostic(Diag::SceneProfile, "particles " + name + ": " + why);
        };
        if (emitters_.contains(name)) {
            return refuse("registered twice");
        }
        if (sprite && sprite->encoding == paint::Encoding::SignedDistance) {
            return refuse("a distance field as a sprite");
        }
        if (auto valid = checked(emitter, sprite); !valid) {
            return refuse(valid.error().message);
        }
        emitters_.emplace(std::move(name),
                          std::make_shared<const SceneEmitter>(SceneEmitter{std::move(emitter), std::move(sprite)}));
        return {};
    }

    std::shared_ptr<const SceneEmitter>
    SceneParticles::resolve(std::string_view name,
                            const std::map<std::string, std::optional<std::string>>& custom) const {
        const auto found = emitters_.find(name);
        if (found == emitters_.end()) {
            throw_diagnostic(Diag::SceneProfile, "unknown particles " + std::string(name));
        }
        const auto& registered = *found->second;
        auto e = registered.emitter;
        bool changed{};
        // The override --particles-<property>: absent or guaranteed invalid, none.
        const auto read = [&](const char* property) -> std::optional<std::string> {
            const auto value = custom.find(std::string("--particles-") + property);
            return value == custom.end() ? std::nullopt : value->second;
        };
        const auto refuse = [&](const char* property, const char* what) {
            throw_diagnostic(Diag::SceneProfile,
                             "particles " + std::string(name) + ": --particles-" + property + " is not " + what);
        };
        const auto number = [&](const char* property, float& into) {
            if (const auto text = read(property)) {
                const auto value = scene_effect_value(Type::Number, *text);
                if (!value) {
                    refuse(property, "a number");
                }
                into = (*value)[0];
                changed = true;
            }
        };
        // Two numbers, or one for both ends.
        const auto range = [&](const char* property, std::array<float, 2>& into) {
            if (const auto text = read(property)) {
                auto value = scene_effect_value(Type::Vector2, *text);
                if (!value) {
                    value = scene_effect_value(Type::Number, *text);
                    if (value) {
                        (*value)[1] = (*value)[0];
                    }
                }
                if (!value) {
                    refuse(property, "a number or two");
                }
                into = {(*value)[0], (*value)[1]};
                changed = true;
            }
        };
        number("rate", e.rate);
        range("life", e.life);
        range("speed", e.speed);
        number("direction", e.direction);
        number("spread", e.spread);
        if (const auto text = read("gravity")) {
            const auto value = scene_effect_value(Type::Vector2, *text);
            if (!value) {
                refuse("gravity", "two numbers");
            }
            e.gravity = {(*value)[0], (*value)[1]};
            changed = true;
        }
        range("scale", e.scale);
        if (const auto text = read("color")) {
            // The color's straight linear RGB at every key, each key's alpha times its alpha.
            const auto value = scene_effect_value(Type::Color, *text);
            if (!value) {
                refuse("color", "a color");
            }
            for (auto& key : e.color) {
                key = {(*value)[0], (*value)[1], (*value)[2], key[3] * (*value)[3]};
            }
            changed = true;
        }
        if (!changed) {
            return found->second;
        }
        if (auto valid = checked(e, registered.sprite); !valid) {
            throw_diagnostic(Diag::SceneProfile, "particles " + std::string(name) + ": " + valid.error().message);
        }
        return std::make_shared<const SceneEmitter>(SceneEmitter{std::move(e), registered.sprite});
    }
} // namespace weft
