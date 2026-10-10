#pragma once

#include "probe_common.hpp"
#include "cheetah_probe.hpp"
#include "scroll_timing.hpp"
#include "probe_constants.hpp"

namespace probes {
    inline bool same_style_input(const weft::DomSnapshot& a, const weft::DomSnapshot& b) {
        if (a.identity != b.identity || a.type != b.type || a.local_name != b.local_name ||
            a.namespace_uri != b.namespace_uri || a.data != b.data || a.attributes != b.attributes ||
            a.children.size() != b.children.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.children.size(); ++i) {
            if (!same_style_input(a.children[i], b.children[i])) {
                return false;
            }
        }
        return true; // Scroll offsets affect geometry/paint, never this fixed CSS profile.
    }

    // Logical layout/identities stay intact. Rasterize glyphs again at surface ppem
    // after scaling bearings-independent glyph advances, offsets and all clips.
    inline weft::PositionedPage raster_flow_page(weft::PositionedPage page, UINT w, UINT h) {
        const auto v = weft::Viewport::fit(float(w), float(h));
        auto rect = [&](std::array<float, 4>& r) {
            r = {v.x + r[0] * v.scale, v.y + r[1] * v.scale, r[2] * v.scale, r[3] * v.scale};
        };
        for (auto& item : page.paints) {
            auto& p = item.paint;
            p.x = v.x + p.x * v.scale;
            p.y = v.y + p.y * v.scale;
            p.width *= v.scale;
            p.height *= v.scale;
            p.font_size *= v.scale;
            p.shaped.advance *= v.scale;
            for (auto& g : p.shaped.glyphs) {
                g.advance_x *= v.scale;
                g.advance_y *= v.scale;
                g.offset_x *= v.scale;
                g.offset_y *= v.scale;
            }
            rect(item.clip);
            for (auto& c : item.rounded_clips) {
                rect(c.rect);
                for (auto& r : c.radius_x) {
                    r *= v.scale;
                }
                for (auto& r : c.radius_y) {
                    r *= v.scale;
                }
            }
        }
        weft::PositionedPaint background;
        background.paint.kind = weft::PaintKind::Rectangle;
        background.paint.width = float(w);
        background.paint.height = float(h);
        background.paint.srgb = {0, 0, 0, 1};
        background.clip = {0, 0, float(w), float(h)};
        page.paints.insert(page.paints.begin(), std::move(background));
        return page;
    }

    inline FramePacket frame_rate_overlay(weft::FontSet& fonts, const std::string& text) {
        weft::PositionedPage page;
        const std::array<float, 4> clip{weft::probe_constants::fps_hud_x, 16, 524, 112};
        weft::PositionedPaint background;
        background.paint.kind = weft::PaintKind::Rectangle;
        background.paint.x = clip[0];
        background.paint.y = clip[1];
        background.paint.width = clip[2];
        background.paint.height = clip[3];
        background.paint.srgb = {.02f, .03f, .05f, .92f};
        background.clip = clip;
        page.paints.push_back(std::move(background));
        auto line = [&](const std::string& value, float y, float size) {
            weft::PositionedPaint item;
            auto& p = item.paint;
            p.kind = weft::PaintKind::Text;
            p.x = 3320;
            p.y = y;
            p.font_size = size;
            p.srgb = {.8f, 1, .8f, 1};
            p.shaped = fonts.shape(value, size).shaped;
            item.clip = clip;
            page.paints.push_back(std::move(item));
        };
        line(text, 62, 34);
        line("3840 x 2160  |  D3D11", 104, 26);
        return cheetah_packet(page, fonts, {});
    }

    // Experimental flow owner loop. HWND callbacks enqueue values only.
} // namespace probes
