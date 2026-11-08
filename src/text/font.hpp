#pragma once
#include "base/function_ref.hpp"
#include "constants/text.hpp"
#include "text/mapped_file.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

// HarfBuzz's blob and face (hb.h).
struct hb_blob_t;
struct hb_face_t;

namespace weft {
    // A face's unicode-range (CSS Fonts 4): the code points first through last. A face with none covers every one.
    using UnicodeRange = std::array<char32_t, 2>;

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

    // A run of text of one direction and one script, as shaping takes it: HarfBuzz shapes it in that direction (a
    // right-to-left run's glyphs come in visual order, left to right) with that script's shaper. script is an ISO
    // 15924 tag (script_runs); 0 lets HarfBuzz guess the script and direction from the text.
    struct ShapeSegment {
        bool rtl{};
        std::uint32_t script{};
    };

    // The text around what is shaped, as HarfBuzz's pre- and post-context (UTF-8): an Arabic letter joins, and a
    // contextual substitution looks, across the boundary without the context being shaped (Blink passes its paragraph's
    // text). HarfBuzz reads up to five characters of each.
    struct ShapeContext {
        std::string_view before;
        std::string_view after;
    };

    struct FontMetrics {
        float ascent{};
        float descent{};
        float line_gap{};
    };

    // Blink's split of a line box's leading around the content area: floor(leading / 2) whole pixels above, the rest
    // of the LayoutUnit-rounded leading below. Returns {above, below}.
    std::pair<float, float> split_leading(float leading);

    struct GlyphBitmap {
        std::uint32_t width{};
        std::uint32_t height{};
        int left{};
        int top{};
        std::vector<std::uint8_t> coverage;
    };

    // A glyph's outline, y down from its origin on the baseline: contours of lines and quadratic and cubic curves.
    // Move and Line take one point, Quadratic two (control, end), Cubic three (two controls, end), Close none.
    struct GlyphOutline {
        enum class Verb : std::uint8_t {
            Move,
            Line,
            Quadratic,
            Cubic,
            Close
        };

        std::vector<Verb> verbs;
        std::vector<std::array<float, 2>> points;
        bool evenodd{};
    };

    // A color glyph's pixels: premultiplied encoded sRGB RGBA, placed like GlyphBitmap.
    struct ColorGlyphBitmap {
        std::uint32_t width{};
        std::uint32_t height{};
        int left{};
        int top{};
        std::vector<std::uint8_t> rgba;
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

    // An OpenType feature over the whole text, e.g. {'tnum', 1}.
    struct FontFeature {
        std::uint32_t tag{};
        std::uint32_t value{};
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
        // The file's bytes when they come from memory (a page's @font-face source, which the host's reader gave): the
        // path then only names them.
        std::shared_ptr<const std::vector<char>> data;

        friend bool operator==(const FontFaceSource&, const FontFaceSource&) = default;
        friend auto operator<=>(const FontFaceSource&, const FontFaceSource&) = default;
    };

    // Immutable bytes of one font file, shared by the faces and instances read from it, on any thread: the file mapped
    // into memory (text/mapped_file.hpp), so that only what its faces read of it is read from disk (a CJK font is tens
    // of MB), else read whole.
    class FontFile final {
    public:
        explicit FontFile(const std::filesystem::path& path);
        ~FontFile();
        FontFile(const FontFile&) = delete;
        FontFile& operator=(const FontFile&) = delete;

        // Bytes in memory (a page's @font-face source): held as they are, not copied.
        explicit FontFile(std::shared_ptr<const std::vector<char>> data);

        // The file of a path as long as any thread's fonts hold it, else opened anew: one copy of its bytes for them all.
        static std::shared_ptr<const FontFile> open(const std::filesystem::path& path);
        // The file of a face's source: its bytes in memory, or its path's (open).
        static std::shared_ptr<const FontFile> of(const FontFaceSource& source);

        std::span<const char> bytes() const noexcept {
            return bytes_;
        }

        // A new reference to the HarfBuzz face of one of the file's faces, made once. HarfBuzz faces may be shared
        // between threads, and a table is read and checked when the face first reads it: once for all the fonts of the
        // face. Its color tables (an emoji font's COLR is megabytes) are read when it is made, not when a color glyph
        // is first drawn.
        hb_face_t* harfbuzz_face(std::uint32_t face_index) const;

    private:
        // The mapped file, the bytes read in its place, or bytes held in memory; bytes_ views one of them.
        std::unique_ptr<const MappedFile> mapped_;
        std::vector<char> read_;
        std::shared_ptr<const std::vector<char>> held_;
        std::span<const char> bytes_;
        mutable std::mutex faces_lock_;
        mutable hb_blob_t* blob_{};
        mutable std::map<std::uint32_t, hb_face_t*> faces_;
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
        // The face's em box as Blink takes it (NormalizedTypoAscentAndDescent): its OS/2 typo ascender and descender,
        // else its line metrics' ascent and descent, scaled to sum to pixels, the ascent rounded to a LayoutUnit.
        FontMetrics em_metrics(float pixels) const;
        // The width of the face's bounding box (its head table's xMax - xMin) at pixels per em: Blink's maximum
        // character width (SimpleFontData::MaxCharWidth).
        float bounding_width(float pixels) const;
        // The x-height at pixels per em, as DirectWrite (and Chrome on Windows) gives it: the OS/2 sxHeight of a
        // version 2 or later table, else the top of the face's 'x', else Blink's guess for a face that reports none
        // (a share of the ascent).
        float x_height(float pixels) const;
        // True when the face has COLR, CBDT, sbix or SVG color glyph data.
        bool has_color_glyphs() const;
        // Glyph of a Unicode code point, 0 when the face has none.
        std::uint32_t glyph_index(char32_t codepoint) const;
        // HarfBuzz's default features, and `features` over the whole text; as segment says when given, otherwise in
        // the direction and script HarfBuzz guesses.
        ShapedText shape(std::string_view utf8, float pixels, ShapePrecision precision = ShapePrecision::Fixed26_6,
                         std::span<const FontFeature> features = {},
                         const std::optional<ShapeSegment>& segment = std::nullopt,
                         const ShapeContext& context = {}) const;
        GlyphBitmap rasterize(std::uint32_t glyph_id, float pixels);
        // Coverage of the outline with its origin offset_x right of and offset_y below a whole pixel origin
        // (subpixel positioning, y down); left/top stay relative to that whole pixel origin. skew slants the outline
        // about its baseline: x moves right by skew times the height above it (a synthetic oblique).
        GlyphBitmap rasterize(std::uint32_t glyph_id, float pixels, float offset_x, float offset_y, float skew = 0);
        GlyphBitmap rasterize(const Glyph& glyph, float pixels);
        // The unhinted outline at pixels per em, as rasterize draws it (skew too); empty for a glyph without one.
        GlyphOutline outline(std::uint32_t glyph_id, float pixels, float skew = 0);
        // The glyph's color paint (a COLRv1 graph or COLRv0 layers; an outline without one is the foreground) at
        // pixels per em, its origin offset like rasterize's; foreground is the text's straight color. Empty when the
        // glyph paints nothing; none for paint the canvas cannot draw (PNG or SVG images, ColorCanvas bounds).
        std::optional<ColorGlyphBitmap> rasterize_color(std::uint32_t glyph_id, float pixels, float offset_x,
                                                        float offset_y, const std::array<float, 4>& foreground,
                                                        float skew = 0);
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

    // Where a font of a fallback list comes from: the CSS font-family list, or the system's fallback, monochrome or
    // color.
    enum class FontRole : std::uint8_t {
        Listed,
        FallbackText,
        FallbackColor,
    };

    // Shapes text with the first font and moves every cluster it cannot render to the first later font
    // that covers it; adjacent clusters of one font are shaped together. Glyph font_id is the index in
    // fonts. With a segment, the text is one run of its direction and script (the caller itemized it: SceneText), and
    // a right-to-left run's glyphs come in visual order. Without one, a bounded profile of left-to-right scripts that
    // shape without script itemization: Latin, Greek, Cyrillic, Han, Hiragana, Katakana, Hangul, Bopomofo, Common and
    // Inherited (no bidi; other text is WEFT_TEXT_SCRIPT).
    // With roles (one per font), the listed fonts are tried in order first; then, as Chrome's system fallback
    // chooses, an emoji shown as text (by default, or with VS15) prefers a monochrome fallback font and one shown as
    // emoji (by default, or with VS16) a color one. With ranges (one list per font, an empty one for every code
    // point), a font covers only the clusters whose code points all lie in its unicode-range, the first font too.
    FallbackText shape_with_fallback(const std::vector<Font*>& fonts, std::string_view utf8, float pixels,
                                     ShapePrecision precision = ShapePrecision::Fixed26_6,
                                     std::span<const FontFeature> features = {}, std::span<const FontRole> roles = {},
                                     std::span<const std::vector<UnicodeRange>> ranges = {},
                                     const std::optional<ShapeSegment>& segment = std::nullopt,
                                     const ShapeContext& context = {});

    // The fonts of a fallback list as shaping asks for them, so that a font no cluster reaches need not be opened:
    // `count` fonts, font(i) the i-th, and role(i) its role when roles is set (one per font as above).
    struct FallbackFonts {
        std::size_t count{};
        FunctionRef<Font&(std::uint32_t)> font;
        FunctionRef<FontRole(std::uint32_t)> role;
        bool roles{};
    };

    // shape_with_fallback over fonts asked for as they are needed: the first font always, a later one only when a
    // cluster no earlier font renders (or prefers) reaches it, its role only for a cluster whose emoji presentation
    // chooses between fallback fonts.
    FallbackText shape_with_fallback(const FallbackFonts& fonts, std::string_view utf8, float pixels,
                                     ShapePrecision precision = ShapePrecision::Fixed26_6,
                                     std::span<const FontFeature> features = {},
                                     std::span<const std::vector<UnicodeRange>> ranges = {},
                                     const std::optional<ShapeSegment>& segment = std::nullopt,
                                     const ShapeContext& context = {});

    // Whether a code point lies in a unicode-range (an empty one is every code point).
    bool in_unicode_range(std::span<const UnicodeRange> ranges, char32_t codepoint);

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
