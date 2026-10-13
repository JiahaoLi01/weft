#pragma once

#include "base/result.hpp"
#include "dom/node.hpp"
#include "paint/display_list.hpp"

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// UI particles as scene elements show them (docs/architecture.md, "粒子"): the emitters a host registers by name, which
// -weft-particles: name and weft.emitParticles(element, name, options) emit from an element, and the emissions an
// element shows at a frame, which the display list draws as paint::Particles items in the element's place.
//
// Owner thread of the scene. An emitter is an immutable value that frames carry to the render thread.
namespace weft {
    // An emitter as elements show it: its description, with its area relative to the element's border box (a point or
    // a disc's centre as fractions of the box's width and height, a disc's radius as a fraction of half the box's
    // shorter side, a rectangle's or an outline's x, y, width and height as fractions), and its sprite's asset (the
    // description's sprite index is the display list's to set).
    struct SceneEmitter {
        paint::ParticleEmitter emitter;
        std::shared_ptr<const paint::Asset> sprite;

        friend bool operator==(const SceneEmitter&, const SceneEmitter&) = default;
    };

    // One emission an element shows: its emitter, and the emission at the frame's time (its elapsed time and the rest),
    // a target in the element's border box px.
    struct SceneEmission {
        std::shared_ptr<const SceneEmitter> emitter;
        paint::ParticleEmission emission;

        friend bool operator==(const SceneEmission& a, const SceneEmission& b) {
            return (a.emitter == b.emitter || (a.emitter && b.emitter && *a.emitter == *b.emitter)) &&
                   a.emission == b.emission;
        }
    };

    // A burst weft.emitParticles asks of an element: how many particles, over how long (seconds), and where they fly
    // to: the centre of a target element, or a point in the viewport's CSS px.
    struct SceneBurst {
        std::uint32_t count{};
        float duration{};
        std::optional<NodeIdentity> target;
        std::optional<std::array<float, 2>> point;
    };

    // The area of an emitter relative to a border box of width x height (SceneEmitter), in the box's px.
    std::array<float, 4> scene_emitter_area(const paint::ParticleEmitter& emitter, float width, float height);

    // Bounds (left, top, right, bottom, in the box's px) of every particle the emission can draw at any time on a
    // border box of width x height: where particles start, how far they move in the longest life, and half the
    // diagonal of the largest particle. Damage takes them instead of the whole target.
    std::array<float, 4> scene_particle_reach(const SceneEmission& emission, float width, float height);

    class SceneParticles {
    public:
        // Registers emitter under name, its area relative (SceneEmitter), with its sprite's asset when it shows one:
        // coverage, or sRGB color. Diag::SceneProfile for a name registered before, a distance field as a sprite, or
        // values the paint checks refuse (paint::check).
        Status add(std::string name, paint::ParticleEmitter emitter, std::shared_ptr<const paint::Asset> sprite = {});

        // The emitter registered as name, on an element whose computed custom properties are custom (names with their
        // leading "--"): its own values with the overrides of --particles-rate (particles per second),
        // --particles-life and --particles-speed (a value, or a range of two), --particles-direction and
        // --particles-spread (degrees), --particles-gravity (x and y, px/s²), --particles-scale (a size factor, or a
        // range of two) and --particles-color (a CSS color for every key of the color curve, the keys' alphas kept).
        // Throws DiagnosticError (Diag::SceneProfile) for a name nobody registered, an override that is not such a
        // value, or values the paint checks refuse.
        std::shared_ptr<const SceneEmitter>
        resolve(std::string_view name, const std::map<std::string, std::optional<std::string>>& custom) const;

    private:
        std::map<std::string, std::shared_ptr<const SceneEmitter>, std::less<>> emitters_;
    };
} // namespace weft
