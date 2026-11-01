#pragma once

#include "scene/scene.hpp"

#include <array>
#include <optional>

// Portable text images of scene text nodes: FreeType coverage of the SceneText glyph runs, and the paint of color
// glyphs (Font::rasterize_color), laid out as the reference raster lays out a text node. Renderers sample them as masks
// or premultiplied color.
namespace weft {
    // Top-left of the line box of a text node's single line inside its border box: text-align, flex centering of
    // the text, padding and border. The baseline is metrics.baseline below it.
    std::array<float, 2> scene_text_origin(const SceneNode& node, const SceneTextMetrics& metrics);

    // Where still text's image goes on the device's pixels (paint::Raster::still), as the native passes and the
    // reference raster draw it: an image whose corner lies at corner (device px) and whose line's baseline lies
    // baseline device px below that corner goes with its corner on the whole pixel corner, its glyphs drawn for phase
    // across (scene_text_image). The baseline goes on the pixel row nearest it (rounded half up), the image keeping it on
    // a row of its own wherever it lies; across, phase is where the corner lies in a pixel in steps of
    // text_phase_steps, so that a line moving across draws from at most that many images.
    struct SceneTextPlace {
        std::array<float, 2> corner;
        float phase{};
    };

    SceneTextPlace scene_text_place(std::array<float, 2> corner, float baseline);

    // The baseline of a node's line, device px below the corner of its text image (scene_text_image).
    float scene_text_baseline(SceneText& text, const SceneNode& node, float scale, float margin);

    struct SceneTextImage {
        // Encoded sRGB RGBA with straight alpha, (box + 2 * margin) * scale pixels, rounded up.
        SceneBitmap bitmap;
        // Some color glyph has paint that Font::rasterize_color cannot draw (PNG or SVG images): it is not drawn, and
        // the caller needs another raster for it. COLR glyphs are drawn, over the text's other glyphs.
        bool color_glyphs{};
    };

    // The node's text in its computed color at its border box origin (margin, margin), scale device px per CSS px.
    // With a phase, each glyph lies where Chrome draws an axis-aligned glyph on the device's pixels (Skia's subpixel
    // positioning in x only), the image's texels the target's pixels and the node's origin phase device px right of
    // the corner of its first texel ([0, 1)): its baseline on a whole pixel row of the image and its pen on a quarter of a
    // pixel, each rounded half up. Without, each lies where the node's line puts it.
    SceneTextImage scene_text_image(SceneText& text, const SceneNode& node, float scale, float margin,
                                    std::optional<float> phase = std::nullopt);
} // namespace weft
