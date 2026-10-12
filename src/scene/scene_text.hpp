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
        // Extra advance after each character cluster, in px.
        float letter_spacing{};
        // Used line height in px; empty for 'normal'.
        std::optional<float> line_height;
    };

    // font-family, font-size (px), font-weight (normal/bold/number), font-style, letter-spacing (px or em of the
    // font size) and line-height (normal, px or a multiple of the font size).
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
    };

    struct SceneTextRun {
        std::vector<SceneGlyph> glyphs;
        float size{};
        SceneTextMetrics metrics;
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
