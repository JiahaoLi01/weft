#pragma once

#include "paint/display_list.hpp"
#include "style/values.hpp"

#include <array>
#include <cstdint>
#include <optional>

// The background of a scene element whose image has a natural size only the renderer knows, a host texture
// (-weft-texture()) or an image (url(), its size the texture's the renderer makes of it), as CSS Backgrounds 3 lays out a background image: background-origin's box is the positioning
// area, background-clip's box the painting area, background-size, -position and -repeat as paint::TextureFit says. One
// layer, background-attachment scroll.
namespace weft {
    enum class BackgroundBox : std::uint8_t {
        Border,
        Padding,
        Content
    };

    // A length on one axis: px and a share of a reference (a percentage over 100).
    struct BackgroundLength {
        float px{};
        float share{};
    };

    struct BackgroundLayout {
        BackgroundBox origin{BackgroundBox::Padding};
        BackgroundBox clip{BackgroundBox::Border};
        paint::TextureFit::Scale scale{};
        // background-size on each axis with Lengths, shares of the positioning area; auto when not set.
        std::array<std::optional<BackgroundLength>, 2> size{};
        // background-position on each axis: shares of the room the tile leaves, and px.
        std::array<BackgroundLength, 2> position{};
        std::array<paint::TextureFit::Repeat, 2> repeat{};
    };

    // An element's background layout from background-origin, -clip, -size, -position, -repeat and -attachment;
    // std::invalid_argument for a value outside the profile: background-clip text, an attachment other than scroll,
    // a negative size, a position or repeat CSS does not take.
    BackgroundLayout scene_background_layout(const StyleValues& style);

    // An img's image as CSS Images 3 §5.5 and §5.6 place it, a background of one tile in its content box (its origin
    // and clip, no repeat) of the size object-fit gives: fill the content box's, contain and cover scaled to it, none
    // the natural size, scale-down none's or contain's, whichever is smaller (natural is the image's size, content the
    // content box's); at object-position. std::invalid_argument for a value CSS does not take.
    BackgroundLayout scene_object_layout(const StyleValues& style, std::array<float, 2> natural,
                                         std::array<float, 2> content);

    // Whether an element's images (its img's, its background's and border image's) are pixelated: image-rendering
    // pixelated or crisp-edges, as Chrome draws both; auto, smooth, high-quality and -webkit-optimize-contrast filter.
    bool scene_image_pixelated(const StyleValues& style);

    // A box of an element w x h px (its border box at 0, 0) in its px, with its corners' radii, top-left first and
    // clockwise; std::invalid_argument for a content box whose padding is a percentage.
    struct BackgroundRect {
        std::array<float, 4> rect{};
        std::array<float, 4> radius_x{};
        std::array<float, 4> radius_y{};
    };

    BackgroundRect scene_background_box(const StyleValues& style, BackgroundBox box, float w, float h);

    // A length or percentage as backgrounds take one: px, a percentage's share, or calc() of the two; none for
    // another value.
    std::optional<BackgroundLength> scene_length(std::string_view value);

    // A <position> as background-position takes one (CSS Values 4 §9): on each axis px and a share of the room;
    // std::invalid_argument for a value CSS does not take.
    std::array<BackgroundLength, 2> scene_position(std::string_view value);

    // The fit of a host texture over an element's painting area: its positioning area and sizes resolved, in px from
    // the painting area's origin.
    paint::TextureFit scene_texture_fit(const BackgroundLayout& layout, const BackgroundRect& painting,
                                        const BackgroundRect& positioning);

    // Where the tiles of a fitted texture of a natural size lie on one axis, as the scene shader lays them out: the
    // tile's size, where one tile starts, and the period they repeat at (0 when there is one tile). Nothing is drawn
    // for a tile of no size.
    struct TextureTiles {
        float tile{};
        float start{};
        float period{};
    };

    std::array<TextureTiles, 2> scene_texture_tiles(const paint::TextureFit& fit, float natural_width,
                                                    float natural_height);

    // Whether the point x of an axis (px from the painting area's origin) lies in a tile, and where in it (0 to 1).
    struct TexturePoint {
        bool inside{};
        float u{};
    };

    TexturePoint scene_texture_point(const TextureTiles& tiles, float x);
} // namespace weft
