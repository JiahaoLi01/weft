#pragma once

#include "constants/layout.hpp"
#include "layout/output.hpp"
#include "paint/display_list.hpp"
#include "text/font.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>

// A positioned document page as a display list: what the document formatter paints, in paint order.
namespace weft {
    // Glyph coverage images of one font, rasterized once per font, glyph and size. A bound that would be exceeded
    // clears them; binding another font starts over.
    struct GlyphImages {
        using Key = std::tuple<std::uint32_t, std::uint32_t, float>;

        struct Glyph {
            GlyphBitmap bitmap;
            // Null for a glyph without pixels.
            std::shared_ptr<const paint::Asset> asset;
        };

        const void* font_scope{};
        std::map<Key, Glyph> glyphs;
        std::size_t bytes{};
        std::size_t hits{};
        std::size_t misses{};

        // Returns whether font is a different font than the one bound before, which cleared the images.
        bool bind(const void* font) {
            const bool other = font_scope && font_scope != font;
            if (other) {
                glyphs.clear();
                bytes = 0;
            }
            font_scope = font;
            return other;
        }
    };

    // Rectangles become boxes; images and glyphs become images of leased assets, glyphs tinted with their text color.
    // Each item keeps its paint's clip and rounded clips. images is the host allowlist of image sources; glyphs, when
    // given, keeps glyph images across calls under its bounds.
    template <class FontT>
    paint::DisplayList page_display_list(const PositionedPage& page, FontT& font,
                                         const std::map<std::string, std::shared_ptr<const paint::Asset>>& images,
                                         GlyphImages* glyphs = nullptr) {
        paint::DisplayList list;
        GlyphImages local;
        auto& cache = glyphs ? *glyphs : local;
        cache.bind(&font);
        std::map<const paint::Asset*, std::uint32_t> asset_indices;
        auto lease = [&](const std::shared_ptr<const paint::Asset>& asset) {
            const auto [i, inserted] = asset_indices.emplace(asset.get(), std::uint32_t(list.assets.size()));
            if (inserted) {
                list.assets.push_back(asset);
            }
            return i->second;
        };
        // Items of one paint share its clip and its rounded clips.
        paint::Item shared;
        auto place = [&](paint::Item item, float x, float y, float w, float h) {
            item.transform = {1, 0, 0, 1, x, y};
            item.width = w;
            item.height = h;
            item.clip = shared.clip;
            item.clips_begin = shared.clips_begin;
            item.clips_count = shared.clips_count;
            list.items.push_back(item);
        };
        auto image = [&](float x, float y, float w, float h, paint::Image payload) {
            paint::Item item;
            item.kind = paint::Item::Kind::Image;
            item.payload = std::uint32_t(list.images.size());
            list.images.push_back(payload);
            place(item, x, y, w, h);
        };
        for (const auto& item : page.paints) {
            const auto& p = item.paint;
            const auto& c = item.clip;
            shared.clip = {c[0], c[1], c[0] + c[2], c[1] + c[3]};
            shared.clips_begin = std::uint32_t(list.clips.size());
            shared.clips_count = std::uint32_t(item.rounded_clips.size());
            list.clips.insert(list.clips.end(), item.rounded_clips.begin(), item.rounded_clips.end());
            if (!item.image_source.empty()) {
                const auto found = images.find(item.image_source);
                if (found == images.end()) {
                    throw std::runtime_error("WEFT_RESOURCE_PROFILE: image not in fixture host allowlist");
                }
                paint::Image payload;
                payload.asset = lease(found->second);
                payload.uv = item.uv;
                image(p.x, p.y, p.width, p.height, payload);
            } else if (p.kind == PaintKind::Rectangle) {
                paint::Item box;
                box.kind = paint::Item::Kind::Box;
                box.payload = std::uint32_t(list.boxes.size());
                list.boxes.emplace_back().background = p.srgb;
                place(box, p.x, p.y, p.width, p.height);
            } else {
                float pen_x = p.x;
                float pen_y = p.y;
                for (const auto& glyph : p.shaped.glyphs) {
                    const auto key = std::tuple{glyph.font_id, glyph.glyph_id, p.font_size};
                    auto cached = cache.glyphs.find(key);
                    if (cached == cache.glyphs.end()) {
                        ++cache.misses;
                        GlyphImages::Glyph made;
                        made.bitmap = font.rasterize(glyph, p.font_size);
                        auto& b = made.bitmap;
                        const auto bytes = b.coverage.size();
                        if (glyphs && bytes > constants::layout::glyph_image_cache_max_bytes) {
                            throw std::runtime_error("glyph preparation cache byte bound");
                        }
                        if (glyphs && (cache.glyphs.size() >= constants::layout::glyph_image_cache_max_entries ||
                                       bytes > constants::layout::glyph_image_cache_max_bytes - cache.bytes)) {
                            cache.glyphs.clear();
                            cache.bytes = 0;
                        }
                        if (b.width && b.height) {
                            cache.bytes += bytes;
                            made.asset = std::make_shared<const paint::Asset>(
                                paint::Asset{b.width, b.height, paint::Encoding::Coverage, std::move(b.coverage)});
                        }
                        cached = cache.glyphs.emplace(key, std::move(made)).first;
                    } else {
                        ++cache.hits;
                    }
                    const auto& b = cached->second.bitmap;
                    if (cached->second.asset) {
                        paint::Image payload;
                        payload.asset = lease(cached->second.asset);
                        payload.color = p.srgb;
                        image(pen_x + glyph.offset_x + b.left, pen_y - glyph.offset_y - b.top, float(b.width),
                              float(b.height), payload);
                    }
                    pen_x += glyph.advance_x;
                    pen_y -= glyph.advance_y;
                }
            }
        }
        return list;
    }
} // namespace weft
