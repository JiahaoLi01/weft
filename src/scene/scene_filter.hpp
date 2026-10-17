#pragma once

#include "base/result.hpp"
#include "dom/node.hpp"
#include "paint/display_list.hpp"
#include "style/values.hpp"

#include <functional>
#include <optional>
#include <string_view>
#include <vector>

// backdrop-filter as scene elements take it (Filter Effects 2): none, or filter functions in order: at most one blur()
// (a length in px, its standard deviation) and the color functions brightness(), contrast(), grayscale(), invert(),
// opacity(), saturate() and sepia() (a number or a percentage, 1 when left out; grayscale, invert, opacity and sepia
// capped at 1) and hue-rotate() (an angle in deg, rad, grad or turn). The color functions before the blur make the
// matrix applied before it, those after it the one applied after, each matrix their composition in order: color values
// clamp to [0, 1] once per matrix, not after each function.
//
// filter as scene elements take it (Filter Effects 1): the same functions, any number of each, every one a function
// of its own, applied in order (color values clamp after each); drop-shadow(); and url(#id) of an SVG filter element.
namespace weft {
    struct SceneNode;

    struct SceneBackdropFilter {
        float blur{};
        paint::ColorMatrix pre = paint::identity_color_matrix;
        paint::ColorMatrix post = paint::identity_color_matrix;

        friend bool operator==(const SceneBackdropFilter&, const SceneBackdropFilter&) = default;
    };

    // nullopt for none (or an empty value). Diag::SceneProfile for a function the scene does not take, a second blur(),
    // a negative or non-finite value, a blur past constants::max_backdrop_blur, or other text.
    Result<std::optional<SceneBackdropFilter>> scene_backdrop_filter(std::string_view value);

    // An element a filter's url(#id) names: its tag, namespace and attributes, and those of its element children, the
    // primitives of a filter element.
    struct SceneFilterElement {
        struct Child {
            std::string_view tag;
            std::string_view space;
            const AttributeMap* attributes{};
        };

        std::string_view tag;
        std::string_view space;
        const AttributeMap* attributes{};
        std::vector<Child> children;
    };

    // The element of an id, the first in tree order, or none.
    using SceneFilterResolve = std::function<std::optional<SceneFilterElement>(std::string_view id)>;

    // Resolvers over the nodes of a scene tree and over a DOM snapshot, which must outlive them.
    SceneFilterResolve scene_filter_resolver(const SceneNode& root);
    SceneFilterResolve scene_filter_resolver(const DomSnapshot& root);

    // The functions of an element's filter (style's filter) in order, empty for none, an empty value, or a url() of no
    // element or of no SVG filter element (Filter Effects 1 §5: the filter is then ignored). Each is one of:
    // - blur(): a length in px (0 without its unit), its standard deviation; 0 when left out.
    // - drop-shadow(): a color (currentcolor without one) and two or three lengths in px or em, the offset and the
    //   standard deviation of its blur, as Chrome takes them.
    // - the color functions of backdrop-filter, each its own matrix.
    // - url(#id) of an SVG filter element whose primitives are feColorMatrix only (type matrix, saturate, hueRotate or
    //   luminanceToAlpha; reading SourceGraphic or the primitive before), each its own matrix, on linear-light values
    //   unless its color-interpolation-filters (the primitive's attribute, else the filter element's) is sRGB or auto;
    //   a filter element of no primitives draws nothing. The filter region and primitive subregions are not drawn.
    // Functions that change nothing (blur(0), identity matrices, a transparent drop-shadow()) are left out.
    // Diag::SceneProfile for a function the scene does not take, a negative or non-finite value, a blur past
    // constants::max_filter_blur or an offset past constants::max_filter_offset, more than
    // constants::max_filter_functions functions, other text, or a filter element of other primitives.
    Result<std::vector<paint::FilterOp>> scene_filter(std::string_view value, const StyleValues& style,
                                                      const SceneFilterResolve& resolve);

    // Whether an element's filter draws as a path shadow (paint::PathShadow) rather than a filtered image: an svg
    // element whose filter is one drop-shadow() that shows, whose box paints nothing (no background, border, box
    // shadow, border image, effect or particles) and whose content is shapes alone (no text, and no element of an
    // opacity below 1). The drop shadow of a CRPG icon is one.
    bool scene_path_shadowed(const SceneNode& n);
} // namespace weft
