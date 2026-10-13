#pragma once

#include "paint/display_list.hpp"

#include <array>
#include <cstdint>

// The particle model of paint::Particles: what each particle of an emission is at the emission's elapsed time. The
// scene shader (src/render/shaders/scene.hlsl, WEFT_PARTICLES) computes the same values with the same formulas in
// float; this is their reference, and the source of the per-emission values the planner writes into the record.
namespace weft::paint {
    // An emission's timing at its elapsed time, in what the shader reads: the particles its item draws, and for a
    // stream the age of its newest particle, the time between emissions and the newest particle's index (modulo
    // constants::particle_index_period); for a burst its elapsed time. Computed in double, rounded once.
    struct ParticleTiming {
        std::uint32_t count{};
        float age{};
        float interval{};
        std::uint32_t newest{};
    };

    ParticleTiming particle_timing(const Particles& particles);

    // A particle's random value k in [0, 1): an integer hash of the seed (its low 24 bits), the particle's index and k,
    // as the shader computes it. particle_state draws its values in a fixed order of k.
    float particle_random(std::uint32_t seed, std::uint32_t particle, std::uint32_t k);

    // Particle instance (below the timing's count) at the emission's elapsed time: whether it lives, its centre in item
    // px, its size across, its angle in degrees, its straight linear color and its sprite frame.
    struct ParticleState {
        bool alive{};
        std::array<float, 2> position{};
        float size{};
        float angle{};
        Color color{};
        std::uint32_t frame{};
    };

    ParticleState particle_state(const Particles& particles, const ParticleTiming& timing, std::uint32_t instance);
} // namespace weft::paint
