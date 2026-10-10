#pragma once
#include "caseelephant/script_probe.hpp"
#include "caseelephant/flex_probe.hpp"
#include "caseelephant/style_probe.hpp"
#include "caseelephant/rounded_probe.hpp"
#include <array>

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

    // Experimental inline-style/flex/single-line HTML profile. Explicit diagnostics
    // for unsupported formatting and paint. Uses shared bounded style cascade.
    Page build_page(const DomSnapshot& root, Font& font);
    Page build_page(const DomSnapshot& root, std::string_view stylesheet, Font& font);
} // namespace weft
