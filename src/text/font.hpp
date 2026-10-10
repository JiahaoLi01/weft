#pragma once
#include "constants/text.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace weft {
    struct Glyph {
        std::uint32_t glyph_id{};
        std::uint32_t utf8_cluster{};
        float advance_x{};
        float advance_y{};
        float offset_x{};
        float offset_y{};
        std::uint32_t font_id{}; // Index in the owning immutable font set; single font = 0.
    };

    struct ShapedText {
        std::vector<Glyph> glyphs;
        float advance{};
        bool missing_glyph{};
    };

    struct FontMetrics {
        float ascent{};
        float descent{};
        float line_gap{};
    };

    struct GlyphBitmap {
        std::uint32_t width{};
        std::uint32_t height{};
        int left{};
        int top{};
        std::vector<std::uint8_t> coverage;
    };
    enum class SdfSource {
        Bitmap,
        Outline
    };

    struct SdfBitmap {
        std::uint32_t width{};
        std::uint32_t height{};
        // Includes distance padding; does not change advance.
        int left{};
        int top{};
        std::uint32_t base_ppem{};
        std::uint32_t spread{};
        SdfSource source{SdfSource::Bitmap};
        // FreeType encoding: distance = (byte - 128) * spread / 128.
        // Positive inside, zero at 128. Linear data, never sRGB/coverage.
        std::vector<std::uint8_t> distances;
    };

    // Fixed-point unit of HarfBuzz positions. Layout probes keep 26.6 (1/64 px per glyph); Chrome positions in 16.16.
    enum class ShapePrecision {
        Fixed26_6,
        Fixed16_16
    };

    // Variable font axis value in design coordinates, e.g. {'wght', 700}.
    struct FontVariation {
        std::uint32_t tag{};
        float value{};

        friend bool operator==(const FontVariation&, const FontVariation&) = default;
        friend auto operator<=>(const FontVariation&, const FontVariation&) = default;
    };

    // One face of a font file: the index inside a TrueType/OpenType collection and the axis values of a
    // variable font (unlisted axes keep their defaults).
    struct FontFaceSource {
        std::filesystem::path path;
        std::uint32_t face_index{};
        std::vector<FontVariation> variations;

        friend bool operator==(const FontFaceSource&, const FontFaceSource&) = default;
        friend auto operator<=>(const FontFaceSource&, const FontFaceSource&) = default;
    };

    // Immutable bytes of one font file, shared by the faces and instances read from it.
    class FontFile final {
    public:
        explicit FontFile(const std::filesystem::path& path);

        const std::vector<char>& bytes() const noexcept {
            return bytes_;
        }

    private:
        std::vector<char> bytes_;
    };

    // M0 font adapter. Every operation and destruction belongs to its creator.
    // No Unicode line breaking, bidi paragraph analysis, fallback, or editing yet.
    class Font final {
    public:
        explicit Font(const std::filesystem::path& font);
        explicit Font(const FontFaceSource& source);
        Font(std::shared_ptr<const FontFile> file, std::uint32_t face_index,
             const std::vector<FontVariation>& variations = {});
        ~Font();
        Font(const Font&) = delete;
        Font& operator=(const Font&) = delete;
        FontMetrics metrics(float pixels) const;
        // Explicit inline probe policy: OpenType USE_TYPO_METRICS, otherwise Win
        // ascent/descent, rounded at logical 1x. Legacy design metrics stay unchanged.
        FontMetrics inline_metrics(float pixels) const;
        // Unrounded line metrics as DirectWrite (and Chrome on Windows) derive them: the OS/2 typo values
        // with USE_TYPO_METRICS, otherwise Win ascent/descent and the part of the hhea line spacing that
        // exceeds them as line gap.
        FontMetrics line_metrics(float pixels) const;
        // True when the face has COLR, CBDT, sbix or SVG color glyph data.
        bool has_color_glyphs() const;
        // Glyph of a Unicode code point, 0 when the face has none.
        std::uint32_t glyph_index(char32_t codepoint) const;
        ShapedText shape(std::string_view utf8, float pixels,
                         ShapePrecision precision = ShapePrecision::Fixed26_6) const;
        GlyphBitmap rasterize(std::uint32_t glyph_id, float pixels);
        // Coverage of the outline with its origin offset_x right of and offset_y below a whole pixel origin
        // (subpixel positioning, y down); left/top stay relative to that whole pixel origin.
        GlyphBitmap rasterize(std::uint32_t glyph_id, float pixels, float offset_x, float offset_y);
        GlyphBitmap rasterize(const Glyph& glyph, float pixels);
        SdfBitmap rasterize_sdf(std::uint32_t glyph_id,
                                std::uint32_t base_ppem = constants::text::sdf_default_base_ppem,
                                std::uint32_t spread = constants::text::sdf_default_spread,
                                SdfSource source = SdfSource::Bitmap);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    struct FontRun {
        std::uint32_t font_id{};
        std::uint32_t utf8_start{};
        std::uint32_t utf8_length{};
        FontMetrics metrics;
    };

    struct FallbackText {
        ShapedText shaped;
        std::vector<FontRun> runs; // Logical order; no FT/HB/VM pointers.
    };

    struct ShapeCacheStats {
        std::size_t entries{};
        std::size_t bytes{};
        std::size_t hits{};
        std::size_t misses{};
    };

    // Shapes text with the first font and moves every cluster it cannot render to the first later font
    // that covers it; adjacent clusters of one font are shaped together. Glyph font_id is the index in
    // fonts. Bounded LTR Latin/Han/Common/Inherited profile (no bidi or script itemization yet).
    FallbackText shape_with_fallback(const std::vector<Font*>& fonts, std::string_view utf8, float pixels,
                                     ShapePrecision precision = ShapePrecision::Fixed26_6);

    // Owner-only font resource collection. Bounded LTR Latin/Han/Common/Inherited
    // profile, HarfBuzz clusters for fallback; paragraph bidi/line breaking pending.
    // Entire missing clusters change face, adjacent spans reshape together.
    class FontSet final {
    public:
        explicit FontSet(const std::vector<std::filesystem::path>& ordered_fonts);
        ~FontSet();
        FontSet(const FontSet&) = delete;
        FontSet& operator=(const FontSet&) = delete;
        FallbackText shape(std::string_view utf8, float pixels) const;
        ShapeCacheStats shape_cache_stats() const;
        FontMetrics metrics(std::uint32_t font_id, float pixels) const;
        FontMetrics inline_metrics(std::uint32_t font_id, float pixels) const;
        GlyphBitmap rasterize(const Glyph& glyph, float pixels);
        Font& primary();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace weft
