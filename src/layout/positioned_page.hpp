#pragma once
#include "layout/page.hpp"
#include "style/style.hpp"
#include "render/rounded_clip.hpp"
#include <string_view>
#include <optional>

namespace weft {
    // A bounded formatting context for definite-size positioned art surfaces.
    // Separate from the Flex profile until a document formatting dispatcher exists.
    struct PositionedPaint {
        PagePaint paint;
        std::string image_source;
        std::array<float, 4> uv{0, 0, 1, 1};
        std::array<float, 4> clip{}; // Absolute x/y/width/height; glyph quads clip later.
        std::vector<RoundedClip> rounded_clips;
        std::optional<NodeIdentity> canvas_identity; // raster lookup by owner, never by renderer
    };

    struct PositionedPage {
        Page geometry;
        std::vector<PositionedPaint> paints;
        std::vector<ElementGeometry> elements; // owner geometry reads, not a hit-test contract
    };

    // Shared bounded author style evaluator; definite-size formatting remains an
    // independent adapter. A paired style subtree can inherit document :root vars.
    PositionedPage build_positioned_page(const DomSnapshot& root, std::string_view stylesheet, float viewport_width,
                                         float viewport_height, Font& font);
    PositionedPage build_positioned_page(const DomSnapshot& root, std::string_view stylesheet, float viewport_width,
                                         float viewport_height, FontSet& fonts);
    PositionedPage build_positioned_page(const DomSnapshot& root, const ComputedStyle& style, float viewport_width,
                                         float viewport_height, Font& font);
    PositionedPage build_positioned_page(const DomSnapshot& root, const ComputedStyle& style, float viewport_width,
                                         float viewport_height, FontSet& fonts);
} // namespace weft
