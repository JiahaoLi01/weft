#pragma once

#include <cstdint>

// Fixture and stress parameters of the renderer tests (resource lifetime and backend C ABI).
namespace weft::test_constants::render {
    // ResourceLifetime randomized create/prepare/encode/submit cycles.
    inline constexpr std::uint32_t lifetime_stress_seed = 0xCE102026;
    inline constexpr int lifetime_stress_cycles = 2000;

    // Schema tag ("MTL") the fake backend expects in its native pass payload.
    inline constexpr int fake_pass_schema = 0x4d544c;

    // Randomized external texture import/encode/retire iterations.
    inline constexpr std::uint32_t external_mutation_seed = 0xce112026;
    inline constexpr int external_mutation_iterations = 512;

    // Randomized export encode/release/complete steps over one ring.
    inline constexpr std::uint32_t export_mutation_seed = 0xce1b2026;
    inline constexpr int export_mutation_steps = 4000;

    // Random scene pass schedules (scene_schedule_contract): the seed, the passes and the most steps of one.
    inline constexpr std::uint32_t schedule_seed = 0xce172026;
    inline constexpr int schedule_passes = 2000;
    inline constexpr int schedule_batches = 40;
    // constants::render::scene's rounding and budget of hoisted group images, as the schedule test checks them.
    inline constexpr std::uint32_t group_image_granularity = 64;
    inline constexpr std::uint64_t hoisted_group_budget = 1;
} // namespace weft::test_constants::render
