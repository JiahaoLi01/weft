#pragma once

#include "paint/display_list.hpp"

#include <memory>

// UI particles as scene elements show them (docs/architecture.md, "粒子"): an emitter, and the emissions an element
// shows at a frame, which the display list draws as paint::Particles items in the element's place.
//
// Owner thread of the scene. An emitter is an immutable value that frames carry to the render thread.
namespace weft {
    // An emitter as elements show it: its description, and its sprite's asset (the description's sprite index is the
    // display list's to set).
    struct SceneEmitter {
        paint::ParticleEmitter emitter;
        std::shared_ptr<const paint::Asset> sprite;

        friend bool operator==(const SceneEmitter&, const SceneEmitter&) = default;
    };

    // One emission an element shows: its emitter, and the emission at the frame's time (its elapsed time and the rest),
    // in the element's border box space.
    struct SceneEmission {
        std::shared_ptr<const SceneEmitter> emitter;
        paint::ParticleEmission emission;

        friend bool operator==(const SceneEmission& a, const SceneEmission& b) {
            return (a.emitter == b.emitter || (a.emitter && b.emitter && *a.emitter == *b.emitter)) &&
                   a.emission == b.emission;
        }
    };
} // namespace weft
