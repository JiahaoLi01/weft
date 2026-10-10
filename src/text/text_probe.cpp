#include "caseelephant/text_probe.hpp"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#include FT_MODULE_H
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

namespace caseelephant {
struct TextProbeFont::Impl {
    std::thread::id owner{std::this_thread::get_id()};
    std::vector<char> bytes;
    FT_Library library{};
    FT_Face face{};
    hb_blob_t* blob{};
    hb_face_t* hb_face{};
    hb_font_t* shape_font{};
    void check(float px) const {
        if (owner != std::this_thread::get_id()) throw std::logic_error("CE_WRONG_THREAD: font owner");
        if (!std::isfinite(px) || px <= 0 || px > 4096) throw std::invalid_argument("CE_FONT_SIZE: expected (0,4096] px");
    }
    explicit Impl(const std::filesystem::path& path) {
        try {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file) throw std::runtime_error("CE_FONT_FILE: cannot open font");
            const auto size = file.tellg();
            if (size <= 0 || size > 32 * 1024 * 1024) throw std::runtime_error("CE_FONT_FILE: invalid font byte size");
            bytes.resize(static_cast<std::size_t>(size));
            file.seekg(0);
            if (!file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) throw std::runtime_error("CE_FONT_FILE: truncated font");
            if (FT_Init_FreeType(&library) || FT_New_Memory_Face(library,
                reinterpret_cast<const FT_Byte*>(bytes.data()), static_cast<FT_Long>(bytes.size()), 0, &face))
                throw std::runtime_error("CE_FONT_FILE: FreeType rejected font");
            if (!face->units_per_EM) throw std::runtime_error("CE_FONT_FILE: non-scalable font");
            blob = hb_blob_create(bytes.data(), static_cast<unsigned>(bytes.size()), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
            hb_face = hb_face_create(blob, 0);
            if (!hb_blob_get_length(blob) || !hb_face_get_glyph_count(hb_face)) throw std::runtime_error("CE_FONT_FILE: HarfBuzz rejected font");
            shape_font=hb_font_create(hb_face);
            if(shape_font==hb_font_get_empty())throw std::bad_alloc();
            hb_ot_font_set_funcs(shape_font);
        } catch (...) { cleanup(); throw; }
    }
    void cleanup() noexcept {
        if(shape_font)hb_font_destroy(shape_font);
        if (hb_face) hb_face_destroy(hb_face);
        if (blob) hb_blob_destroy(blob);
        if (face) FT_Done_Face(face);
        if (library) FT_Done_FreeType(library);
    }
    ~Impl() {
        if (owner != std::this_thread::get_id()) std::terminate();
        cleanup();
    }
};
TextProbeFont::TextProbeFont(const std::filesystem::path& font) : impl_(std::make_unique<Impl>(font)) {}
TextProbeFont::~TextProbeFont() = default;
ProbeFontMetrics TextProbeFont::metrics(float pixels) const {
    impl_->check(pixels);
    const auto scale = pixels / impl_->face->units_per_EM;
    return {impl_->face->ascender * scale, -impl_->face->descender * scale,
        (impl_->face->height - impl_->face->ascender + impl_->face->descender) * scale};
}
ProbeFontMetrics TextProbeFont::inline_metrics(float pixels) const {
    impl_->check(pixels);auto m=metrics(pixels);const auto scale=pixels/impl_->face->units_per_EM;
    const auto* os2=static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(impl_->face,ft_sfnt_os2));
    if(os2 && os2->version!=0xffff){
        if(os2->fsSelection & 128){m={os2->sTypoAscender*scale,-os2->sTypoDescender*scale,os2->sTypoLineGap*scale};}
        else m={os2->usWinAscent*scale,os2->usWinDescent*scale,0};
    }
    m.ascent=std::round(m.ascent);m.descent=std::round(m.descent);m.line_gap=std::round(m.line_gap);return m;
}
ProbeShapedText TextProbeFont::shape(std::string_view utf8, float pixels) const {
    impl_->check(pixels);
    if (utf8.size() > 1024 * 1024) throw std::invalid_argument("CE_TEXT_LIMIT: probe input exceeds 1 MiB");
    const auto free_buffer = [](hb_buffer_t* p) { hb_buffer_destroy(p); };
    std::unique_ptr<hb_buffer_t, decltype(free_buffer)> buffer(hb_buffer_create(), free_buffer);
    if (buffer.get() == hb_buffer_get_empty()) throw std::bad_alloc();
    // The immutable face and OpenType accelerators live for the owner font;
    // changing scale does not rebuild them for every fallback cluster.
    auto* font=impl_->shape_font;
    const int scale = static_cast<int>(std::round(pixels * 64));
    hb_font_set_scale(font, scale, scale);
    hb_buffer_add_utf8(buffer.get(), utf8.data(), static_cast<int>(utf8.size()), 0, static_cast<int>(utf8.size()));
    hb_buffer_guess_segment_properties(buffer.get());
    hb_shape(font, buffer.get(), nullptr, 0);
    if (!hb_buffer_allocation_successful(buffer.get())) throw std::bad_alloc();
    unsigned count{};
    const auto* infos = hb_buffer_get_glyph_infos(buffer.get(), &count);
    const auto* positions = hb_buffer_get_glyph_positions(buffer.get(), nullptr);
    ProbeShapedText result;
    result.glyphs.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        const auto& p = positions[i];
        result.glyphs.push_back({infos[i].codepoint, infos[i].cluster,
            p.x_advance / 64.0f, p.y_advance / 64.0f, p.x_offset / 64.0f, p.y_offset / 64.0f});
        result.advance += p.x_advance / 64.0f;
        result.missing_glyph |= infos[i].codepoint == 0;
    }
    return result;
}
ProbeGlyphBitmap TextProbeFont::rasterize(std::uint32_t glyph_id, float pixels) {
    impl_->check(pixels);
    if (glyph_id >= static_cast<std::uint32_t>(impl_->face->num_glyphs)) throw std::invalid_argument("CE_GLYPH_ID: out of range");
    if (FT_Set_Char_Size(impl_->face, 0, static_cast<FT_F26Dot6>(std::round(pixels * 64)), 72, 72) ||
        FT_Load_Glyph(impl_->face, glyph_id, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) ||
        FT_Render_Glyph(impl_->face->glyph, FT_RENDER_MODE_NORMAL)) throw std::runtime_error("CE_GLYPH_RASTER: FreeType failure");
    const auto& bitmap = impl_->face->glyph->bitmap;
    if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY || bitmap.num_grays != 256) throw std::runtime_error("CE_UNSUPPORTED_FONT: grayscale coverage required");
    ProbeGlyphBitmap result{bitmap.width, bitmap.rows, impl_->face->glyph->bitmap_left,
        impl_->face->glyph->bitmap_top, {}};
    result.coverage.resize(static_cast<std::size_t>(bitmap.width) * bitmap.rows);
    for (unsigned row = 0; row < bitmap.rows; ++row) {
        const auto offset = static_cast<std::ptrdiff_t>(row) * bitmap.pitch;
        const auto* source = bitmap.buffer + offset;
        std::copy_n(source, bitmap.width, result.coverage.data() + static_cast<std::size_t>(row) * bitmap.width);
    }
    return result;
}
ProbeGlyphBitmap TextProbeFont::rasterize(const ProbeGlyph& glyph,float pixels) {
    impl_->check(pixels);
    if(glyph.font_id!=0)throw std::invalid_argument("CE_FONT_ID: glyph does not belong to single-font resource");
    return rasterize(glyph.glyph_id,pixels);
}

struct TextProbeFontSet::Impl {
    std::thread::id owner{std::this_thread::get_id()};
    std::vector<std::unique_ptr<TextProbeFont>> fonts;
    std::map<std::pair<float,std::string>,ProbeFallbackText> shape_cache;
    std::size_t shape_bytes{},shape_hits{},shape_misses{};
    void check()const {if(owner!=std::this_thread::get_id())throw std::logic_error("CE_WRONG_THREAD: font set owner");}
};
TextProbeFontSet::TextProbeFontSet(const std::vector<std::filesystem::path>& ordered):impl_(std::make_unique<Impl>()) {
    if(ordered.empty() || ordered.size()>8)throw std::invalid_argument("CE_FONT_SET: expected 1..8 ordered fonts");
    std::uintmax_t bytes{};
    for(const auto& path:ordered) {
        bytes+=std::filesystem::file_size(path);
        if(bytes>64*1024*1024)throw std::invalid_argument("CE_FONT_SET: 64 MiB input byte bound");
        impl_->fonts.push_back(std::make_unique<TextProbeFont>(path));
    }
}
TextProbeFontSet::~TextProbeFontSet(){if(impl_->owner!=std::this_thread::get_id())std::terminate();}
TextProbeFont& TextProbeFontSet::primary(){impl_->check();return *impl_->fonts.front();}
ProbeFontMetrics TextProbeFontSet::metrics(std::uint32_t id,float pixels)const {
    impl_->check();if(id>=impl_->fonts.size())throw std::invalid_argument("CE_FONT_ID: font set index");return impl_->fonts[id]->metrics(pixels);
}
ProbeGlyphBitmap TextProbeFontSet::rasterize(const ProbeGlyph& glyph,float pixels) {
    impl_->check();if(glyph.font_id>=impl_->fonts.size())throw std::invalid_argument("CE_FONT_ID: glyph font set index");
    return impl_->fonts[glyph.font_id]->rasterize(glyph.glyph_id,pixels);
}
ProbeFontMetrics TextProbeFontSet::inline_metrics(std::uint32_t id,float pixels)const {
    impl_->check();if(id>=impl_->fonts.size())throw std::invalid_argument("CE_FONT_ID: inline metrics index");return impl_->fonts[id]->inline_metrics(pixels);
}
ProbeShapeCacheStats TextProbeFontSet::shape_cache_stats()const {
    impl_->check();return {impl_->shape_cache.size(),impl_->shape_bytes,impl_->shape_hits,impl_->shape_misses};
}
ProbeFallbackText TextProbeFontSet::shape(std::string_view text,float pixels)const {
    impl_->check();(void)impl_->fonts.front()->metrics(pixels);
    if(text.size()>65536)throw std::invalid_argument("CE_FONT_SET: 64 KiB text bound");
    const auto key=std::pair{pixels,std::string(text)};
    if(const auto i=impl_->shape_cache.find(key);i!=impl_->shape_cache.end()){++impl_->shape_hits;return i->second;}
    ++impl_->shape_misses;
    const auto free_buffer=[](hb_buffer_t* p){hb_buffer_destroy(p);};
    std::unique_ptr<hb_buffer_t,decltype(free_buffer)> unicode(hb_buffer_create(),free_buffer);
    if(unicode.get()==hb_buffer_get_empty())throw std::bad_alloc();
    // HarfBuzz decodes UTF-8; an out-of-Unicode replacement sentinel lets us
    // reject malformed input without reimplementing the decoder/Unicode data.
    hb_buffer_set_replacement_codepoint(unicode.get(),0x110000);
    hb_buffer_add_utf8(unicode.get(),text.data(),int(text.size()),0,int(text.size()));
    if(!hb_buffer_allocation_successful(unicode.get()))throw std::bad_alloc();
    unsigned count{};const auto* points=hb_buffer_get_glyph_infos(unicode.get(),&count);
    for(unsigned i=0;i<count;++i) {
        const auto cp=points[i].codepoint;
        if(cp>0x10ffff)throw std::invalid_argument("CE_TEXT_UTF8: malformed input");
        const auto script=hb_unicode_script(hb_unicode_funcs_get_default(),cp);
        if((cp>=0x202a && cp<=0x202e) || (cp>=0x2066 && cp<=0x2069) || cp==0x200e || cp==0x200f || cp==0x061c ||
            (script!=HB_SCRIPT_LATIN && script!=HB_SCRIPT_HAN && script!=HB_SCRIPT_COMMON && script!=HB_SCRIPT_INHERITED))
            throw std::invalid_argument("CE_TEXT_SCRIPT: LTR Latin/Han profile; paragraph bidi/script itemization required");
    }
    auto primary=impl_->fonts.front()->shape(text,pixels);ProbeFallbackText result;
    if(primary.glyphs.empty())return result;
    std::map<std::uint32_t,bool> clusters;
    for(const auto& g:primary.glyphs) {
        if(g.utf8_cluster>=text.size())throw std::logic_error("CE_TEXT_CLUSTER: invalid HarfBuzz output");
        clusters[g.utf8_cluster]=clusters[g.utf8_cluster] || g.glyph_id==0;
    }
    struct Span {std::uint32_t start,end,font;};std::vector<Span> spans;
    for(auto i=clusters.begin();i!=clusters.end();++i) {
        const auto next=std::next(i);const auto end=next==clusters.end()?std::uint32_t(text.size()):next->first;
        std::uint32_t selected{};
        if(i->second)for(std::uint32_t f=1;f<impl_->fonts.size();++f) {
            if(!impl_->fonts[f]->shape(text.substr(i->first,end-i->first),pixels).missing_glyph){selected=f;break;}
        }
        if(!spans.empty() && spans.back().font==selected && spans.back().end==i->first)spans.back().end=end;
        else spans.push_back({i->first,end,selected});
    }
    for(const auto& span:spans) {
        auto shaped=impl_->fonts[span.font]->shape(text.substr(span.start,span.end-span.start),pixels);
        result.shaped.missing_glyph|=shaped.missing_glyph;result.shaped.advance+=shaped.advance;
        for(auto g:shaped.glyphs){g.font_id=span.font;g.utf8_cluster+=span.start;result.shaped.glyphs.push_back(g);}
        result.runs.push_back({span.font,span.start,span.end-span.start,impl_->fonts[span.font]->metrics(pixels)});
    }
    const auto bytes=text.size()+result.shaped.glyphs.size()*sizeof(ProbeGlyph)+result.runs.size()*sizeof(ProbeFontRun);
    if(bytes<=4*1024*1024){
        if(impl_->shape_cache.size()>=1024 || bytes>4*1024*1024-impl_->shape_bytes){impl_->shape_cache.clear();impl_->shape_bytes=0;}
        impl_->shape_cache.emplace(key,result);impl_->shape_bytes+=bytes;
    }
    return result;
}
ProbeSdfBitmap TextProbeFont::rasterize_sdf(std::uint32_t glyph_id, std::uint32_t base_ppem,
    std::uint32_t spread, ProbeSdfSource source) {
    impl_->check(static_cast<float>(base_ppem));
    if (base_ppem < 16 || base_ppem > 128 || spread < 2 || spread > 32)
        throw std::invalid_argument("CE_SDF_RANGE: base [16,128] ppem, spread [2,32]");
    if (source != ProbeSdfSource::Bitmap && source != ProbeSdfSource::Outline)
        throw std::invalid_argument("CE_SDF_SOURCE: unsupported generator");
    if (glyph_id >= static_cast<std::uint32_t>(impl_->face->num_glyphs)) throw std::invalid_argument("CE_GLYPH_ID: out of range");
    // These properties belong to this font's owner-only FT_Library. Explicitly
    // reset orientation/range; ambient FREETYPE_PROPERTIES cannot flip sign.
    FT_Int range = static_cast<FT_Int>(spread);
    // FreeType's property ABI reads FT_Int, including the boolean switches.
    FT_Int no = 0;
    for (const auto* module : {"sdf", "bsdf"}) {
        if (FT_Property_Set(impl_->library,module,"spread",&range) ||
            FT_Property_Set(impl_->library,module,"flip_sign",&no) ||
            FT_Property_Set(impl_->library,module,"flip_y",&no))
            throw std::runtime_error("CE_SDF_GENERATOR: required FreeType properties unavailable");
    }
    FT_Bool overlaps = 0;
    if (FT_Property_Set(impl_->library,"sdf","overlaps",&overlaps))
        throw std::runtime_error("CE_SDF_GENERATOR: outline overlap policy unavailable");
    if (FT_Set_Pixel_Sizes(impl_->face,0,base_ppem) ||
        FT_Load_Glyph(impl_->face,glyph_id,FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP))
        throw std::runtime_error("CE_SDF_GENERATOR: glyph load failed");
    if (impl_->face->glyph->format != FT_GLYPH_FORMAT_OUTLINE)
        throw std::runtime_error("CE_UNSUPPORTED_FONT: SDF requires scalable outline");
    ProbeSdfBitmap result{0,0,0,0,base_ppem,spread,source,{}};
    if (!impl_->face->glyph->outline.n_points) return result; // Space stays empty.
    // BSDF avoids outline renderer limitations on intersecting contours. It is
    // deliberately recorded in metadata; no silent generator fallback.
    if ((source == ProbeSdfSource::Bitmap && FT_Render_Glyph(impl_->face->glyph,FT_RENDER_MODE_NORMAL)) ||
        FT_Render_Glyph(impl_->face->glyph,FT_RENDER_MODE_SDF))
        throw std::runtime_error("CE_SDF_GENERATOR: FreeType SDF failed");
    const auto& bitmap = impl_->face->glyph->bitmap;
    // The pinned FreeType SDF renderer sets num_grays=255 (the coverage
    // renderer sets 256), while its signed encoding still spans bytes 0..255.
    if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY || bitmap.num_grays != 255)
        throw std::runtime_error("CE_SDF_GENERATOR: expected R8 distance data");
    if (bitmap.width > 2048 || bitmap.rows > 2048)
        throw std::runtime_error("CE_SDF_LIMIT: generated bitmap bounds");
    result.width=bitmap.width; result.height=bitmap.rows;
    result.left=impl_->face->glyph->bitmap_left; result.top=impl_->face->glyph->bitmap_top;
    result.distances.resize(static_cast<std::size_t>(bitmap.width)*bitmap.rows);
    for (unsigned row=0;row<bitmap.rows;++row) {
        const auto* data=bitmap.buffer+static_cast<std::ptrdiff_t>(row)*bitmap.pitch;
        std::copy_n(data,bitmap.width,result.distances.data()+static_cast<std::size_t>(row)*bitmap.width);
    }
    return result;
}
}
