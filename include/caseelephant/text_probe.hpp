#pragma once
#include "caseelephant/constants/text.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace caseelephant {
    struct ProbeGlyph {
        std::uint32_t glyph_id{};
        std::uint32_t utf8_cluster{};
        float advance_x{};
        float advance_y{};
        float offset_x{};
        float offset_y{};
        std::uint32_t font_id{}; // Index in the owning immutable font set; single font = 0.
    };

    struct ProbeShapedText {
        std::vector<ProbeGlyph> glyphs;
        float advance{};
        bool missing_glyph{};
    };

    struct ProbeFontMetrics {
        float ascent{};
        float descent{};
        float line_gap{};
    };

    struct ProbeGlyphBitmap {
        std::uint32_t width{};
        std::uint32_t height{};
        int left{};
        int top{};
        std::vector<std::uint8_t> coverage;
    };
    enum class ProbeSdfSource {
        Bitmap,
        Outline
    };

    struct ProbeSdfBitmap {
        std::uint32_t width{};
        std::uint32_t height{};
        // Includes distance padding; does not change advance.
        int left{};
        int top{};
        std::uint32_t base_ppem{};
        std::uint32_t spread{};
        ProbeSdfSource source{ProbeSdfSource::Bitmap};
        // FreeType encoding: distance = (byte - 128) * spread / 128.
        // Positive inside, zero at 128. Linear data, never sRGB/coverage.
        std::vector<std::uint8_t> distances;
    };

    // M0 font adapter. Every operation and destruction belongs to its creator.
    // No Unicode line breaking, bidi paragraph analysis, fallback, or editing yet.
    class TextProbeFont final {
    public:
        explicit TextProbeFont(const std::filesystem::path& font);
        ~TextProbeFont();
        TextProbeFont(const TextProbeFont&) = delete;
        TextProbeFont& operator=(const TextProbeFont&) = delete;
        ProbeFontMetrics metrics(float pixels) const;
        // Explicit inline probe policy: OpenType USE_TYPO_METRICS, otherwise Win
        // ascent/descent, rounded at logical 1x. Legacy design metrics stay unchanged.
        ProbeFontMetrics inline_metrics(float pixels) const;
        ProbeShapedText shape(std::string_view utf8, float pixels) const;
        ProbeGlyphBitmap rasterize(std::uint32_t glyph_id, float pixels);
        ProbeGlyphBitmap rasterize(const ProbeGlyph& glyph, float pixels);
        ProbeSdfBitmap rasterize_sdf(std::uint32_t glyph_id,
                                     std::uint32_t base_ppem = constants::text::sdf_default_base_ppem,
                                     std::uint32_t spread = constants::text::sdf_default_spread,
                                     ProbeSdfSource source = ProbeSdfSource::Bitmap);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    struct ProbeFontRun {
        std::uint32_t font_id{};
        std::uint32_t utf8_start{};
        std::uint32_t utf8_length{};
        ProbeFontMetrics metrics;
    };

    struct ProbeFallbackText {
        ProbeShapedText shaped;
        std::vector<ProbeFontRun> runs; // Logical order; no FT/HB/VM pointers.
    };

    struct ProbeShapeCacheStats {
        std::size_t entries{};
        std::size_t bytes{};
        std::size_t hits{};
        std::size_t misses{};
    };

    // Owner-only font resource collection. Bounded LTR Latin/Han/Common/Inherited
    // profile, HarfBuzz clusters for fallback; paragraph bidi/line breaking pending.
    // Entire missing clusters change face, adjacent spans reshape together.
    class TextProbeFontSet final {
    public:
        explicit TextProbeFontSet(const std::vector<std::filesystem::path>& ordered_fonts);
        ~TextProbeFontSet();
        TextProbeFontSet(const TextProbeFontSet&) = delete;
        TextProbeFontSet& operator=(const TextProbeFontSet&) = delete;
        ProbeFallbackText shape(std::string_view utf8, float pixels) const;
        ProbeShapeCacheStats shape_cache_stats() const;
        ProbeFontMetrics metrics(std::uint32_t font_id, float pixels) const;
        ProbeFontMetrics inline_metrics(std::uint32_t font_id, float pixels) const;
        ProbeGlyphBitmap rasterize(const ProbeGlyph& glyph, float pixels);
        TextProbeFont& primary();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace caseelephant
