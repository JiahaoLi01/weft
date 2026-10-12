#include "scene/scene_text.hpp"

#include "base/css_scalar.hpp"
#include "base/diagnostic.hpp"
#include "base/owner_thread.hpp"
#include "constants/common.hpp"
#include "constants/scene.hpp"
#include "constants/text.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <tuple>

namespace weft {
    namespace {
        using constants::text::layout_units_per_px;

        float number(std::string_view value) {
            const auto n = css::parse_number<float>(std::string(value),
                                                    static_cast<float>(constants::scene::max_numeric_magnitude));
            if (!n) {
                throw_diagnostic(Diag::SceneRaster, "font numeric value " + std::string(value));
            }
            return *n;
        }

        // px, em (of reference), % (of reference) and unitless px.
        float length(std::string_view input, float reference) {
            const auto s = css::trim(input);
            const auto [digits, unit] = css::split_unit(s, {.px = true, .percent = true, .em = true});
            const float n = number(digits);
            if (unit == css::Unit::Percent) {
                return n * reference / constants::percent_scale;
            }
            return n * (unit == css::Unit::Em ? reference : 1);
        }

        // UTF-8 code point at i; advances i. Malformed input is left to HarfBuzz, which rejects it.
        char32_t next_codepoint(std::string_view text, std::size_t& i) {
            const auto lead = static_cast<unsigned char>(text[i]);
            const int length = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
            char32_t c = length == 1 ? lead : lead & (0x3f >> (length - 1));
            for (int k = 1; k < length && i + k < text.size(); ++k) {
                c = (c << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3f);
            }
            i = std::min(text.size(), i + length);
            return c;
        }

        // CJK ideographs, kana, CJK symbols and punctuation, compatibility and full-width forms.
        bool cjk(char32_t c) {
            return (c >= 0x2e80 && c <= 0x9fff) || (c >= 0xf900 && c <= 0xfaff) || (c >= 0xfe30 && c <= 0xfe4f) ||
                   (c >= 0xff00 && c <= 0xffef) || (c >= 0x20000 && c <= 0x3ffff);
        }

        // Variation selectors and joiners stay with the character before them.
        bool attaches(char32_t c) {
            return (c >= 0xfe00 && c <= 0xfe0f) || c == 0x200d || (c >= 0xe0100 && c <= 0xe01ef);
        }

        // Blink shapes a line word by word (CachingWordShaper): each space or tab is a word of its own, and so is
        // each CJK ideograph or symbol. Kerning and contextual substitution never cross these boundaries, which
        // also keeps digits after a Latin word in their own script run.
        std::vector<std::pair<std::size_t, std::size_t>> words(std::string_view text) {
            std::vector<std::pair<std::size_t, std::size_t>> out;
            std::size_t start = 0;
            std::size_t i = 0;
            while (i < text.size()) {
                const auto at = i;
                const auto c = next_codepoint(text, i);
                if (c == ' ' || c == '\t' || cjk(c)) {
                    if (at > start) {
                        out.emplace_back(start, at);
                    }
                    while (i < text.size()) {
                        auto peek = i;
                        if (!attaches(next_codepoint(text, peek))) {
                            break;
                        }
                        i = peek;
                    }
                    out.emplace_back(at, i);
                    start = i;
                }
            }
            if (start < text.size()) {
                out.emplace_back(start, text.size());
            }
            return out;
        }

    } // namespace

    SceneFont scene_font(const SceneStyle& style) {
        SceneFont font;
        font.families = std::string(style.text(PropertyId::FontFamily, constants::scene::default_text_families));
        font.size = length(std::string(style.text(PropertyId::FontSize, constants::default_font_size)),
                           constants::default_font_size_px);
        const auto weight = std::string(style.text(PropertyId::FontWeight, constants::default_font_weight));
        font.weight = weight == "bold"     ? constants::scene::bold_font_weight_value
                      : weight == "normal" ? constants::scene::normal_font_weight_value
                                           : int(number(weight));
        const auto italic = std::string(style.text(PropertyId::FontStyle, "normal"));
        font.italic = italic == "italic" || italic.starts_with("oblique");
        const auto spacing = std::string(style.text(PropertyId::LetterSpacing, "0"));
        font.letter_spacing = spacing == "normal" ? 0 : length(spacing, font.size);
        const auto line = std::string(style.text(PropertyId::LineHeight, "normal"));
        if (line != "normal") {
            // split_unit returns views: keep the trimmed string alive.
            const auto trimmed = css::trim(line);
            const auto [digits, unit] = css::split_unit(trimmed, {.px = true, .percent = true, .em = true});
            font.line_height = unit == css::Unit::None ? number(digits) * font.size : length(line, font.size);
        }
        if (!std::isfinite(font.size) || font.size <= 0 || font.size > constants::text::max_font_pixels) {
            throw_diagnostic(Diag::SceneRaster, "font-size out of range");
        }
        return font;
    }

    struct SceneText::Impl {
        OwnerThread owner;
        FontCatalog catalog;

        struct Face {
            FontFaceSource source;
            std::unique_ptr<Font> font;
            bool color{};
        };

        std::vector<Face> faces;
        std::map<FontFaceSource, std::uint32_t> face_ids;
        std::map<std::filesystem::path, std::shared_ptr<const FontFile>> files;
        std::size_t file_bytes{};
        std::map<std::tuple<std::string, int, bool>, std::vector<std::uint32_t>> matches;
        using RunKey = std::tuple<std::string, std::string, float, int, bool, float, float>;
        std::map<RunKey, std::shared_ptr<const SceneTextRun>> runs;
        std::size_t run_bytes{};
        // Run outlines by the run's key; null for runs with color glyphs.
        std::map<RunKey, std::shared_ptr<const paint::Path>> outlines;
        std::size_t outline_bytes{};
        std::size_t hits{};
        std::size_t misses{};

        void require_owner() const {
            owner.require(Diag::SceneRasterOwner);
        }

        std::uint32_t face(const FontFaceSource& source) {
            if (const auto i = face_ids.find(source); i != face_ids.end()) {
                return i->second;
            }
            auto& file = files[source.path];
            if (!file) {
                file = std::make_shared<const FontFile>(source.path);
                file_bytes += file->bytes().size();
                if (file_bytes > constants::scene::text_font_max_bytes) {
                    throw_diagnostic(Diag::SceneRaster, "scene font file byte bound");
                }
            }
            if (faces.size() >= constants::scene::text_max_faces) {
                throw_diagnostic(Diag::SceneRaster, "scene font face bound");
            }
            auto font = std::make_unique<Font>(file, source.face_index, source.variations);
            const bool color = font->has_color_glyphs();
            faces.push_back({source, std::move(font), color});
            const auto id = std::uint32_t(faces.size() - 1);
            face_ids.emplace(source, id);
            return id;
        }

        const std::vector<std::uint32_t>& matched(const SceneFont& f) {
            auto key = std::tuple{f.families, f.weight, f.italic};
            if (auto i = matches.find(key); i != matches.end()) {
                return i->second;
            }
            auto sources = match_font_faces(catalog, f.families, f.weight, f.italic);
            if (sources.empty()) {
                throw_diagnostic(Diag::SceneRaster, "no font for font-family " + f.families);
            }
            if (sources.size() > constants::text::max_fallback_fonts) {
                sources.resize(constants::text::max_fallback_fonts);
            }
            std::vector<std::uint32_t> ids;
            for (const auto& source : sources) {
                ids.push_back(face(source));
            }
            if (matches.size() >= constants::scene::text_cache_max_entries) {
                matches.clear();
            }
            return matches.emplace(std::move(key), std::move(ids)).first->second;
        }

        std::shared_ptr<const SceneTextRun> shape(std::string_view text, const SceneFont& f) {
            require_owner();
            if (text.size() > constants::max_text_bytes) {
                throw_diagnostic(Diag::SceneRaster, "scene text byte bound");
            }
            RunKey key{std::string(text),         f.families, f.size, f.weight, f.italic, f.letter_spacing,
                       f.line_height.value_or(-1)};
            if (auto i = runs.find(key); i != runs.end()) {
                ++hits;
                return i->second;
            }
            ++misses;
            const auto ids = matched(f);
            std::vector<Font*> fonts;
            for (auto id : ids) {
                fonts.push_back(faces[id].font.get());
            }
            auto run = std::make_shared<SceneTextRun>();
            run->size = f.size;
            float pen{};
            std::set<std::uint32_t> used{ids.front()};
            for (const auto& [begin, end] : words(text)) {
                // Chrome positions glyphs in HarfBuzz 16.16 units.
                const auto shaped =
                    shape_with_fallback(fonts, text.substr(begin, end - begin), f.size, ShapePrecision::Fixed16_16);
                const auto& glyphs = shaped.shaped.glyphs;
                for (std::size_t i = 0; i < glyphs.size(); ++i) {
                    const auto& g = glyphs[i];
                    const auto face = ids.at(g.font_id);
                    used.insert(face);
                    // Letter-spacing follows each character cluster, including the last one.
                    const bool cluster_end = i + 1 == glyphs.size() || glyphs[i + 1].utf8_cluster != g.utf8_cluster;
                    const float advance = g.advance_x + (cluster_end ? f.letter_spacing : 0);
                    run->glyphs.push_back({face, g.glyph_id, pen + g.offset_x, -g.offset_y, advance,
                                           std::uint32_t(begin + g.utf8_cluster)});
                    pen += advance;
                }
            }
            // Line box of one line: the primary font's strut, plus the fonts actually used when line-height is
            // normal (Blink includes fallback font metrics only then).
            float above{};
            float below{};
            for (auto id : used) {
                if (f.line_height && id != ids.front()) {
                    continue;
                }
                const auto m = faces[id].font->line_metrics(f.size);
                const float ascent = std::round(m.ascent);
                const float descent = std::round(m.descent);
                const float leading = f.line_height ? *f.line_height - (ascent + descent) : std::round(m.line_gap);
                const auto [top, bottom] = split_leading(leading);
                above = std::max(above, ascent + top);
                below = std::max(below, descent + bottom);
            }
            // The inline size is a LayoutUnit rounded up from the summed advances.
            const float width = std::ceil(pen * layout_units_per_px) / layout_units_per_px;
            run->metrics = {width, above + below, above};
            const auto bytes = text.size() + run->glyphs.size() * sizeof(SceneGlyph);
            if (runs.size() >= constants::scene::text_cache_max_entries ||
                run_bytes + bytes > constants::scene::text_cache_max_bytes) {
                runs.clear();
                run_bytes = 0;
            }
            run_bytes += bytes;
            runs.emplace(std::move(key), run);
            return run;
        }
    };

    std::shared_ptr<const paint::Path> SceneText::outline(std::string_view text, const SceneStyle& style) {
        impl_->require_owner();
        const auto f = scene_font(style);
        Impl::RunKey key{std::string(text),         f.families, f.size, f.weight, f.italic, f.letter_spacing,
                         f.line_height.value_or(-1)};
        if (auto i = impl_->outlines.find(key); i != impl_->outlines.end()) {
            return i->second;
        }
        const auto run = impl_->shape(text, f);
        auto path = std::make_shared<paint::Path>();
        bool color{};
        // Outlines load at a multiple of the size, for points finer than FreeType's 1/64 px.
        const float oversample = constants::scene::text_outline_oversample;
        for (const auto& g : run->glyphs) {
            const auto& face = impl_->faces[g.face];
            if (face.color) {
                color = true;
                break;
            }
            const auto glyph = face.font->outline(g.glyph_id, run->size * oversample);
            const float ox = g.x;
            const float oy = run->metrics.baseline + g.y;
            const auto at = [&](std::size_t i) {
                return std::array<float, 2>{glyph.points[i][0] / oversample + ox, glyph.points[i][1] / oversample + oy};
            };
            std::size_t p{};
            std::array<float, 2> current{};
            for (const auto verb : glyph.verbs) {
                using Verb = GlyphOutline::Verb;
                switch (verb) {
                    case Verb::Move:
                        current = at(p++);
                        path->verbs.push_back(paint::Path::Verb::Move);
                        path->points.push_back(current);
                        break;
                    case Verb::Line:
                        current = at(p++);
                        path->verbs.push_back(paint::Path::Verb::Line);
                        path->points.push_back(current);
                        break;
                    case Verb::Quadratic: {
                        const auto c = at(p);
                        const auto end = at(p + 1);
                        p += 2;
                        path->verbs.push_back(paint::Path::Verb::Cubic);
                        path->points.push_back(
                            {current[0] + 2.f / 3 * (c[0] - current[0]), current[1] + 2.f / 3 * (c[1] - current[1])});
                        path->points.push_back(
                            {end[0] + 2.f / 3 * (c[0] - end[0]), end[1] + 2.f / 3 * (c[1] - end[1])});
                        path->points.push_back(end);
                        current = end;
                        break;
                    }
                    case Verb::Cubic:
                        path->verbs.push_back(paint::Path::Verb::Cubic);
                        path->points.push_back(at(p));
                        path->points.push_back(at(p + 1));
                        path->points.push_back(at(p + 2));
                        current = at(p + 2);
                        p += 3;
                        break;
                    case Verb::Close:
                        path->verbs.push_back(paint::Path::Verb::Close);
                        break;
                }
            }
        }
        std::shared_ptr<const paint::Path> result;
        if (!color) {
            result = std::move(path);
        }
        const auto bytes =
            text.size() + (result ? result->verbs.size() + result->points.size() * sizeof(float) * 2 : 0);
        if (impl_->outlines.size() >= constants::scene::text_cache_max_entries ||
            impl_->outline_bytes + bytes > constants::scene::text_cache_max_bytes) {
            impl_->outlines.clear();
            impl_->outline_bytes = 0;
        }
        impl_->outline_bytes += bytes;
        impl_->outlines.emplace(std::move(key), result);
        return result;
    }

    SceneText::SceneText(FontCatalog catalog) : impl_(std::make_unique<Impl>()) {
        impl_->catalog = std::move(catalog);
    }

    SceneText::~SceneText() {
        impl_->owner.require_or_terminate();
    }

    SceneTextMetrics SceneText::measure(std::string_view text, const SceneStyle& style) {
        return shape(text, style)->metrics;
    }

    std::shared_ptr<const SceneTextRun> SceneText::shape(std::string_view text, const SceneStyle& style) {
        impl_->require_owner();
        return impl_->shape(text, scene_font(style));
    }

    std::shared_ptr<const SceneTextRun> SceneText::shape(std::string_view text, const SceneFont& font) {
        return impl_->shape(text, font);
    }

    const FontFaceSource& SceneText::face_source(std::uint32_t face) const {
        impl_->require_owner();
        return impl_->faces.at(face).source;
    }

    Font& SceneText::face(std::uint32_t face) {
        impl_->require_owner();
        return *impl_->faces.at(face).font;
    }

    bool SceneText::color_face(std::uint32_t face) const {
        impl_->require_owner();
        return impl_->faces.at(face).color;
    }

    SceneText::Stats SceneText::stats() const {
        impl_->require_owner();
        return {impl_->faces.size(), impl_->files.size(), impl_->hits, impl_->misses};
    }
} // namespace weft
