#pragma once

#include "paint/display_list.hpp"
#include "render/scene_pass.hpp"

#include <array>
#include <cstdint>
#include <vector>

// A filter's chain (paint::Filter, CSS filter): its functions as fullscreen passes of the backdrop shaders
// (src/render/shaders/backdrop.hlsl) between images of its own, every image covering the filter's box, the first its
// content. A blur is the backdrop's: for a standard deviation over max_backdrop_sigma texels, an area downsample to the
// size that brings it to that first, then a horizontal and a vertical Gaussian pass, addresses past an image's edge
// mirrored back into it; a color matrix is one pass on straight values; a drop shadow is the image blurred, its alpha
// tinted and offset by a matrix pass into a new image, then the image drawn source-over it by an identity matrix pass.
// A pass that reads a smaller image upsamples it bilinearly. The filter's content has a transparent margin at least as
// wide as what its functions spread and offset (scene_filter_region), so that mirrored addresses read nothing. Backends
// run the passes on their targets; run_filter_chain runs them on the CPU, as the D2D reference raster draws a filter.
namespace weft::render {
    struct FilterStep {
        enum class Shader : std::uint8_t {
            // BACKDROP_AREA, BACKDROP_BLUR and BACKDROP_MATRIX.
            Area,
            Blur,
            Matrix
        };

        Shader shader{};
        // The image the pass reads (0: the content) and the one it draws into (past 0), which it clears first; over:
        // the pass draws source-over what the image holds instead.
        std::uint32_t source{};
        std::uint32_t target{};
        bool over{};
        BackdropConstants constants;
    };

    struct FilterChain {
        // Each image's size in texels, the content's first.
        std::vector<std::array<std::uint32_t, 2>> images;
        std::vector<FilterStep> steps;
        // The image the last pass draws into: the filtered image (0 for a chain of no passes).
        std::uint32_t result{};
    };

    // The passes of ops over a content image of width x height texels, resolution of them per item px.
    FilterChain plan_filter_chain(const std::vector<paint::FilterOp>& ops, std::uint32_t width, std::uint32_t height,
                                  float resolution);

    // An image of a chain as an 8-bit target holds it: encoded premultiplied RGBA in [0, 1], rows top down.
    struct FilterPixels {
        std::uint32_t width{};
        std::uint32_t height{};
        std::vector<std::array<float, 4>> rgba;
    };

    // Runs a chain's passes on the CPU over its content, as the backends run them: each pass's output quantized to 8
    // bits, mirrored addresses, bilinear samples, source-over blending. The result image.
    FilterPixels run_filter_chain(const FilterChain& chain, FilterPixels content);
} // namespace weft::render
