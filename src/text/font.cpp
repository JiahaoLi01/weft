#include "text/font.hpp"
#include "base/owner_thread.hpp"
#include "text/color_canvas.hpp"
#include "text/unicode_properties.hpp"
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
    FontFile::FontFile(const std::filesystem::path& path) : mapped_(MappedFile::open(path)) {
        if (mapped_) {
            bytes_ = mapped_->bytes();
            if (bytes_.size() > static_cast<std::size_t>(constants::text::max_font_file_bytes)) {
                throw std::runtime_error("WEFT_FONT_FILE: invalid font byte size");
            }
            return;
        }
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            throw std::runtime_error("WEFT_FONT_FILE: cannot open font");
        }
        const auto size = file.tellg();
        if (size <= 0 || size > constants::text::max_font_file_bytes) {
            throw std::runtime_error("WEFT_FONT_FILE: invalid font byte size");
        }
        read_.resize(static_cast<std::size_t>(size));
        file.seekg(0);
        if (!file.read(read_.data(), static_cast<std::streamsize>(read_.size()))) {
            throw std::runtime_error("WEFT_FONT_FILE: truncated font");
        }
        bytes_ = read_;
    }

    FontFile::~FontFile() {
        for (const auto& [index, face] : faces_) {
            hb_face_destroy(face);
        }
        if (blob_) {
            hb_blob_destroy(blob_);
        }
    }

    FontFile::FontFile(std::shared_ptr<const std::vector<char>> data) : held_(std::move(data)) {
        if (!held_ || held_->empty() ||
            held_->size() > static_cast<std::size_t>(constants::text::max_font_file_bytes)) {
            throw std::runtime_error("WEFT_FONT_FILE: invalid font byte size");
        }
        bytes_ = std::span<const char>(*held_);
    }

    std::shared_ptr<const FontFile> FontFile::of(const FontFaceSource& source) {
        return source.data ? std::make_shared<const FontFile>(source.data) : open(source.path);
    }

    std::shared_ptr<const FontFile> FontFile::open(const std::filesystem::path& path) {
        // A file read before, while it lives and the file on disk has the size and time it had.
        struct Kept {
            std::weak_ptr<const FontFile> file;
            std::uintmax_t size{};
            std::filesystem::file_time_type time;
        };

        static std::mutex lock;
        static std::map<std::filesystem::path, Kept> files;
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        const auto time = error ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(path, error);
        const auto current = [&](const Kept& kept) {
            return !error && kept.size == size && kept.time == time;
        };
        {
            const std::lock_guard guard(lock);
            const auto& kept = files[path];
            if (auto file = kept.file.lock(); file && current(kept)) {
                return file;
            }
        }
        // Opened without the lock, so that other threads open other files meanwhile; the first copy opened is kept.
        auto file = std::make_shared<const FontFile>(path);
        const std::lock_guard guard(lock);
        auto& kept = files[path];
        if (auto other = kept.file.lock(); other && current(kept)) {
            return other;
        }
        kept = {file, size, time};
        return file;
    }

    hb_face_t* FontFile::harfbuzz_face(std::uint32_t face_index) const {
        const std::lock_guard guard(faces_lock_);
        auto& face = faces_[face_index];
        if (!face) {
            if (!blob_) {
                blob_ = hb_blob_create(bytes_.data(), static_cast<unsigned>(bytes_.size()), HB_MEMORY_MODE_READONLY,
                                       nullptr, nullptr);
            }
            face = hb_face_create(blob_, face_index);
            // Its color tables, read now.
            hb_ot_color_has_paint(face);
            hb_ot_color_has_layers(face);
            hb_ot_color_has_png(face);
            hb_ot_color_has_svg(face);
            hb_face_make_immutable(face);
        }
        return hb_face_reference(face);
    }

    struct Font::Impl {
        OwnerThread owner;
        std::shared_ptr<const FontFile> file;
        FT_Library library{};
        FT_Face face{};
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
                hb_face = file->harfbuzz_face(face_index);
                if (!hb_face_get_glyph_count(hb_face)) {
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

    Font::Font(const std::filesystem::path& font) : Font(FontFile::open(font), 0) {
    }

    Font::Font(const FontFaceSource& source) : Font(FontFile::of(source), source.face_index, source.variations) {
    }

    Font::Font(std::shared_ptr<const FontFile> file, std::uint32_t face_index,
               const std::vector<FontVariation>& variations)
        : impl_(std::make_unique<Impl>(std::move(file), face_index, variations)) {
    }

    Font::~Font() = default;

    std::pair<float, float> split_leading(float leading) {
        const auto units = std::lround(leading * constants::text::layout_units_per_px);
        const float above = std::floor(units / constants::text::layout_units_per_px / 2);
        return {above, units / constants::text::layout_units_per_px - above};
    }

    FontMetrics Font::metrics(float pixels) const {
        impl_->check(pixels);
        const auto scale = pixels / impl_->face->units_per_EM;
        return {impl_->face->ascender * scale, -impl_->face->descender * scale,
                (impl_->face->height - impl_->face->ascender + impl_->face->descender) * scale};
    }

    float Font::bounding_width(float pixels) const {
        impl_->check(pixels);
        const auto* face = impl_->face;
        return float(face->bbox.xMax - face->bbox.xMin) * pixels / float(face->units_per_EM);
    }

    float Font::x_height(float pixels) const {
        impl_->check(pixels);
        auto* face = impl_->face;
        const auto scale = pixels / face->units_per_EM;
        const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(face, ft_sfnt_os2));
        if (os2 && os2->version != constants::text::os2_absent_version &&
            os2->version >= constants::text::os2_x_height_version && os2->sxHeight > 0) {
            return float(os2->sxHeight) * scale;
        }
        if (const auto glyph = FT_Get_Char_Index(face, 'x');
            glyph && FT_Load_Glyph(face, glyph, FT_LOAD_NO_SCALE) == FT_Err_Ok) {
            return float(face->glyph->metrics.horiBearingY) * scale;
        }
        return line_metrics(pixels).ascent * constants::text::guessed_x_height_share;
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

    FontMetrics Font::em_metrics(float pixels) const {
        impl_->check(pixels);
        float ascent{};
        float descent{};
        const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(impl_->face, ft_sfnt_os2));
        if (os2 && os2->version != constants::text::os2_absent_version && os2->sTypoAscender > 0 &&
            os2->sTypoDescender <= 0) {
            ascent = float(os2->sTypoAscender);
            descent = -float(os2->sTypoDescender);
        } else {
            const auto m = line_metrics(pixels);
            ascent = m.ascent;
            descent = m.descent;
        }
        if (ascent + descent <= 0) {
            return {pixels, 0, 0};
        }
        const float above = std::round(pixels * ascent / (ascent + descent) * constants::text::layout_units_per_px) /
                            constants::text::layout_units_per_px;
        return {above, pixels - above, 0};
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

    ShapedText Font::shape(std::string_view utf8, float pixels, ShapePrecision precision,
                           std::span<const FontFeature> features) const {
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
        std::vector<hb_feature_t> settings;
        settings.reserve(features.size());
        for (const auto& f : features) {
            settings.push_back({f.tag, f.value, HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END});
        }
        hb_shape(font, buffer.get(), settings.data(), unsigned(settings.size()));
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

    GlyphBitmap Font::rasterize(std::uint32_t glyph_id, float pixels, float offset_x, float offset_y, float skew) {
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
            if (skew != 0) {
                // FreeType's outlines are y up: x += skew y.
                FT_Matrix slant{constants::text::fixed_16_16_one,
                                static_cast<FT_Fixed>(std::lround(skew * constants::text::fixed_16_16_one)), 0,
                                constants::text::fixed_16_16_one};
                FT_Outline_Transform(&impl_->face->glyph->outline, &slant);
            }
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

    namespace {
        // A FreeType point in pixels, y down.
        std::array<float, 2> glyph_point(const FT_Vector* v) {
            return {float(v->x) / constants::text::fixed_26_6_units_per_pixel,
                    -float(v->y) / constants::text::fixed_26_6_units_per_pixel};
        }

        struct OutlineSink {
            GlyphOutline& out;
            bool open{};

            void add(GlyphOutline::Verb verb, std::initializer_list<const FT_Vector*> points) {
                out.verbs.push_back(verb);
                for (const auto* p : points) {
                    out.points.push_back(glyph_point(p));
                }
            }
        };
    } // namespace

    GlyphOutline Font::outline(std::uint32_t glyph_id, float pixels, float skew) {
        impl_->check(pixels);
        if (glyph_id >= static_cast<std::uint32_t>(impl_->face->num_glyphs)) {
            throw std::invalid_argument("WEFT_GLYPH_ID: out of range");
        }
        if (FT_Set_Char_Size(impl_->face, 0,
                             static_cast<FT_F26Dot6>(std::round(pixels * constants::text::fixed_26_6_units_per_pixel)),
                             constants::text::freetype_pixel_dpi, constants::text::freetype_pixel_dpi) ||
            FT_Load_Glyph(impl_->face, glyph_id, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP)) {
            throw std::runtime_error("WEFT_GLYPH_OUTLINE: FreeType failure");
        }
        GlyphOutline out;
        auto* glyph = impl_->face->glyph;
        if (glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
            return out;
        }
        out.evenodd = (glyph->outline.flags & FT_OUTLINE_EVEN_ODD_FILL) != 0;
        if (skew != 0) {
            FT_Matrix slant{constants::text::fixed_16_16_one,
                            static_cast<FT_Fixed>(std::lround(skew * constants::text::fixed_16_16_one)), 0,
                            constants::text::fixed_16_16_one};
            FT_Outline_Transform(&glyph->outline, &slant);
        }
        OutlineSink sink{out};
        FT_Outline_Funcs funcs{};
        funcs.move_to = [](const FT_Vector* to, void* user) {
            auto& s = *static_cast<OutlineSink*>(user);
            if (s.open) {
                s.out.verbs.push_back(GlyphOutline::Verb::Close);
            }
            s.add(GlyphOutline::Verb::Move, {to});
            s.open = true;
            return 0;
        };
        funcs.line_to = [](const FT_Vector* to, void* user) {
            static_cast<OutlineSink*>(user)->add(GlyphOutline::Verb::Line, {to});
            return 0;
        };
        funcs.conic_to = [](const FT_Vector* control, const FT_Vector* to, void* user) {
            static_cast<OutlineSink*>(user)->add(GlyphOutline::Verb::Quadratic, {control, to});
            return 0;
        };
        funcs.cubic_to = [](const FT_Vector* c1, const FT_Vector* c2, const FT_Vector* to, void* user) {
            static_cast<OutlineSink*>(user)->add(GlyphOutline::Verb::Cubic, {c1, c2, to});
            return 0;
        };
        if (FT_Outline_Decompose(&glyph->outline, &funcs, &sink)) {
            throw std::runtime_error("WEFT_GLYPH_OUTLINE: FreeType failure");
        }
        if (sink.open) {
            out.verbs.push_back(GlyphOutline::Verb::Close);
        }
        return out;
    }

    namespace {
        // HarfBuzz's paint calls on a ColorCanvas, in font units (the font's scale is its em). Image paint (PNG and
        // SVG glyphs) cannot be drawn: it marks the glyph unsupported.
        struct ColorPainter {
            ColorCanvas& canvas;
            bool unsupported{};
        };

        ColorCanvas& canvas_of(void* data) {
            return static_cast<ColorPainter*>(data)->canvas;
        }

        ColorCanvas::Color straight(hb_color_t c) {
            const float max = constants::color_channel_max;
            return {hb_color_get_red(c) / max, hb_color_get_green(c) / max, hb_color_get_blue(c) / max,
                    hb_color_get_alpha(c) / max};
        }

        std::vector<ColorCanvas::Stop> stops_of(hb_color_line_t* line) {
            unsigned count = hb_color_line_get_color_stops(line, 0, nullptr, nullptr);
            std::vector<hb_color_stop_t> raw(count);
            hb_color_line_get_color_stops(line, 0, &count, raw.data());
            std::vector<ColorCanvas::Stop> out;
            out.reserve(count);
            for (unsigned i = 0; i < count; ++i) {
                out.push_back({raw[i].offset, straight(raw[i].color)});
            }
            return out;
        }

        ColorCanvas::Extend extend_of(hb_color_line_t* line) {
            switch (hb_color_line_get_extend(line)) {
                case HB_PAINT_EXTEND_REPEAT:
                    return ColorCanvas::Extend::Repeat;
                case HB_PAINT_EXTEND_REFLECT:
                    return ColorCanvas::Extend::Reflect;
                default:
                    return ColorCanvas::Extend::Pad;
            }
        }

        // A glyph's outline as HarfBuzz draws it, in font units with y up.
        hb_draw_funcs_t* outline_funcs() {
            static hb_draw_funcs_t* const funcs = [] {
                auto* f = hb_draw_funcs_create();
                hb_draw_funcs_set_move_to_func(
                    f,
                    [](hb_draw_funcs_t*, void* data, hb_draw_state_t*, float x, float y, void*) {
                        auto& o = *static_cast<GlyphOutline*>(data);
                        o.verbs.push_back(GlyphOutline::Verb::Move);
                        o.points.push_back({x, y});
                    },
                    nullptr, nullptr);
                hb_draw_funcs_set_line_to_func(
                    f,
                    [](hb_draw_funcs_t*, void* data, hb_draw_state_t*, float x, float y, void*) {
                        auto& o = *static_cast<GlyphOutline*>(data);
                        o.verbs.push_back(GlyphOutline::Verb::Line);
                        o.points.push_back({x, y});
                    },
                    nullptr, nullptr);
                hb_draw_funcs_set_quadratic_to_func(
                    f,
                    [](hb_draw_funcs_t*, void* data, hb_draw_state_t*, float cx, float cy, float x, float y, void*) {
                        auto& o = *static_cast<GlyphOutline*>(data);
                        o.verbs.push_back(GlyphOutline::Verb::Quadratic);
                        o.points.push_back({cx, cy});
                        o.points.push_back({x, y});
                    },
                    nullptr, nullptr);
                hb_draw_funcs_set_cubic_to_func(
                    f,
                    [](hb_draw_funcs_t*, void* data, hb_draw_state_t*, float c1x, float c1y, float c2x, float c2y,
                       float x, float y, void*) {
                        auto& o = *static_cast<GlyphOutline*>(data);
                        o.verbs.push_back(GlyphOutline::Verb::Cubic);
                        o.points.push_back({c1x, c1y});
                        o.points.push_back({c2x, c2y});
                        o.points.push_back({x, y});
                    },
                    nullptr, nullptr);
                hb_draw_funcs_set_close_path_func(
                    f,
                    [](hb_draw_funcs_t*, void* data, hb_draw_state_t*, void*) {
                        static_cast<GlyphOutline*>(data)->verbs.push_back(GlyphOutline::Verb::Close);
                    },
                    nullptr, nullptr);
                hb_draw_funcs_make_immutable(f);
                return f;
            }();
            return funcs;
        }

        hb_paint_funcs_t* paint_funcs() {
            static hb_paint_funcs_t* const funcs = [] {
                auto* f = hb_paint_funcs_create();
                hb_paint_funcs_set_push_transform_func(
                    f,
                    [](hb_paint_funcs_t*, void* data, float xx, float yx, float xy, float yy, float dx, float dy,
                       void*) {
                        canvas_of(data).push_transform({xx, yx, xy, yy, dx, dy});
                    },
                    nullptr, nullptr);
                hb_paint_funcs_set_pop_transform_func(
                    f,
                    [](hb_paint_funcs_t*, void* data, void*) {
                        canvas_of(data).pop_transform();
                    },
                    nullptr, nullptr);
                hb_paint_funcs_set_push_clip_glyph_func(
                    f,
                    [](hb_paint_funcs_t*, void* data, hb_codepoint_t glyph, hb_font_t* font, void*) {
                        GlyphOutline outline;
                        hb_font_draw_glyph(font, glyph, outline_funcs(), &outline);
                        canvas_of(data).push_clip(outline);
                    },
                    nullptr, nullptr);
                hb_paint_funcs_set_push_clip_rectangle_func(
                    f,
                    [](hb_paint_funcs_t*, void* data, float xmin, float ymin, float xmax, float ymax, void*) {
                        canvas_of(data).push_clip(std::array<double, 4>{xmin, ymin, xmax, ymax});
                    },
                    nullptr, nullptr);
                hb_paint_funcs_set_pop_clip_func(
                    f,
                    [](hb_paint_funcs_t*, void* data, void*) {
                        canvas_of(data).pop_clip();
                    },
                    nullptr, nullptr);
                hb_paint_funcs_set_color_func(
                    f,
                    [](hb_paint_funcs_t*, void* data, hb_bool_t, hb_color_t color, void*) {
                        canvas_of(data).fill(straight(color));
                    },
                    nullptr, nullptr);
                hb_paint_funcs_set_image_func(
                    f,
                    [](hb_paint_funcs_t*, void* data, hb_blob_t*, unsigned, unsigned, hb_tag_t, float,
                       hb_glyph_extents_t*, void*) -> hb_bool_t {
                        static_cast<ColorPainter*>(data)->unsupported = true;
                        return false;
                    },
                    nullptr, nullptr);
                hb_paint_funcs_set_linear_gradient_func(
                    f,
                    [](hb_paint_funcs_t*, void* data, hb_color_line_t* line, float x0, float y0, float x1, float y1,
                       float x2, float y2, void*) {
                        canvas_of(data).linear_gradient(stops_of(line), extend_of(line), {x0, y0}, {x1, y1}, {x2, y2});
                    },
                    nullptr, nullptr);
                hb_paint_funcs_set_radial_gradient_func(
                    f,
                    [](hb_paint_funcs_t*, void* data, hb_color_line_t* line, float x0, float y0, float r0, float x1,
                       float y1, float r1, void*) {
                        canvas_of(data).radial_gradient(stops_of(line), extend_of(line), {x0, y0}, r0, {x1, y1}, r1);
                    },
                    nullptr, nullptr);
                hb_paint_funcs_set_sweep_gradient_func(
                    f,
                    [](hb_paint_funcs_t*, void* data, hb_color_line_t* line, float x0, float y0, float start, float end,
                       void*) {
                        canvas_of(data).sweep_gradient(stops_of(line), extend_of(line), {x0, y0}, start, end);
                    },
                    nullptr, nullptr);
                hb_paint_funcs_set_push_group_func(
                    f,
                    [](hb_paint_funcs_t*, void* data, void*) {
                        canvas_of(data).push_group();
                    },
                    nullptr, nullptr);
                hb_paint_funcs_set_pop_group_func(
                    f,
                    [](hb_paint_funcs_t*, void* data, hb_paint_composite_mode_t mode, void*) {
                        canvas_of(data).pop_group(ColorCanvas::Composite(mode));
                    },
                    nullptr, nullptr);
                hb_paint_funcs_make_immutable(f);
                return f;
            }();
            return funcs;
        }
    } // namespace

    std::optional<ColorGlyphBitmap> Font::rasterize_color(std::uint32_t glyph_id, float pixels, float offset_x,
                                                          float offset_y, const std::array<float, 4>& foreground,
                                                          float skew) {
        impl_->check(pixels);
        if (glyph_id >= static_cast<std::uint32_t>(impl_->face->num_glyphs)) {
            throw std::invalid_argument("WEFT_GLYPH_ID: out of range");
        }
        // The em as the font's scale: HarfBuzz paints in font units, and the canvas's base map takes them to pixels.
        auto* font = impl_->shape_font;
        const auto em = static_cast<int>(hb_face_get_upem(impl_->hb_face));
        hb_font_set_scale(font, em, em);
        hb_glyph_extents_t extents{};
        ColorGlyphBitmap out;
        if (!hb_font_get_glyph_extents(font, glyph_id, &extents) || !extents.width || !extents.height) {
            return out;
        }
        const double scale = double(pixels) / em;
        const double margin = constants::text::color_glyph_margin;
        // A slant moves the top and bottom of the extents right by skew times their heights.
        const double top_shift = skew * std::max<double>(extents.y_bearing, extents.y_bearing + extents.height);
        const double bottom_shift = skew * std::min<double>(extents.y_bearing, extents.y_bearing + extents.height);
        const double left =
            std::floor(offset_x + (extents.x_bearing + std::min(top_shift, bottom_shift)) * scale) - margin;
        const double right =
            std::ceil(offset_x + (extents.x_bearing + extents.width + std::max(top_shift, bottom_shift)) * scale) +
            margin;
        const double top = std::floor(offset_y - extents.y_bearing * scale) - margin;
        const double bottom = std::ceil(offset_y - (extents.y_bearing + extents.height) * scale) + margin;
        const double area = (right - left) * (bottom - top);
        if (!(area > 0) || area > constants::text::max_color_glyph_pixels) {
            return std::nullopt;
        }
        const auto width = static_cast<std::uint32_t>(right - left);
        const auto height = static_cast<std::uint32_t>(bottom - top);
        ColorCanvas canvas(width, height, {scale, 0, skew * scale, -scale, offset_x - left, offset_y - top});
        ColorPainter painter{canvas};
        const auto channel = [](float v) {
            return static_cast<std::uint8_t>(
                std::lround(std::clamp(v, 0.f, 1.f) * static_cast<float>(constants::color_channel_max)));
        };
        hb_font_paint_glyph(
            font, glyph_id, paint_funcs(), &painter, 0,
            HB_COLOR(channel(foreground[2]), channel(foreground[1]), channel(foreground[0]), channel(foreground[3])));
        if (painter.unsupported || canvas.overflowed()) {
            return std::nullopt;
        }
        out.width = width;
        out.height = height;
        out.left = static_cast<int>(left);
        out.top = -static_cast<int>(top);
        out.rgba = canvas.pixels();
        return out;
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

    namespace {
        // A cluster's emoji presentation: VS16 shows it as emoji and VS15 as text; otherwise its first character's
        // default.
        EmojiPresentation cluster_presentation(std::string_view utf8) {
            char32_t first{};
            bool started{};
            for (std::size_t i = 0; i < utf8.size();) {
                const auto lead = static_cast<unsigned char>(utf8[i]);
                const std::size_t length = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
                char32_t c = length == 1 ? lead : length == 2 ? lead & 0x1f : length == 3 ? lead & 0x0f : lead & 0x07;
                for (std::size_t k = 1; k < length && i + k < utf8.size(); ++k) {
                    c = c << 6 | (static_cast<unsigned char>(utf8[i + k]) & 0x3f);
                }
                i += length;
                if (c == constants::text::emoji_variation_selector) {
                    return EmojiPresentation::Emoji;
                }
                if (c == constants::text::text_variation_selector) {
                    return EmojiPresentation::Text;
                }
                if (!started) {
                    first = c;
                    started = true;
                }
            }
            return started ? emoji_presentation(first) : EmojiPresentation::None;
        }
    } // namespace

    bool in_unicode_range(std::span<const UnicodeRange> ranges, char32_t codepoint) {
        return ranges.empty() || std::any_of(ranges.begin(), ranges.end(), [&](const UnicodeRange& r) {
                   return codepoint >= r[0] && codepoint <= r[1];
               });
    }

    FallbackText shape_with_fallback(const std::vector<Font*>& fonts, std::string_view text, float pixels,
                                     ShapePrecision precision, std::span<const FontFeature> features,
                                     std::span<const FontRole> roles,
                                     std::span<const std::vector<UnicodeRange>> ranges) {
        if (!roles.empty() && roles.size() != fonts.size()) {
            throw std::invalid_argument("WEFT_FONT_SET: one role per font");
        }
        const auto font = [&](std::uint32_t i) -> Font& {
            return *fonts[i];
        };
        const auto role = [&](std::uint32_t i) {
            return roles[i];
        };
        return shape_with_fallback(FallbackFonts{fonts.size(), font, role, !roles.empty()}, text, pixels, precision,
                                   features, ranges);
    }

    FallbackText shape_with_fallback(const FallbackFonts& fonts, std::string_view text, float pixels,
                                     ShapePrecision precision, std::span<const FontFeature> features,
                                     std::span<const std::vector<UnicodeRange>> ranges) {
        if (!ranges.empty() && ranges.size() != fonts.count) {
            throw std::invalid_argument("WEFT_FONT_SET: one unicode-range per font");
        }
        if (fonts.count == 0 || fonts.count > constants::text::max_fallback_fonts) {
            throw std::invalid_argument("WEFT_FONT_SET: expected 1..8 ordered fonts");
        }
        (void)fonts.font(0).metrics(pixels);
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
            // Scripts whose text shapes one character to a glyph or by the default shaper's features alone, so that a
            // run of several of them needs no itemization: Japanese kana and Korean Hangul among Han, Greek and
            // Cyrillic among Latin.
            const bool simple = script == HB_SCRIPT_LATIN || script == HB_SCRIPT_HAN || script == HB_SCRIPT_COMMON ||
                                script == HB_SCRIPT_INHERITED || script == HB_SCRIPT_HIRAGANA ||
                                script == HB_SCRIPT_KATAKANA || script == HB_SCRIPT_HANGUL ||
                                script == HB_SCRIPT_BOPOMOFO || script == HB_SCRIPT_GREEK ||
                                script == HB_SCRIPT_CYRILLIC;
            if ((cp >= constants::text::bidi_embedding_first && cp <= constants::text::bidi_embedding_last) ||
                (cp >= constants::text::bidi_isolate_first && cp <= constants::text::bidi_isolate_last) ||
                cp == constants::text::left_to_right_mark || cp == constants::text::right_to_left_mark ||
                cp == constants::text::arabic_letter_mark || !simple) {
                throw std::invalid_argument("WEFT_TEXT_SCRIPT: LTR Latin, Greek, Cyrillic, Han, kana, Hangul and "
                                            "Bopomofo profile; paragraph bidi/script itemization required");
            }
        }
        // Whether every code point of the cluster [begin, end) lies in font f's unicode-range.
        const auto in_range = [&](std::uint32_t f, std::uint32_t begin, std::uint32_t end) {
            if (ranges.empty() || ranges[f].empty()) {
                return true;
            }
            for (unsigned i = 0; i < count; ++i) {
                if (points[i].cluster >= begin && points[i].cluster < end &&
                    !in_unicode_range(ranges[f], char32_t(points[i].codepoint))) {
                    return false;
                }
            }
            return true;
        };
        auto primary = fonts.font(0).shape(text, pixels, precision, features);
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
        // A cluster outside the first font's unicode-range is the next font's, as one it has no glyph for.
        if (!ranges.empty() && !ranges.front().empty()) {
            for (auto i = clusters.begin(); i != clusters.end(); ++i) {
                const auto next = std::next(i);
                const auto end = next == clusters.end() ? std::uint32_t(text.size()) : next->first;
                i->second = i->second || !in_range(0, i->first, end);
            }
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
                const auto cluster = text.substr(i->first, end - i->first);
                const auto covers = [&](std::uint32_t f) {
                    return in_range(f, i->first, end) &&
                           !fonts.font(f).shape(cluster, pixels, precision, features).missing_glyph;
                };
                // The fallback fonts this cluster prefers: any, the monochrome ones or the color ones.
                auto preferred = FontRole::Listed;
                if (fonts.roles) {
                    const auto presentation = cluster_presentation(cluster);
                    preferred = presentation == EmojiPresentation::Text    ? FontRole::FallbackText
                                : presentation == EmojiPresentation::Emoji ? FontRole::FallbackColor
                                                                           : FontRole::Listed;
                }
                bool found{};
                for (std::uint32_t f = 1; !found && f < fonts.count; ++f) {
                    if ((preferred == FontRole::Listed || fonts.role(f) == FontRole::Listed ||
                         fonts.role(f) == preferred) &&
                        covers(f)) {
                        selected = f;
                        found = true;
                    }
                }
                for (std::uint32_t f = 1; !found && f < fonts.count; ++f) {
                    if (covers(f)) {
                        selected = f;
                        found = true;
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
            auto shaped = fonts.font(span.font).shape(text.substr(span.start, span.end - span.start), pixels, precision,
                                                      features);
            result.shaped.missing_glyph |= shaped.missing_glyph;
            result.shaped.advance += shaped.advance;
            for (auto g : shaped.glyphs) {
                g.font_id = span.font;
                g.utf8_cluster += span.start;
                result.shaped.glyphs.push_back(g);
            }
            result.runs.push_back(
                {span.font, span.start, span.end - span.start, fonts.font(span.font).metrics(pixels)});
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
