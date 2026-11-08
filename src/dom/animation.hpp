#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace weft {
    // A script animation of an element (Web Animations 1, element.animate() and Animation), as its Animation object last
    // set it (src/script/js/scene_platform_bootstrap.js): the keyframe effect's keyframes and timing, and where the
    // animation's time stands. A scene solves the effect's values from it at each frame's time (scene.cpp).
    struct DomAnimation {
        // A keyframe: its computed offset, its easing to the next, and its values as CSS declarations.
        struct Keyframe {
            double offset{};
            std::string easing = "linear";
            std::string declarations;

            friend bool operator==(const Keyframe&, const Keyframe&) = default;
        };

        // The Animation's, in creation order: the composite order of the element's script animations.
        std::uint64_t id{};
        std::vector<Keyframe> keyframes;
        // The effect's timing (EffectTiming, resolved): ms; iterations may be infinite; fill none, forwards, backwards or
        // both (auto is none); direction normal, reverse, alternate or alternate-reverse.
        double delay{};
        double end_delay{};
        double iteration_start{};
        double iterations{1};
        double duration{};
        std::string direction = "normal";
        std::string fill = "none";
        std::string easing = "linear";
        // The current time: hold_time when it holds (paused, pending, finished), else (the frame's time - start_time) x
        // rate; neither, idle (no effect).
        std::optional<double> start_time;
        std::optional<double> hold_time;
        double rate{1};

        friend bool operator==(const DomAnimation&, const DomAnimation&) = default;
    };

    // An element's script animations, by id.
    using DomAnimations = std::vector<DomAnimation>;
} // namespace weft
