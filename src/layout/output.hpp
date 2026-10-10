#pragma once

#include "layout/flex.hpp"
#include "render/rounded_clip.hpp"
#include "script/script_dom.hpp"
#include "text/font.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

// Owned layout output shared by the document formatter, input routing and packet builders: boxes, paint items, hit
// regions and element geometry. No DOM nodes, VM values or Yoga nodes.
namespace weft {
    enum class PaintKind {
        Rectangle,
        Text
    };

    struct PagePaint {
        PaintKind kind{};
        std::string id;
        std::string text;
        float x{};
        float y{};
        float width{};
        float height{};
        float font_size{};
        std::array<float, 4> srgb{};
        ShapedText shaped; // Logical glyphs and positions, no atlas coordinates.
    };

    struct Page {
        struct Hit {
            NodeIdentity identity;
            std::size_t parent; // Index in hits; root uses size_t max. No Node/JSValue.
            float x{};
            float y{};
            float width{};
            float height{};
            bool enabled{};
            bool disabled_button{};
        };

        std::vector<FlexBox> boxes;
        std::vector<PagePaint> paints;
        std::vector<Hit> hits; // Logical identities/parents; legacy profile also orders these by paint.

        struct HitRegion {
            std::size_t hit; // Index in the logical identity/parent tree above.
            std::array<float, 4> rect{};
            std::array<float, 4> clip{};
            std::vector<RoundedClip> rounded_clips;
        };

        // Optional explicit paint-order regions. Inline fragments are separate;
        // their logical parent indices remain stable when stacking reorders paint.
        std::vector<HitRegion> hit_regions;
    };

    // One paint item with its clip chain and replaced-content source.
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

} // namespace weft
