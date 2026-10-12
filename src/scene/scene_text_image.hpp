#pragma once

#include "scene/scene.hpp"

#include <array>

// Portable text images of scene text nodes: FreeType coverage of the SceneText glyph runs, and the paint of color
// glyphs (Font::rasterize_color), laid out as the reference raster lays out a text node. Renderers sample them as masks
// or premultiplied color.
namespace weft {
    // Top-left of the line box of a text node's single line inside its border box: text-align, flex centering of
    // the text, padding and border. The baseline is metrics.baseline below it.
    std::array<float, 2> scene_text_origin(const SceneNode& node, const SceneTextMetrics& metrics);

    struct SceneTextImage {
        // Encoded sRGB RGBA with straight alpha, (box + 2 * margin) * scale pixels, rounded up.
        SceneBitmap bitmap;
        // Some color glyph has paint that Font::rasterize_color cannot draw (PNG or SVG images): it is not drawn, and
        // the caller needs another raster for it. COLR glyphs are drawn, over the text's other glyphs.
        bool color_glyphs{};
    };

    // The node's text in its computed color at its border box origin (margin, margin), scale device px per CSS px.
    SceneTextImage scene_text_image(SceneText& text, const SceneNode& node, float scale, float margin);
} // namespace weft
