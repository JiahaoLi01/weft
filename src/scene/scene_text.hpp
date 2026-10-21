#pragma once

#include "paint/display_list.hpp"
#include "style/values.hpp"
#include "text/font.hpp"
#include "text/font_catalog.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Portable scene text: CSS font matching over a FontCatalog, HarfBuzz shaping with per-cluster fallback and
// Blink's single-line box metrics. Owner-thread only, like the fonts it holds. Renderers draw the shaped glyph
// runs; coverage and SDF glyph images come from the same faces.
namespace weft {
    using SceneStyle = StyleValues;

    struct SceneTextMetrics {
        float width{};
        float height{};
        // Distance from the top of the line box to the baseline.
        float baseline{};
    };

    // Typed font properties of a scene text node's computed style.
    struct SceneFont {
        std::string families;
        float size{16};
        int weight{400};
        bool italic{};
        // font-variant-numeric as SceneNumeric bits.
        std::uint8_t numeric{};
        // Extra advance after each character cluster, in px.
        float letter_spacing{};
        // Used line height in px; empty for 'normal'.
        std::optional<float> line_height;
    };

    // The numeric glyph variants of font-variant-numeric, each an OpenType feature.
    enum SceneNumeric : std::uint8_t {
        LiningNums = 1,
        OldstyleNums = 2,
        ProportionalNums = 4,
        TabularNums = 8,
        DiagonalFractions = 16,
        StackedFractions = 32,
        Ordinal = 64,
        SlashedZero = 128,
    };

    // font-family, font-size (px), font-weight (normal/bold/number), font-style, font-variant-numeric, letter-spacing
    // (px or em of the font size) and line-height (normal, px or a multiple of the font size).
    SceneFont scene_font(const SceneStyle& style);

    struct SceneGlyph {
        // Index in SceneText's face table.
        std::uint32_t face{};
        std::uint32_t glyph_id{};
        // Pen position relative to the run origin on the baseline, y down, in px.
        float x{};
        float y{};
        // Advance including letter-spacing.
        float advance{};
        std::uint32_t utf8_cluster{};
        // The run asks for italics and the face has none: drawn slanted (constants::text::synthetic_oblique_skew),
        // as Chrome synthesizes it.
        bool oblique{};
    };

    struct SceneTextRun {
        std::vector<SceneGlyph> glyphs;
        float size{};
        SceneTextMetrics metrics;
    };

    // A font's strut: its primary face's ascent and descent as Blink rounds them (the content area of an inline box),
    // and the extents above and below the baseline that its line-height gives a line box.
    struct SceneStrut {
        float ascent{};
        float descent{};
        float above{};
        float below{};
    };

    // A text's em box about its baseline, as Blink's em height (ComputeEmHeight) gives it.
    struct SceneEmBox {
        float ascent{};
        float descent{};
    };

    class SceneText {
    public:
        explicit SceneText(FontCatalog catalog);
        ~SceneText();
        SceneText(const SceneText&) = delete;
        SceneText& operator=(const SceneText&) = delete;

        SceneTextMetrics measure(std::string_view text, const SceneStyle& style);
        // Shaped single line; the run stays valid while the caller holds it, also after cache eviction.
        std::shared_ptr<const SceneTextRun> shape(std::string_view text, const SceneStyle& style);
        std::shared_ptr<const SceneTextRun> shape(std::string_view text, const SceneFont& font);
        // The line box of the run's bytes [begin, end) alone, as measure() of just those bytes gives it when the
        // run's words break there: the advances of the glyphs whose cluster lies in the range, and the faces they use.
        SceneTextMetrics slice(const SceneTextRun& run, const SceneFont& font, std::uint32_t begin, std::uint32_t end);
        SceneStrut strut(const SceneFont& font);
        // The em box of text in style: the em metrics (Font::em_metrics) of the faces its glyphs use, each side the
        // largest of theirs rounded up to a whole px and no more than the primary face's ascent and descent. Ruby
        // annotations stand on it.
        SceneEmBox em_box(std::string_view text, const SceneStyle& style);
        // Whether the font's primary face (the first that its families match) has a glyph for a character.
        bool primary_covers(const SceneFont& font, char32_t character);

        // The run's glyph outlines as one path, its origin the top left of the line box (the baseline at
        // metrics.baseline); null when a glyph comes from a color face. Kept while the cache holds it.
        std::shared_ptr<const paint::Path> outline(std::string_view text, const SceneStyle& style);

        const FontFaceSource& face_source(std::uint32_t face) const;
        Font& face(std::uint32_t face);
        bool color_face(std::uint32_t face) const;

        struct Stats {
            std::size_t faces{};
            std::size_t files{};
            std::size_t shape_hits{};
            std::size_t shape_misses{};
        };

        Stats stats() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace weft
