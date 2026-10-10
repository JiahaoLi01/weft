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

    // M0 font adapter. Every operation and destruction belongs to its creator.
    // No Unicode line breaking, bidi paragraph analysis, fallback, or editing yet.
    class Font final {
    public:
        explicit Font(const std::filesystem::path& font);
        ~Font();
        Font(const Font&) = delete;
        Font& operator=(const Font&) = delete;
        FontMetrics metrics(float pixels) const;
        // Explicit inline probe policy: OpenType USE_TYPO_METRICS, otherwise Win
        // ascent/descent, rounded at logical 1x. Legacy design metrics stay unchanged.
        FontMetrics inline_metrics(float pixels) const;
        ShapedText shape(std::string_view utf8, float pixels) const;
        GlyphBitmap rasterize(std::uint32_t glyph_id, float pixels);
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
