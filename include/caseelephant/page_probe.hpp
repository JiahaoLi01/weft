#pragma once
#include "caseelephant/script_probe.hpp"
#include "caseelephant/flex_probe.hpp"
#include "caseelephant/style_probe.hpp"
#include "caseelephant/rounded_probe.hpp"
#include <array>

namespace caseelephant {
    enum class ProbePaintKind {
        Rectangle,
        Text
    };

    struct ProbePagePaint {
        ProbePaintKind kind{};
        std::string id;
        std::string text;
        float x{};
        float y{};
        float width{};
        float height{};
        float font_size{};
        std::array<float, 4> srgb{};
        ProbeShapedText shaped; // Logical glyphs and positions, no atlas coordinates.
    };

    struct ProbePage {
        struct Hit {
            ProbeNodeIdentity identity;
            std::size_t parent; // Index in hits; root uses size_t max. No Node/JSValue.
            float x{};
            float y{};
            float width{};
            float height{};
            bool enabled{};
            bool disabled_button{};
        };

        std::vector<FlexProbeBox> boxes;
        std::vector<ProbePagePaint> paints;
        std::vector<Hit> hits; // Logical identities/parents; legacy profile also orders these by paint.

        struct HitRegion {
            std::size_t hit; // Index in the logical identity/parent tree above.
            std::array<float, 4> rect{};
            std::array<float, 4> clip{};
            std::vector<ProbeRoundedClip> rounded_clips;
        };

        // Optional explicit paint-order regions. Inline fragments are separate;
        // their logical parent indices remain stable when stacking reorders paint.
        std::vector<HitRegion> hit_regions;
    };

    // Experimental inline-style/flex/single-line HTML profile. Explicit diagnostics
    // for unsupported formatting and paint. Uses shared bounded style cascade.
    ProbePage build_page_probe(const ProbeDomSnapshot& root, TextProbeFont& font);
    ProbePage build_page_probe(const ProbeDomSnapshot& root, std::string_view stylesheet, TextProbeFont& font);
} // namespace caseelephant
