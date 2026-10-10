#include "text/font.hpp"
#include "base/owner_thread.hpp"
#include "constants/common.hpp"
#include "constants/text.hpp"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#include FT_MODULE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_OUTLINE_H
#include <hb.h>
#include <hb-ot.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>

namespace weft {
    FontFile::FontFile(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            throw std::runtime_error("WEFT_FONT_FILE: cannot open font");
        }
        const auto size = file.tellg();
        if (size <= 0 || size > constants::text::max_font_file_bytes) {
            throw std::runtime_error("WEFT_FONT_FILE: invalid font byte size");
        }
        bytes_.resize(static_cast<std::size_t>(size));
        file.seekg(0);
        if (!file.read(bytes_.data(), static_cast<std::streamsize>(bytes_.size()))) {
            throw std::runtime_error("WEFT_FONT_FILE: truncated font");
        }
    }

    struct Font::Impl {
        OwnerThread owner;
        std::shared_ptr<const FontFile> file;
        FT_Library library{};
        FT_Face face{};
        hb_blob_t* blob{};
        hb_face_t* hb_face{};
        hb_font_t* shape_font{};

        void check(float px) const {
            owner.require(Diag::WrongThread, "font owner");
            if (!std::isfinite(px) || px <= 0 || px > constants::text::max_font_pixels) {
                throw std::invalid_argument("WEFT_FONT_SIZE: expected (0,4096] px");
            }
        }

        Impl(std::shared_ptr<const FontFile> source, std::uint32_t face_index,
             const std::vector<FontVariation>& variations)
            : file(std::move(source)) {
            try {
                if (!file) {
                    throw std::invalid_argument("WEFT_FONT_FILE: missing font file");
                }
                const auto& bytes = file->bytes();
                if (FT_Init_FreeType(&library) ||
                    FT_New_Memory_Face(library, reinterpret_cast<const FT_Byte*>(bytes.data()),
                                       static_cast<FT_Long>(bytes.size()), static_cast<FT_Long>(face_index), &face)) {
                    throw std::runtime_error("WEFT_FONT_FILE: FreeType rejected font face");
                }
                if (!face->units_per_EM) {
                    throw std::runtime_error("WEFT_FONT_FILE: non-scalable font");
                }
                blob = hb_blob_create(bytes.data(), static_cast<unsigned>(bytes.size()), HB_MEMORY_MODE_READONLY,
                                      nullptr, nullptr);
                hb_face = hb_face_create(blob, face_index);
                if (!hb_blob_get_length(blob) || !hb_face_get_glyph_count(hb_face)) {
                    throw std::runtime_error("WEFT_FONT_FILE: HarfBuzz rejected font");
                }
                shape_font = hb_font_create(hb_face);
                if (shape_font == hb_font_get_empty()) {
                    throw std::bad_alloc();
                }
                hb_ot_font_set_funcs(shape_font);
                if (!variations.empty()) {
                    vary(variations);
                }
            } catch (...) {
                cleanup();
                throw;
            }
        }

        // The same axis values drive FreeType outlines and HarfBuzz advances.
        void vary(const std::vector<FontVariation>& variations) {
            FT_MM_Var* axes{};
            if (!FT_HAS_MULTIPLE_MASTERS(face) || FT_Get_MM_Var(face, &axes)) {
                throw std::invalid_argument("WEFT_FONT_VARIATION: face has no variation axes");
            }
            std::vector<FT_Fixed> coordinates(axes->num_axis);
            for (FT_UInt i = 0; i < axes->num_axis; ++i) {
                coordinates[i] = axes->axis[i].def;
            }
            std::vector<hb_variation_t> shaping;
            for (const auto& v : variations) {
                FT_UInt i = 0;
                while (i < axes->num_axis && axes->axis[i].tag != v.tag) {
                    ++i;
                }
                if (i == axes->num_axis || !std::isfinite(v.value)) {
                    FT_Done_MM_Var(library, axes);
                    const char tag[]{char(v.tag >> 24), char(v.tag >> 16), char(v.tag >> 8), char(v.tag), 0};
                    throw std::invalid_argument(std::string("WEFT_FONT_VARIATION: unknown axis or value ") + tag);
                }
                coordinates[i] = static_cast<FT_Fixed>(std::lround(v.value * constants::text::fixed_16_16_one));
                shaping.push_back({v.tag, v.value});
            }
            const auto count = axes->num_axis;
            FT_Done_MM_Var(library, axes);
            if (FT_Set_Var_Design_Coordinates(face, count, coordinates.data())) {
                throw std::runtime_error("WEFT_FONT_VARIATION: FreeType rejected axis values");
            }
            hb_font_set_variations(shape_font, shaping.data(), static_cast<unsigned>(shaping.size()));
        }

        void cleanup() noexcept {
            if (shape_font) {
                hb_font_destroy(shape_font);
            }
            if (hb_face) {
                hb_face_destroy(hb_face);
            }
            if (blob) {
                hb_blob_destroy(blob);
            }
            if (face) {
                FT_Done_Face(face);
            }
            if (library) {
                FT_Done_FreeType(library);
            }
        }

        ~Impl() {
            owner.require_or_terminate();
            cleanup();
        }
    };

    Font::Font(const std::filesystem::path& font) : Font(std::make_shared<const FontFile>(font), 0) {
    }

    Font::Font(const FontFaceSource& source)
        : Font(std::make_shared<const FontFile>(source.path), source.face_index, source.variations) {
    }

    Font::Font(std::shared_ptr<const FontFile> file, std::uint32_t face_index,
               const std::vector<FontVariation>& variations)
        : impl_(std::make_unique<Impl>(std::move(file), face_index, variations)) {
    }

    Font::~Font() = default;

    FontMetrics Font::metrics(float pixels) const {
        impl_->check(pixels);
        const auto scale = pixels / impl_->face->units_per_EM;
        return {impl_->face->ascender * scale, -impl_->face->descender * scale,
                (impl_->face->height - impl_->face->ascender + impl_->face->descender) * scale};
    }

    FontMetrics Font::inline_metrics(float pixels) const {
        impl_->check(pixels);
        auto m = metrics(pixels);
        const auto scale = pixels / impl_->face->units_per_EM;
        const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(impl_->face, ft_sfnt_os2));
        if (os2 && os2->version != constants::text::os2_absent_version) {
            if (os2->fsSelection & constants::text::os2_use_typo_metrics) {
                m = {os2->sTypoAscender * scale, -os2->sTypoDescender * scale, os2->sTypoLineGap * scale};
            } else {
                m = {os2->usWinAscent * scale, os2->usWinDescent * scale, 0};
            }
        }
        m.ascent = std::round(m.ascent);
        m.descent = std::round(m.descent);
        m.line_gap = std::round(m.line_gap);
        return m;
    }

    FontMetrics Font::line_metrics(float pixels) const {
        impl_->check(pixels);
        const auto* face = impl_->face;
        const auto scale = pixels / face->units_per_EM;
        const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(impl_->face, ft_sfnt_os2));
        const auto* hhea = static_cast<const TT_HoriHeader*>(FT_Get_Sfnt_Table(impl_->face, ft_sfnt_hhea));
        if (!os2 || os2->version == constants::text::os2_absent_version || !hhea) {
            return metrics(pixels);
        }
        if (os2->fsSelection & constants::text::os2_use_typo_metrics) {
            return {os2->sTypoAscender * scale, -os2->sTypoDescender * scale, os2->sTypoLineGap * scale};
        }
        const long win = long(os2->usWinAscent) + long(os2->usWinDescent);
        const long spacing = long(hhea->Ascender) - long(hhea->Descender) + long(hhea->Line_Gap);
        return {os2->usWinAscent * scale, os2->usWinDescent * scale, std::max(0L, spacing - win) * scale};
    }

    bool Font::has_color_glyphs() const {
        impl_->owner.require(Diag::WrongThread, "font owner");
        for (const auto tag : {HB_TAG('C', 'O', 'L', 'R'), HB_TAG('C', 'B', 'D', 'T'), HB_TAG('s', 'b', 'i', 'x'),
                               HB_TAG('S', 'V', 'G', ' ')}) {
            hb_blob_t* table = hb_face_reference_table(impl_->hb_face, tag);
            const bool present = hb_blob_get_length(table) > 0;
            hb_blob_destroy(table);
            if (present) {
                return true;
            }
        }
        return false;
    }

    std::uint32_t Font::glyph_index(char32_t codepoint) const {
        impl_->owner.require(Diag::WrongThread, "font owner");
        return FT_Get_Char_Index(impl_->face, static_cast<FT_ULong>(codepoint));
    }

    ShapedText Font::shape(std::string_view utf8, float pixels, ShapePrecision precision) const {
        impl_->check(pixels);
        if (utf8.size() > constants::text::max_shape_input_bytes) {
            throw std::invalid_argument("WEFT_TEXT_LIMIT: probe input exceeds 1 MiB");
        }
        const auto free_buffer = [](hb_buffer_t* p) {
            hb_buffer_destroy(p);
        };
        std::unique_ptr<hb_buffer_t, decltype(free_buffer)> buffer(hb_buffer_create(), free_buffer);
        if (buffer.get() == hb_buffer_get_empty()) {
            throw std::bad_alloc();
        }
        // The immutable face and OpenType accelerators live for the owner font;
        // changing scale does not rebuild them for every fallback cluster.
        auto* font = impl_->shape_font;
        const float units = precision == ShapePrecision::Fixed16_16
                                ? static_cast<float>(constants::text::fixed_16_16_one)
                                : static_cast<float>(constants::text::fixed_26_6_units_per_pixel);
        const int scale = static_cast<int>(std::round(pixels * units));
        hb_font_set_scale(font, scale, scale);
        hb_buffer_add_utf8(buffer.get(), utf8.data(), static_cast<int>(utf8.size()), 0, static_cast<int>(utf8.size()));
        hb_buffer_guess_segment_properties(buffer.get());
        hb_shape(font, buffer.get(), nullptr, 0);
        if (!hb_buffer_allocation_successful(buffer.get())) {
            throw std::bad_alloc();
        }
        unsigned count{};
        const auto* infos = hb_buffer_get_glyph_infos(buffer.get(), &count);
        const auto* positions = hb_buffer_get_glyph_positions(buffer.get(), nullptr);
        ShapedText result;
        result.glyphs.reserve(count);
        for (unsigned i = 0; i < count; ++i) {
            const auto& p = positions[i];
            result.glyphs.push_back({infos[i].codepoint, infos[i].cluster, p.x_advance / units, p.y_advance / units,
                                     p.x_offset / units, p.y_offset / units});
            result.advance += p.x_advance / units;
            result.missing_glyph |= infos[i].codepoint == 0;
        }
        return result;
    }

    GlyphBitmap Font::rasterize(std::uint32_t glyph_id, float pixels) {
        return rasterize(glyph_id, pixels, 0, 0);
    }

    GlyphBitmap Font::rasterize(std::uint32_t glyph_id, float pixels, float offset_x, float offset_y) {
        impl_->check(pixels);
        if (glyph_id >= static_cast<std::uint32_t>(impl_->face->num_glyphs)) {
            throw std::invalid_argument("WEFT_GLYPH_ID: out of range");
        }
        if (!std::isfinite(offset_x) || !std::isfinite(offset_y) || offset_x < 0 || offset_x >= 1 || offset_y < 0 ||
            offset_y >= 1) {
            throw std::invalid_argument("WEFT_GLYPH_OFFSET: expected [0,1) px");
        }
        if (FT_Set_Char_Size(impl_->face, 0,
                             static_cast<FT_F26Dot6>(std::round(pixels * constants::text::fixed_26_6_units_per_pixel)),
                             constants::text::freetype_pixel_dpi, constants::text::freetype_pixel_dpi) ||
            FT_Load_Glyph(impl_->face, glyph_id, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP)) {
            throw std::runtime_error("WEFT_GLYPH_RASTER: FreeType failure");
        }
        if (impl_->face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
            FT_Outline_Translate(
                &impl_->face->glyph->outline,
                static_cast<FT_Pos>(std::lround(offset_x * constants::text::fixed_26_6_units_per_pixel)),
                -static_cast<FT_Pos>(std::lround(offset_y * constants::text::fixed_26_6_units_per_pixel)));
        }
        if (FT_Render_Glyph(impl_->face->glyph, FT_RENDER_MODE_NORMAL)) {
            throw std::runtime_error("WEFT_GLYPH_RASTER: FreeType failure");
        }
        const auto& bitmap = impl_->face->glyph->bitmap;
        if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY || bitmap.num_grays != constants::text::coverage_num_grays) {
            throw std::runtime_error("WEFT_UNSUPPORTED_FONT: grayscale coverage required");
        }
        GlyphBitmap result{
            bitmap.width, bitmap.rows, impl_->face->glyph->bitmap_left, impl_->face->glyph->bitmap_top, {}};
        result.coverage.resize(static_cast<std::size_t>(bitmap.width) * bitmap.rows);
        for (unsigned row = 0; row < bitmap.rows; ++row) {
            const auto offset = static_cast<std::ptrdiff_t>(row) * bitmap.pitch;
            const auto* source = bitmap.buffer + offset;
            std::copy_n(source, bitmap.width, result.coverage.data() + static_cast<std::size_t>(row) * bitmap.width);
        }
        return result;
    }

    GlyphBitmap Font::rasterize(const Glyph& glyph, float pixels) {
        impl_->check(pixels);
        if (glyph.font_id != 0) {
            throw std::invalid_argument("WEFT_FONT_ID: glyph does not belong to single-font resource");
        }
        return rasterize(glyph.glyph_id, pixels);
    }

    struct FontSet::Impl {
        OwnerThread owner;
        std::vector<std::unique_ptr<Font>> fonts;
        std::map<std::pair<float, std::string>, FallbackText> shape_cache;
        std::size_t shape_bytes{};
        std::size_t shape_hits{};
        std::size_t shape_misses{};

        void check() const {
            owner.require(Diag::WrongThread, "font set owner");
        }
    };

    FontSet::FontSet(const std::vector<std::filesystem::path>& ordered) : impl_(std::make_unique<Impl>()) {
        if (ordered.empty() || ordered.size() > constants::text::max_fallback_fonts) {
            throw std::invalid_argument("WEFT_FONT_SET: expected 1..8 ordered fonts");
        }
        std::uintmax_t bytes{};
        for (const auto& path : ordered) {
            bytes += std::filesystem::file_size(path);
            if (bytes > constants::text::max_font_set_bytes) {
                throw std::invalid_argument("WEFT_FONT_SET: 64 MiB input byte bound");
            }
            impl_->fonts.push_back(std::make_unique<Font>(path));
        }
    }

    FontSet::~FontSet() {
        impl_->owner.require_or_terminate();
    }

    Font& FontSet::primary() {
        impl_->check();
        return *impl_->fonts.front();
    }

    FontMetrics FontSet::metrics(std::uint32_t id, float pixels) const {
        impl_->check();
        if (id >= impl_->fonts.size()) {
            throw std::invalid_argument("WEFT_FONT_ID: font set index");
        }
        return impl_->fonts[id]->metrics(pixels);
    }

    GlyphBitmap FontSet::rasterize(const Glyph& glyph, float pixels) {
        impl_->check();
        if (glyph.font_id >= impl_->fonts.size()) {
            throw std::invalid_argument("WEFT_FONT_ID: glyph font set index");
        }
        return impl_->fonts[glyph.font_id]->rasterize(glyph.glyph_id, pixels);
    }

    FontMetrics FontSet::inline_metrics(std::uint32_t id, float pixels) const {
        impl_->check();
        if (id >= impl_->fonts.size()) {
            throw std::invalid_argument("WEFT_FONT_ID: inline metrics index");
        }
        return impl_->fonts[id]->inline_metrics(pixels);
    }

    ShapeCacheStats FontSet::shape_cache_stats() const {
        impl_->check();
        return {impl_->shape_cache.size(), impl_->shape_bytes, impl_->shape_hits, impl_->shape_misses};
    }

    FallbackText FontSet::shape(std::string_view text, float pixels) const {
        impl_->check();
        (void)impl_->fonts.front()->metrics(pixels);
        if (text.size() > constants::max_text_bytes) {
            throw std::invalid_argument("WEFT_FONT_SET: 64 KiB text bound");
        }
        const auto key = std::pair{pixels, std::string(text)};
        if (const auto i = impl_->shape_cache.find(key); i != impl_->shape_cache.end()) {
            ++impl_->shape_hits;
            return i->second;
        }
        ++impl_->shape_misses;
        std::vector<Font*> fonts;
        for (const auto& font : impl_->fonts) {
            fonts.push_back(font.get());
        }
        auto result = shape_with_fallback(fonts, text, pixels);
        if (result.shaped.glyphs.empty()) {
            return result;
        }
        const auto bytes =
            text.size() + result.shaped.glyphs.size() * sizeof(Glyph) + result.runs.size() * sizeof(FontRun);
        if (bytes <= constants::text::shape_cache_max_bytes) {
            if (impl_->shape_cache.size() >= constants::text::shape_cache_max_entries ||
                bytes > constants::text::shape_cache_max_bytes - impl_->shape_bytes) {
                impl_->shape_cache.clear();
                impl_->shape_bytes = 0;
            }
            impl_->shape_cache.emplace(key, result);
            impl_->shape_bytes += bytes;
        }
        return result;
    }

    FallbackText shape_with_fallback(const std::vector<Font*>& fonts, std::string_view text, float pixels,
                                     ShapePrecision precision) {
        if (fonts.empty() || fonts.size() > constants::text::max_fallback_fonts) {
            throw std::invalid_argument("WEFT_FONT_SET: expected 1..8 ordered fonts");
        }
        (void)fonts.front()->metrics(pixels);
        if (text.size() > constants::max_text_bytes) {
            throw std::invalid_argument("WEFT_FONT_SET: 64 KiB text bound");
        }
        const auto free_buffer = [](hb_buffer_t* p) {
            hb_buffer_destroy(p);
        };
        std::unique_ptr<hb_buffer_t, decltype(free_buffer)> unicode(hb_buffer_create(), free_buffer);
        if (unicode.get() == hb_buffer_get_empty()) {
            throw std::bad_alloc();
        }
        // HarfBuzz decodes UTF-8; an out-of-Unicode replacement sentinel lets us
        // reject malformed input without reimplementing the decoder/Unicode data.
        hb_buffer_set_replacement_codepoint(unicode.get(), constants::invalid_codepoint_marker);
        hb_buffer_add_utf8(unicode.get(), text.data(), int(text.size()), 0, int(text.size()));
        if (!hb_buffer_allocation_successful(unicode.get())) {
            throw std::bad_alloc();
        }
        unsigned count{};
        const auto* points = hb_buffer_get_glyph_infos(unicode.get(), &count);
        for (unsigned i = 0; i < count; ++i) {
            const auto cp = points[i].codepoint;
            if (cp > constants::max_unicode_codepoint) {
                throw std::invalid_argument("WEFT_TEXT_UTF8: malformed input");
            }
            const auto script = hb_unicode_script(hb_unicode_funcs_get_default(), cp);
            if ((cp >= constants::text::bidi_embedding_first && cp <= constants::text::bidi_embedding_last) ||
                (cp >= constants::text::bidi_isolate_first && cp <= constants::text::bidi_isolate_last) ||
                cp == constants::text::left_to_right_mark || cp == constants::text::right_to_left_mark ||
                cp == constants::text::arabic_letter_mark ||
                (script != HB_SCRIPT_LATIN && script != HB_SCRIPT_HAN && script != HB_SCRIPT_COMMON &&
                 script != HB_SCRIPT_INHERITED)) {
                throw std::invalid_argument(
                    "WEFT_TEXT_SCRIPT: LTR Latin/Han profile; paragraph bidi/script itemization required");
            }
        }
        auto primary = fonts.front()->shape(text, pixels, precision);
        FallbackText result;
        if (primary.glyphs.empty()) {
            return result;
        }
        std::map<std::uint32_t, bool> clusters;
        for (const auto& g : primary.glyphs) {
            if (g.utf8_cluster >= text.size()) {
                throw std::logic_error("WEFT_TEXT_CLUSTER: invalid HarfBuzz output");
            }
            clusters[g.utf8_cluster] = clusters[g.utf8_cluster] || g.glyph_id == 0;
        }

        struct Span {
            std::uint32_t start;
            std::uint32_t end;
            std::uint32_t font;
        };

        std::vector<Span> spans;
        for (auto i = clusters.begin(); i != clusters.end(); ++i) {
            const auto next = std::next(i);
            const auto end = next == clusters.end() ? std::uint32_t(text.size()) : next->first;
            std::uint32_t selected{};
            if (i->second) {
                for (std::uint32_t f = 1; f < fonts.size(); ++f) {
                    if (!fonts[f]->shape(text.substr(i->first, end - i->first), pixels, precision).missing_glyph) {
                        selected = f;
                        break;
                    }
                }
            }
            if (!spans.empty() && spans.back().font == selected && spans.back().end == i->first) {
                spans.back().end = end;
            } else {
                spans.push_back({i->first, end, selected});
            }
        }
        for (const auto& span : spans) {
            auto shaped = fonts[span.font]->shape(text.substr(span.start, span.end - span.start), pixels, precision);
            result.shaped.missing_glyph |= shaped.missing_glyph;
            result.shaped.advance += shaped.advance;
            for (auto g : shaped.glyphs) {
                g.font_id = span.font;
                g.utf8_cluster += span.start;
                result.shaped.glyphs.push_back(g);
            }
            result.runs.push_back({span.font, span.start, span.end - span.start, fonts[span.font]->metrics(pixels)});
        }
        return result;
    }

    SdfBitmap Font::rasterize_sdf(std::uint32_t glyph_id, std::uint32_t base_ppem, std::uint32_t spread,
                                  SdfSource source) {
        impl_->check(static_cast<float>(base_ppem));
        if (base_ppem < constants::text::sdf_min_base_ppem || base_ppem > constants::text::sdf_max_base_ppem ||
            spread < constants::text::sdf_min_spread || spread > constants::text::sdf_max_spread) {
            throw std::invalid_argument("WEFT_SDF_RANGE: base [16,128] ppem, spread [2,32]");
        }
        if (source != SdfSource::Bitmap && source != SdfSource::Outline) {
            throw std::invalid_argument("WEFT_SDF_SOURCE: unsupported generator");
        }
        if (glyph_id >= static_cast<std::uint32_t>(impl_->face->num_glyphs)) {
            throw std::invalid_argument("WEFT_GLYPH_ID: out of range");
        }
        // These properties belong to this font's owner-only FT_Library. Explicitly
        // reset orientation/range; ambient FREETYPE_PROPERTIES cannot flip sign.
        FT_Int range = static_cast<FT_Int>(spread);
        // FreeType's property ABI reads FT_Int, including the boolean switches.
        FT_Int no = 0;
        for (const auto* module : {"sdf", "bsdf"}) {
            if (FT_Property_Set(impl_->library, module, "spread", &range) ||
                FT_Property_Set(impl_->library, module, "flip_sign", &no) ||
                FT_Property_Set(impl_->library, module, "flip_y", &no)) {
                throw std::runtime_error("WEFT_SDF_GENERATOR: required FreeType properties unavailable");
            }
        }
        FT_Bool overlaps = 0;
        if (FT_Property_Set(impl_->library, "sdf", "overlaps", &overlaps)) {
            throw std::runtime_error("WEFT_SDF_GENERATOR: outline overlap policy unavailable");
        }
        if (FT_Set_Pixel_Sizes(impl_->face, 0, base_ppem) ||
            FT_Load_Glyph(impl_->face, glyph_id, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP)) {
            throw std::runtime_error("WEFT_SDF_GENERATOR: glyph load failed");
        }
        if (impl_->face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
            throw std::runtime_error("WEFT_UNSUPPORTED_FONT: SDF requires scalable outline");
        }
        SdfBitmap result{0, 0, 0, 0, base_ppem, spread, source, {}};
        if (!impl_->face->glyph->outline.n_points) {
            return result; // Space stays empty.
        }
        // BSDF avoids outline renderer limitations on intersecting contours. It is
        // deliberately recorded in metadata; no silent generator fallback.
        if ((source == SdfSource::Bitmap && FT_Render_Glyph(impl_->face->glyph, FT_RENDER_MODE_NORMAL)) ||
            FT_Render_Glyph(impl_->face->glyph, FT_RENDER_MODE_SDF)) {
            throw std::runtime_error("WEFT_SDF_GENERATOR: FreeType SDF failed");
        }
        const auto& bitmap = impl_->face->glyph->bitmap;
        // The pinned FreeType SDF renderer sets num_grays=255 (the coverage
        // renderer sets 256), while its signed encoding still spans bytes 0..255.
        if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY || bitmap.num_grays != constants::text::sdf_num_grays) {
            throw std::runtime_error("WEFT_SDF_GENERATOR: expected R8 distance data");
        }
        if (bitmap.width > constants::text::sdf_max_bitmap_dimension ||
            bitmap.rows > constants::text::sdf_max_bitmap_dimension) {
            throw std::runtime_error("WEFT_SDF_LIMIT: generated bitmap bounds");
        }
        result.width = bitmap.width;
        result.height = bitmap.rows;
        result.left = impl_->face->glyph->bitmap_left;
        result.top = impl_->face->glyph->bitmap_top;
        result.distances.resize(static_cast<std::size_t>(bitmap.width) * bitmap.rows);
        for (unsigned row = 0; row < bitmap.rows; ++row) {
            const auto* data = bitmap.buffer + static_cast<std::ptrdiff_t>(row) * bitmap.pitch;
            std::copy_n(data, bitmap.width, result.distances.data() + static_cast<std::size_t>(row) * bitmap.width);
        }
        return result;
    }
} // namespace weft
