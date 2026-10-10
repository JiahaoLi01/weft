#pragma once

#include <cstdint>

// Fixture and stress parameters of the renderer tests (resource lifetime and backend C ABI).
namespace caseelephant::test_constants::render {
    // ResourceLifetime randomized create/prepare/encode/submit cycles.
    inline constexpr std::uint32_t lifetime_stress_seed = 0xCE102026;
    inline constexpr int lifetime_stress_cycles = 2000;

    // Schema tag ("MTL") the fake backend expects in its native pass payload.
    inline constexpr int fake_pass_schema = 0x4d544c;

    // Randomized external texture import/encode/retire iterations.
    inline constexpr std::uint32_t external_mutation_seed = 0xce112026;
    inline constexpr int external_mutation_iterations = 512;
} // namespace caseelephant::test_constants::render
