#pragma once

#include "paint/display_list.hpp"
#include "style/values.hpp"
#include "text/bidi.hpp"
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

        friend bool operator==(const SceneTextMetrics&, const SceneTextMetrics&) = default;
    };

    // Typed font properties of a scene text node's computed style.
    struct SceneFont {
        std::string families;
        // The size its faces are made at: the computed font-size truncated to Blink's platform font precision
        // (constants::text::font_size_precision).
        float size{16};
        int weight{400};
        bool italic{};
        // font-variant-numeric as SceneNumeric bits.
        std::uint8_t numeric{};
        // Extra advance after each character cluster, in px.
        float letter_spacing{};
        // Used line height in px; empty for 'normal'.
        std::optional<float> line_height;
        // direction and unicode-bidi: the base direction its text is shaped in (the bidirectional algorithm orders its
        // runs; plaintext takes its first strong character's), or, with bidi_override (bidi-override and
        // isolate-override), the one direction all of it is shaped in. An inline formatting context shapes each run of
        // one level alone, overridden to that level's direction.
        BidiBase base{BidiBase::Ltr};
        bool bidi_override{};
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
    // (px or em of the font size), line-height (normal, px or a multiple of the font size), direction (ltr, rtl) and
    // unicode-bidi (normal, embed, isolate, bidi-override, isolate-override, plaintext).
    SceneFont scene_font(const SceneStyle& style);

    struct SceneGlyph {
        // Index in the face table of the SceneText that shaped its run (SceneTextRun::text_id).
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
        // It comes from a right-to-left run (an odd bidi level): its cluster starts on its right.
        bool rtl{};

        friend bool operator==(const SceneGlyph&, const SceneGlyph&) = default;
    };

    struct SceneTextRun {
        std::vector<SceneGlyph> glyphs;
        float size{};
        SceneTextMetrics metrics;
        // The SceneText that shaped it (none: 0), and where each face its glyphs use comes from, by the number that
        // SceneText gave it. A face's number means nothing to another SceneText (each numbers its faces as it opens
        // them): a run laid out in one and drawn in another goes through SceneText::adopted.
        std::uint64_t text_id{};
        std::vector<std::pair<std::uint32_t, FontFaceSource>> faces;

        friend bool operator==(const SceneTextRun&, const SceneTextRun&) = default;
    };

    // Whether two runs, either of them none, are none or alike.
    inline bool same_run(const std::shared_ptr<const SceneTextRun>& a, const std::shared_ptr<const SceneTextRun>& b) {
        return a == b || (a && b && *a == *b);
    }

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
        explicit SceneText(std::shared_ptr<const FontCatalog> catalog);
        // The catalog that matches font-family lists from now on (a View's: the runtime's fonts and its page's
        // @font-face faces): a renderer draws the frames of Views of several catalogs with one SceneText. Faces are
        // shared between catalogs by source; what a catalog matched and shaped stays apart. The constructor's catalog
        // is the first.
        void use(const std::shared_ptr<const FontCatalog>& catalog);
        // The catalog in use, as a number that no other catalog this SceneText used takes (caches outside it key on
        // it: a renderer's text images).
        std::uint64_t catalog_id() const;
        ~SceneText();
        SceneText(const SceneText&) = delete;
        SceneText& operator=(const SceneText&) = delete;

        SceneTextMetrics measure(std::string_view text, const SceneStyle& style);
        // Shaped single line; the run stays valid while the caller holds it, also after cache eviction. context: the
        // text around it (an inline formatting context's paragraph around a run or a line's piece), which joins and
        // substitutes across the boundary as Blink shapes with its paragraph's text.
        std::shared_ptr<const SceneTextRun> shape(std::string_view text, const SceneStyle& style,
                                                  const ShapeContext& context = {});
        std::shared_ptr<const SceneTextRun> shape(std::string_view text, const SceneFont& font,
                                                  const ShapeContext& context = {});
        // A run another SceneText shaped with its faces numbered for this one, which opens those it has not (a layout's
        // run on a renderer's thread); the run itself when this one shaped it.
        std::shared_ptr<const SceneTextRun> adopted(const std::shared_ptr<const SceneTextRun>& run);
        // The line box of the run's bytes [begin, end) alone, as measure() of just those bytes gives it when the
        // run's words break there: the advances of the glyphs whose cluster lies in the range, and the faces they use.
        SceneTextMetrics slice(const SceneTextRun& run, const SceneFont& font, std::uint32_t begin, std::uint32_t end);
        SceneStrut strut(const SceneFont& font);
        // The x-height of the font's primary face (Font::x_height): vertical-align: middle centers on half of it.
        float x_height(const SceneFont& font);
        // The em box of text in style: the em metrics (Font::em_metrics) of the faces its glyphs use, each side the
        // largest of theirs rounded up to a whole px and no more than the primary face's ascent and descent. Ruby
        // annotations stand on it.
        SceneEmBox em_box(std::string_view text, const SceneStyle& style);
        // Whether the font's primary face (the first that its families match) has a glyph for a character.
        bool primary_covers(const SceneFont& font, char32_t character);
        // What sizes a text field (Blink's LayoutTextControl): the advance of the primary face's 'x' (its average
        // character width) and the width of its bounding box (its maximum character width).
        std::pair<float, float> field_char_widths(const SceneFont& font);

        // The run's glyph outlines as one path, its origin the top left of the line box (the baseline at
        // metrics.baseline); null when a glyph comes from a color face. Kept while the cache holds it.
        std::shared_ptr<const paint::Path> outline(std::string_view text, const SceneStyle& style);

        const FontFaceSource& face_source(std::uint32_t face) const;
        Font& face(std::uint32_t face);
        bool color_face(std::uint32_t face) const;

        // A glyph's coverage as face(face).rasterize(glyph_id, pixels, offset_x, offset_y, skew) gives it, kept for the
        // next call with the same arguments (bounded by constants::scene::text_glyph_cache_max_entries and _bytes; a
        // full cache starts over). Still text's images ask for their glyphs here: their pens lie on quarter pixels
        // across and whole rows down, so a line whose text changes (a clock, a counter) rasterizes only the glyphs it has
        // not drawn before. The bitmap stays valid while the caller holds it.
        std::shared_ptr<const GlyphBitmap> glyph(std::uint32_t face, std::uint32_t glyph_id, float pixels,
                                                 float offset_x, float offset_y, float skew = 0);

        struct Stats {
            std::size_t faces{};
            std::size_t files{};
            std::size_t shape_hits{};
            std::size_t shape_misses{};
            // The glyph cache (glyph()): its entries and bytes, and how often it had and lacked a glyph.
            std::size_t glyphs{};
            std::size_t glyph_bytes{};
            std::size_t glyph_hits{};
            std::size_t glyph_misses{};
        };

        Stats stats() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace weft
