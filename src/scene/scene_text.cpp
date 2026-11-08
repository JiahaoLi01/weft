#include "scene/scene_text.hpp"

#include "base/css_scalar.hpp"
#include "base/diagnostic.hpp"
#include "base/owner_thread.hpp"
#include "constants/common.hpp"
#include "constants/scene.hpp"
#include "constants/text.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

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

        // The last and first characters of a context that HarfBuzz reads (constants::text::shape_context_characters).
        std::string_view context_tail(std::string_view s) {
            std::size_t at = s.size();
            for (int n = 0; n < constants::text::shape_context_characters && at > 0; ++n) {
                do {
                    --at;
                } while (at > 0 && (static_cast<unsigned char>(s[at]) & 0xC0) == 0x80);
            }
            return s.substr(at);
        }

        std::string_view context_head(std::string_view s) {
            std::size_t at = 0;
            for (int n = 0; n < constants::text::shape_context_characters && at < s.size(); ++n) {
                std::size_t i = at;
                next_codepoint(s, i);
                at = i;
            }
            return s.substr(0, at);
        }

        // The parts a run is shaped in: each CJK ideograph or symbol (with what attaches to it) alone, the text between
        // them whole. Spaces stay in the text around them, as Blink's LayoutNG shapes a run (HarfBuzzShaper segments
        // it by script, emoji and orientation only): a font that kerns a space with the letter after it (Roboto's
        // " T") is kerned as in Chrome.
        std::vector<std::pair<std::size_t, std::size_t>> words(std::string_view text) {
            std::vector<std::pair<std::size_t, std::size_t>> out;
            std::size_t start = 0;
            std::size_t i = 0;
            while (i < text.size()) {
                const auto at = i;
                const auto c = next_codepoint(text, i);
                if (cjk(c)) {
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
        // A value that does not parse names its property.
        const auto named = [](const char* property, std::string_view value, auto parse) {
            try {
                return parse();
            } catch (const std::invalid_argument&) {
                throw_diagnostic(Diag::SceneRaster,
                                 std::string(property) + ": the font value '" + std::string(value) + "'");
            }
        };
        const auto size_text = std::string(style.text(PropertyId::FontSize, constants::default_font_size));
        font.size = named("font-size", size_text, [&] {
            return length(size_text, constants::default_font_size_px);
        });
        const auto weight = std::string(style.text(PropertyId::FontWeight, constants::default_font_weight));
        font.weight = weight == "bold"     ? constants::scene::bold_font_weight_value
                      : weight == "normal" ? constants::scene::normal_font_weight_value
                                           : named("font-weight", weight, [&] {
                                                 return int(number(weight));
                                             });
        const auto italic = std::string(style.text(PropertyId::FontStyle, "normal"));
        font.italic = italic == "italic" || italic.starts_with("oblique");
        // font-variant-numeric: normal, or at most one value of each group (figures, spacing, fractions) with ordinal
        // and slashed-zero.
        const auto numeric = css::trim(style.text(PropertyId::FontVariantNumeric, "normal"));
        if (numeric != "normal") {
            constexpr std::array<std::pair<std::string_view, std::uint8_t>, 8> values{{
                {"lining-nums", LiningNums},
                {"oldstyle-nums", OldstyleNums},
                {"proportional-nums", ProportionalNums},
                {"tabular-nums", TabularNums},
                {"diagonal-fractions", DiagonalFractions},
                {"stacked-fractions", StackedFractions},
                {"ordinal", Ordinal},
                {"slashed-zero", SlashedZero},
            }};
            for (const auto& word : css::split(numeric, ' ').parts) {
                const auto w = css::trim(word);
                if (w.empty()) {
                    continue;
                }
                const auto it = std::find_if(values.begin(), values.end(), [&](const auto& v) {
                    return v.first == w;
                });
                if (it == values.end() || (font.numeric & it->second)) {
                    throw_diagnostic(Diag::SceneProfile, "font-variant-numeric " + std::string(numeric));
                }
                font.numeric |= it->second;
            }
            for (const auto pair :
                 {LiningNums | OldstyleNums, ProportionalNums | TabularNums, DiagonalFractions | StackedFractions}) {
                if ((font.numeric & pair) == pair) {
                    throw_diagnostic(Diag::SceneProfile, "font-variant-numeric " + std::string(numeric));
                }
            }
        }
        const auto spacing = std::string(style.text(PropertyId::LetterSpacing, "0"));
        font.letter_spacing = spacing == "normal" ? 0 : named("letter-spacing", spacing, [&] {
            return length(spacing, font.size);
        });
        const auto line = std::string(style.text(PropertyId::LineHeight, "normal"));
        if (line != "normal") {
            // split_unit returns views: keep the trimmed string alive.
            const auto trimmed = css::trim(line);
            const auto [digits, unit] = css::split_unit(trimmed, {.px = true, .percent = true, .em = true});
            font.line_height = named("line-height", line, [&] {
                return unit == css::Unit::None ? number(digits) * font.size : length(line, font.size);
            });
        }
        const auto direction = css::trim(style.text(PropertyId::Direction, "ltr"));
        if (direction != "ltr" && direction != "rtl") {
            throw_diagnostic(Diag::SceneRaster, "direction: the value '" + std::string(direction) + "'");
        }
        const auto bidi = css::trim(style.text(PropertyId::UnicodeBidi, "normal"));
        if (bidi != "normal" && bidi != "embed" && bidi != "isolate" && bidi != "bidi-override" &&
            bidi != "isolate-override" && bidi != "plaintext") {
            throw_diagnostic(Diag::SceneRaster, "unicode-bidi: the value '" + std::string(bidi) + "'");
        }
        font.base = bidi == "plaintext" ? BidiBase::Auto : direction == "rtl" ? BidiBase::Rtl : BidiBase::Ltr;
        font.bidi_override = bidi == "bidi-override" || bidi == "isolate-override";
        if (!std::isfinite(font.size) || font.size <= 0 || font.size > constants::text::max_font_pixels) {
            throw_diagnostic(Diag::SceneRaster, "font-size out of range");
        }
        // The faces' size (line-height and letter-spacing above took the computed one).
        if (const float platform = float(static_cast<unsigned>(font.size * constants::text::font_size_precision)) /
                                   constants::text::font_size_precision;
            platform > 0) {
            font.size = platform;
        }
        return font;
    }

    struct SceneText::Impl {
        OwnerThread owner;
        // The catalog in use and its number; the numbers of the catalogs used before (an expired one takes a new
        // number if its address comes back).
        std::shared_ptr<const FontCatalog> catalog;
        std::uint64_t catalog_id{};
        std::map<const FontCatalog*, std::pair<std::weak_ptr<const FontCatalog>, std::uint64_t>> catalog_ids;
        std::uint64_t next_catalog_id{};

        struct Face {
            FontFaceSource source;
            std::unique_ptr<Font> font;
            bool color{};
            // The catalog lists the face as italic.
            bool italic{};
        };

        std::vector<Face> faces;
        std::map<FontFaceSource, std::uint32_t> face_ids;
        // By path, and for bytes in memory by the bytes too (their path only names them).
        std::map<std::pair<std::filesystem::path, const std::vector<char>*>, std::shared_ptr<const FontFile>> files;
        std::size_t file_bytes{};

        // A font-family list's faces in fallback order: their sources and unicode-ranges, how many come from the list
        // (the rest are the system's fallback), and the first available font (CSS Fonts 4, 4.5: the first whose
        // unicode-range includes U+0020), whose metrics are the line's. A face opens when shaping first reaches it, the
        // first available one at once: text that the first fonts render opens none after them (a page's text no emoji
        // font, Chinese text none of the list's later CJK fonts), and a font file is read only for a face that opens.
        struct Match {
            std::vector<FontFaceSource> sources;
            std::vector<std::vector<UnicodeRange>> ranges;
            // Each face's id, once open.
            std::vector<std::optional<std::uint32_t>> ids;
            std::size_t listed{};
            std::uint32_t primary{};
        };

        // A match's i-th face, opened.
        std::uint32_t id(Match& m, std::uint32_t i) {
            if (!m.ids[i]) {
                m.ids[i] = face(m.sources[i]);
            }
            return *m.ids[i];
        }

        // Where a match's i-th face comes from: the list, or the system's fallback, monochrome or color (which opening
        // the face tells).
        FontRole role(Match& m, std::uint32_t i) {
            return i < m.listed            ? FontRole::Listed
                   : faces[id(m, i)].color ? FontRole::FallbackColor
                                           : FontRole::FallbackText;
        }

        // By catalog, families, weight, style and script.
        std::map<std::tuple<std::uint64_t, std::string, int, bool, std::uint32_t>, Match> matches;
        // By catalog, text and font.
        // After the line height: the base direction, and 4 when overridden; then the context before and after (the
        // few characters HarfBuzz reads of it).
        using RunKey = std::tuple<std::uint64_t, std::string, std::string, float, int, bool, std::uint8_t, float, float,
                                  std::uint8_t, std::string, std::string>;

        static RunKey run_key(std::uint64_t catalog, std::string_view text, const SceneFont& f,
                              const ShapeContext& context = {}) {
            return {catalog,
                    std::string(text),
                    f.families,
                    f.size,
                    f.weight,
                    f.italic,
                    f.numeric,
                    f.letter_spacing,
                    f.line_height.value_or(-1),
                    std::uint8_t(std::uint8_t(f.base) | (f.bidi_override ? 4 : 0)),
                    std::string(context_tail(context.before)),
                    std::string(context_head(context.after))};
        }

        std::map<RunKey, std::shared_ptr<const SceneTextRun>> runs;
        std::size_t run_bytes{};
        // Run outlines by the run's key; null for runs with color glyphs.
        std::map<RunKey, std::shared_ptr<const paint::Path>> outlines;
        std::size_t outline_bytes{};
        std::size_t hits{};
        std::size_t misses{};

        // Glyph coverage (glyph()) by face, glyph and the bits of the size, offsets and skew it was rasterized at: the
        // same bits rasterize the same coverage.
        using GlyphKey = std::array<std::uint32_t, 6>;

        struct GlyphKeyHash {
            std::size_t operator()(const GlyphKey& key) const noexcept {
                std::uint64_t h = 1469598103934665603ull;
                for (const auto word : key) {
                    h = (h ^ word) * 1099511628211ull;
                }
                return std::size_t(h);
            }
        };

        std::unordered_map<GlyphKey, std::shared_ptr<const GlyphBitmap>, GlyphKeyHash> glyphs;
        std::size_t glyph_bytes{};
        std::size_t glyph_hits{};
        std::size_t glyph_misses{};

        void require_owner() const {
            owner.require(Diag::SceneRasterOwner);
        }

        std::uint32_t face(const FontFaceSource& source) {
            if (const auto i = face_ids.find(source); i != face_ids.end()) {
                return i->second;
            }
            auto& file = files[{source.path, source.data.get()}];
            if (!file) {
                file = FontFile::of(source);
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
            const bool italic = std::any_of(catalog->faces.begin(), catalog->faces.end(), [&](const FontFaceEntry& e) {
                return e.italic && e.source == source;
            });
            faces.push_back({source, std::move(font), color, italic});
            const auto id = std::uint32_t(faces.size() - 1);
            face_ids.emplace(source, id);
            return id;
        }

        // With a script (an ISO 15924 tag), the platform's fallback fonts for it come before the general ones.
        Match& matched(const SceneFont& f, std::uint32_t script = 0) {
            auto key = std::tuple{catalog_id, f.families, f.weight, f.italic, script};
            if (auto i = matches.find(key); i != matches.end()) {
                return i->second;
            }
            std::size_t listed{};
            std::vector<std::vector<UnicodeRange>> ranges;
            auto sources = match_font_faces(*catalog, f.families, f.weight, f.italic, &listed, &ranges, script);
            if (sources.empty()) {
                throw_diagnostic(Diag::SceneRaster, "no font for font-family " + f.families);
            }
            if (sources.size() > constants::text::max_fallback_fonts) {
                sources.resize(constants::text::max_fallback_fonts);
                ranges.resize(constants::text::max_fallback_fonts);
            }
            Match match;
            match.ranges = std::move(ranges);
            for (std::uint32_t i = 0; i < match.ranges.size(); ++i) {
                if (in_unicode_range(match.ranges[i], U' ')) {
                    match.primary = i;
                    break;
                }
            }
            match.listed = listed;
            match.sources = std::move(sources);
            match.ids.resize(match.sources.size());
            (void)id(match, match.primary);
            if (matches.size() >= constants::scene::text_cache_max_entries) {
                matches.clear();
            }
            return matches.emplace(std::move(key), std::move(match)).first->second;
        }

        // The extents of one face's line box at the font's size and line-height: rounded ascent and descent, and
        // the leading split around them.
        SceneStrut face_strut(std::uint32_t id, const SceneFont& f) {
            const auto m = faces[id].font->line_metrics(f.size);
            SceneStrut s;
            s.ascent = std::round(m.ascent);
            s.descent = std::round(m.descent);
            const float leading = f.line_height ? *f.line_height - (s.ascent + s.descent) : std::round(m.line_gap);
            const auto [top, bottom] = split_leading(leading);
            s.above = s.ascent + top;
            s.below = s.descent + bottom;
            return s;
        }

        // Line box of one line of advances `pen` wide that uses the faces `used`: the primary font's strut, plus the
        // fonts actually used when line-height is normal (Blink includes fallback font metrics only then). The inline
        // size is a LayoutUnit rounded up from the summed advances.
        SceneTextMetrics line_box(const std::set<std::uint32_t>& used, std::uint32_t primary, const SceneFont& f,
                                  float pen) {
            float above{};
            float below{};
            for (auto id : used) {
                if (f.line_height && id != primary) {
                    continue;
                }
                const auto s = face_strut(id, f);
                above = std::max(above, s.above);
                below = std::max(below, s.below);
            }
            const float width = std::ceil(pen * layout_units_per_px) / layout_units_per_px;
            return {width, above + below, above};
        }

        std::shared_ptr<const SceneTextRun> shape(std::string_view text, const SceneFont& f,
                                                  const ShapeContext& context = {}) {
            require_owner();
            if (text.size() > constants::max_text_bytes) {
                throw_diagnostic(Diag::SceneRaster, "scene text byte bound");
            }
            auto key = run_key(catalog_id, text, f, context);
            if (auto i = runs.find(key); i != runs.end()) {
                ++hits;
                return i->second;
            }
            ++misses;
            auto run = std::make_shared<SceneTextRun>();
            run->size = f.size;
            float pen{};
            const auto primary = [&] {
                auto& match = matched(f);
                return id(match, match.primary);
            }();
            std::set<std::uint32_t> used{primary};
            // font-variant-numeric's OpenType features.
            std::vector<FontFeature> features;
            constexpr std::array<std::pair<std::uint8_t, std::array<char, 4>>, 8> tags{{
                {LiningNums, {'l', 'n', 'u', 'm'}},
                {OldstyleNums, {'o', 'n', 'u', 'm'}},
                {ProportionalNums, {'p', 'n', 'u', 'm'}},
                {TabularNums, {'t', 'n', 'u', 'm'}},
                {DiagonalFractions, {'f', 'r', 'a', 'c'}},
                {StackedFractions, {'a', 'f', 'r', 'c'}},
                {Ordinal, {'o', 'r', 'd', 'n'}},
                {SlashedZero, {'z', 'e', 'r', 'o'}},
            }};
            for (const auto& [bit, tag] : tags) {
                if (f.numeric & bit) {
                    features.push_back({std::uint32_t(tag[0]) << 24 | std::uint32_t(tag[1]) << 16 |
                                            std::uint32_t(tag[2]) << 8 | std::uint32_t(tag[3]),
                                        1});
                }
            }

            // The text's runs of one bidi level in logical order: one run when its direction is overridden or it is
            // left-to-right throughout; otherwise as the bidirectional algorithm resolves it at the font's base.
            struct LevelRun {
                std::size_t begin{};
                std::size_t end{};
                std::uint8_t level{};
            };

            std::vector<LevelRun> level_runs;
            if (f.bidi_override || (f.base == BidiBase::Ltr && bidi_left_to_right(text))) {
                level_runs.push_back({0, text.size(), std::uint8_t(f.bidi_override && f.base == BidiBase::Rtl)});
            } else {
                const auto bidi = bidi_levels(text, f.base);
                for (std::size_t b = 0; b < text.size(); ++b) {
                    if (level_runs.empty() || level_runs.back().level != bidi.levels[b]) {
                        level_runs.push_back({b, b, bidi.levels[b]});
                    }
                    level_runs.back().end = b + 1;
                }
            }
            std::vector<std::uint8_t> levels;
            for (const auto& r : level_runs) {
                levels.push_back(r.level);
            }
            // Each run in visual order, its words (and their script runs) right to left at an odd level.
            for (const auto index : bidi_visual_order(levels)) {
                const auto& level_run = level_runs[index];
                const bool rtl = level_run.level % 2;
                const auto part = text.substr(level_run.begin, level_run.end - level_run.begin);
                const auto scripts = script_runs(part);

                struct Segment {
                    std::size_t begin{};
                    std::size_t end{};
                    std::uint32_t script{};
                };

                std::vector<Segment> segments;
                std::size_t s = 0;
                for (const auto& [begin, end] : words(part)) {
                    for (auto at = begin; at < end;) {
                        while (s + 1 < scripts.size() && scripts[s].end <= at) {
                            ++s;
                        }
                        const auto stop = scripts.empty() ? end : std::min(end, scripts[s].end);
                        segments.push_back({at, stop, scripts.empty() ? 0 : scripts[s].script});
                        at = stop;
                    }
                }
                if (rtl) {
                    std::reverse(segments.begin(), segments.end());
                }
                for (const auto& segment : segments) {
                    // Common, Inherited and Unknown text: HarfBuzz picks the script, as it does without itemization.
                    const auto script = segment.script;
                    const bool known = script != constants::text::script_common &&
                                       script != constants::text::script_inherited &&
                                       script != constants::text::script_unknown;
                    // The fonts for the segment's script: the list's, the platform's for the script, the rest of
                    // the fallback. Held only while this segment is shaped (matched() may start its cache over).
                    auto& match = matched(f, known ? script : 0);
                    const auto font = [&](std::uint32_t i) -> Font& {
                        return *faces[id(match, i)].font;
                    };
                    const auto origin = [&](std::uint32_t i) {
                        return role(match, i);
                    };
                    const FallbackFonts fonts{match.sources.size(), font, origin, true};
                    // Its context: the few characters around it in the text, the run's context beyond the text.
                    const auto prefix = text.substr(0, level_run.begin + segment.begin);
                    const auto suffix = text.substr(level_run.begin + segment.end);
                    const auto near_before = context_tail(prefix);
                    const auto near_after = context_head(suffix);
                    std::string before(near_before.size() == prefix.size() ? context_tail(context.before) : "");
                    before.append(near_before);
                    std::string after(near_after);
                    if (near_after.size() == suffix.size()) {
                        after.append(context_head(context.after));
                    }
                    // Chrome positions glyphs in HarfBuzz 16.16 units.
                    const auto shaped = shape_with_fallback(
                        fonts, part.substr(segment.begin, segment.end - segment.begin), f.size,
                        ShapePrecision::Fixed16_16, features, match.ranges, ShapeSegment{rtl, known ? script : 0},
                        ShapeContext{context_tail(before), context_head(after)});
                    const auto& glyphs = shaped.shaped.glyphs;
                    for (std::size_t i = 0; i < glyphs.size(); ++i) {
                        const auto& g = glyphs[i];
                        const auto face = id(match, g.font_id);
                        used.insert(face);
                        // Letter-spacing follows each character cluster, including the last one.
                        const bool cluster_end = i + 1 == glyphs.size() || glyphs[i + 1].utf8_cluster != g.utf8_cluster;
                        const float advance = g.advance_x + (cluster_end ? f.letter_spacing : 0);
                        run->glyphs.push_back({face, g.glyph_id, pen + g.offset_x, -g.offset_y, advance,
                                               std::uint32_t(level_run.begin + segment.begin + g.utf8_cluster),
                                               f.italic && !faces[face].italic, rtl});
                        pen += advance;
                    }
                }
            }
            run->metrics = line_box(used, primary, f, pen);
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
        auto key = Impl::run_key(impl_->catalog_id, text, f);
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
            const auto glyph = face.font->outline(g.glyph_id, run->size * oversample,
                                                  g.oblique ? constants::text::synthetic_oblique_skew : 0);
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

    SceneText::SceneText(FontCatalog catalog) : SceneText(std::make_shared<const FontCatalog>(std::move(catalog))) {
    }

    SceneText::SceneText(std::shared_ptr<const FontCatalog> catalog) : impl_(std::make_unique<Impl>()) {
        use(catalog);
    }

    void SceneText::use(const std::shared_ptr<const FontCatalog>& catalog) {
        impl_->require_owner();
        if (!catalog) {
            throw std::invalid_argument("WEFT_SCENE_TEXT: no font catalog");
        }
        if (catalog == impl_->catalog) {
            return;
        }
        // Catalogs no longer held number nothing.
        std::erase_if(impl_->catalog_ids, [&](const auto& e) {
            return e.first != catalog.get() && e.second.first.expired();
        });
        auto& [held, id] = impl_->catalog_ids[catalog.get()];
        if (held.lock() != catalog) {
            held = catalog;
            id = impl_->next_catalog_id++;
        }
        impl_->catalog = catalog;
        impl_->catalog_id = id;
    }

    std::uint64_t SceneText::catalog_id() const {
        impl_->require_owner();
        return impl_->catalog_id;
    }

    SceneText::~SceneText() {
        impl_->owner.require_or_terminate();
    }

    SceneTextMetrics SceneText::measure(std::string_view text, const SceneStyle& style) {
        return shape(text, style)->metrics;
    }

    std::shared_ptr<const SceneTextRun> SceneText::shape(std::string_view text, const SceneStyle& style,
                                                         const ShapeContext& context) {
        impl_->require_owner();
        return impl_->shape(text, scene_font(style), context);
    }

    std::shared_ptr<const SceneTextRun> SceneText::shape(std::string_view text, const SceneFont& font,
                                                         const ShapeContext& context) {
        return impl_->shape(text, font, context);
    }

    SceneTextMetrics SceneText::slice(const SceneTextRun& run, const SceneFont& font, std::uint32_t begin,
                                      std::uint32_t end) {
        impl_->require_owner();
        auto& match = impl_->matched(font);
        const auto primary = impl_->id(match, match.primary);
        std::set<std::uint32_t> used{primary};
        float pen{};
        for (const auto& g : run.glyphs) {
            if (g.utf8_cluster >= begin && g.utf8_cluster < end) {
                pen += g.advance;
                used.insert(g.face);
            }
        }
        return impl_->line_box(used, primary, font, pen);
    }

    bool SceneText::primary_covers(const SceneFont& font, char32_t character) {
        impl_->require_owner();
        auto& match = impl_->matched(font);
        return in_unicode_range(match.ranges[match.primary], character) &&
               face(impl_->id(match, match.primary)).glyph_index(character) != 0;
    }

    std::pair<float, float> SceneText::field_char_widths(const SceneFont& font) {
        impl_->require_owner();
        auto& match = impl_->matched(font);
        auto& primary = face(impl_->id(match, match.primary));
        float x{};
        if (const auto glyph = primary.glyph_index(U'x')) {
            (void)glyph;
            const auto shaped = primary.shape("x", font.size, ShapePrecision::Fixed16_16);
            x = shaped.advance;
        }
        return {x, primary.bounding_width(font.size)};
    }

    SceneStrut SceneText::strut(const SceneFont& font) {
        impl_->require_owner();
        auto& match = impl_->matched(font);
        return impl_->face_strut(impl_->id(match, match.primary), font);
    }

    float SceneText::x_height(const SceneFont& font) {
        impl_->require_owner();
        auto& match = impl_->matched(font);
        return impl_->faces.at(impl_->id(match, match.primary)).font->x_height(font.size);
    }

    SceneEmBox SceneText::em_box(std::string_view text, const SceneStyle& style) {
        impl_->require_owner();
        const auto font = scene_font(style);
        const auto run = impl_->shape(text, font);
        auto& match = impl_->matched(font);
        const auto primary = impl_->id(match, match.primary);
        std::set<std::uint32_t> used;
        for (const auto& g : run->glyphs) {
            used.insert(g.face);
        }
        if (used.empty()) {
            used.insert(primary);
        }
        SceneEmBox out;
        for (const auto id : used) {
            const auto m = impl_->faces.at(id).font->em_metrics(font.size);
            out.ascent = std::max(out.ascent, m.ascent);
            out.descent = std::max(out.descent, m.descent);
        }
        const auto limit = impl_->faces.at(primary).font->line_metrics(font.size);
        out.ascent = std::min(std::ceil(out.ascent), limit.ascent);
        out.descent = std::min(std::ceil(out.descent), limit.descent);
        return out;
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

    std::shared_ptr<const GlyphBitmap> SceneText::glyph(std::uint32_t face, std::uint32_t glyph_id, float pixels,
                                                        float offset_x, float offset_y, float skew) {
        impl_->require_owner();
        auto& m = *impl_;
        const Impl::GlyphKey key{face,
                                 glyph_id,
                                 std::bit_cast<std::uint32_t>(pixels),
                                 std::bit_cast<std::uint32_t>(offset_x),
                                 std::bit_cast<std::uint32_t>(offset_y),
                                 std::bit_cast<std::uint32_t>(skew)};
        if (const auto found = m.glyphs.find(key); found != m.glyphs.end()) {
            ++m.glyph_hits;
            return found->second;
        }
        ++m.glyph_misses;
        auto bitmap = std::make_shared<const GlyphBitmap>(
            m.faces.at(face).font->rasterize(glyph_id, pixels, offset_x, offset_y, skew));
        const auto bytes = sizeof(GlyphBitmap) + bitmap->coverage.size();
        if (m.glyphs.size() >= std::size_t(constants::scene::text_glyph_cache_max_entries) ||
            m.glyph_bytes + bytes > std::size_t(constants::scene::text_glyph_cache_max_bytes)) {
            m.glyphs.clear();
            m.glyph_bytes = 0;
        }
        // A glyph larger than the whole budget is drawn but not kept.
        if (bytes <= std::size_t(constants::scene::text_glyph_cache_max_bytes)) {
            m.glyph_bytes += bytes;
            m.glyphs.emplace(key, bitmap);
        }
        return bitmap;
    }

    SceneText::Stats SceneText::stats() const {
        impl_->require_owner();
        return {impl_->faces.size(),  impl_->files.size(), impl_->hits,       impl_->misses,
                impl_->glyphs.size(), impl_->glyph_bytes,  impl_->glyph_hits, impl_->glyph_misses};
    }
} // namespace weft
