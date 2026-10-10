#pragma once
#include <array>
#include <cstdint>
#include <vector>
namespace caseelephant {
struct ProbeRoundedClip {
    std::array<float,4> rect{};
    std::array<float,4> radius_x{},radius_y{}; // TL, TR, BR, BL, normalized.
};
ProbeRoundedClip normalize_rounded_probe(ProbeRoundedClip);
bool contains_rounded_probe(const ProbeRoundedClip&,float x,float y);
// Backend-independent coverage preparation. No DOM, JS, or GPU references.
std::vector<std::uint8_t> rounded_coverage_probe(const std::vector<ProbeRoundedClip>&,
    std::array<float,4> rect,std::uint32_t width,std::uint32_t height);
}
