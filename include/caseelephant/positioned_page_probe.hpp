#pragma once
#include "caseelephant/page_probe.hpp"
#include "caseelephant/style_probe.hpp"
#include "caseelephant/rounded_probe.hpp"
#include <string_view>
#include <optional>

namespace caseelephant {
    // A bounded formatting context for definite-size positioned art surfaces.
    // Separate from the Flex profile until a document formatting dispatcher exists.
    struct ProbePositionedPaint {
        ProbePagePaint paint;
        std::string image_source;
        std::array<float, 4> uv{0, 0, 1, 1};
        std::array<float, 4> clip{}; // Absolute x/y/width/height; glyph quads clip later.
        std::vector<ProbeRoundedClip> rounded_clips;
        std::optional<ProbeNodeIdentity> canvas_identity; // raster lookup by owner, never by renderer
    };

    struct ProbePositionedPage {
        ProbePage geometry;
        std::vector<ProbePositionedPaint> paints;
        std::vector<ProbeElementGeometry> elements; // owner geometry reads, not a hit-test contract
    };

    // Shared bounded author style evaluator; definite-size formatting remains an
    // independent adapter. A paired style subtree can inherit document :root vars.
    ProbePositionedPage build_positioned_page_probe(const ProbeDomSnapshot& root, std::string_view stylesheet,
                                                    float viewport_width, float viewport_height, TextProbeFont& font);
    ProbePositionedPage build_positioned_page_probe(const ProbeDomSnapshot& root, std::string_view stylesheet,
                                                    float viewport_width, float viewport_height,
                                                    TextProbeFontSet& fonts);
    ProbePositionedPage build_positioned_page_probe(const ProbeDomSnapshot& root, const ProbeComputedStyle& style,
                                                    float viewport_width, float viewport_height, TextProbeFont& font);
    ProbePositionedPage build_positioned_page_probe(const ProbeDomSnapshot& root, const ProbeComputedStyle& style,
                                                    float viewport_width, float viewport_height,
                                                    TextProbeFontSet& fonts);
} // namespace caseelephant
