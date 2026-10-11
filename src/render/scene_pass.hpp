#pragma once

#include "constants/render.hpp"
#include "paint/display_list.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

// The portable plan of a scene pass: a display list as instance records of the shared scene shader
// (src/render/backends/d3d11/scene.hlsl) and the steps that draw them. A backend uploads the records and runs the
// steps with its own targets, images and draw calls.
namespace weft::render {
    namespace scene_limits = constants::render::scene;

    // The shader's Paint record: 24 float4 registers.
    struct SceneRecord {
        using Vector = std::array<float, 4>;

        Vector bounds;
        Vector inv_x;
        Vector inv_y;
        Vector size;
        Vector clip;
        Vector radii;
        Vector widths;
        Vector background;
        std::array<Vector, 4> edges;
        Vector tile;
        Vector extra;
        std::array<Vector, scene_limits::max_gradient_stops> stops;
        std::array<Vector, scene_limits::gradient_offset_vectors> offsets;
    };

    static_assert(sizeof(SceneRecord) == scene_limits::record_bytes);

    struct ScenePass {
        enum class Step : std::uint8_t {
            // Records [first, first + count) in one instanced draw without an image.
            Boxes,
            // Record first, sampling the image of its Text or Layer item.
            Image,
            // Continue in a cleared group target one level deeper.
            BeginGroup,
            // Return to the enclosing target, where record first composites the group's target.
            EndGroup
        };

        struct Batch {
            Step step{};
            std::uint32_t first{};
            std::uint32_t count{};
        };

        // One record per item, in item order: record i belongs to item i.
        std::vector<SceneRecord> records;
        std::vector<Batch> batches;
        // The target's initial color: an opaque first box covering the whole target becomes the clear.
        paint::Color clear{};
        std::size_t group_depth{};
    };

    // Plans a list that passed paint::check for a width x height device px target at scale device px per CSS px,
    // with every item clip narrowed to crop when given.
    ScenePass plan_scene_pass(const paint::DisplayList& list, float scale, unsigned width, unsigned height,
                              std::optional<paint::ClipRect> crop = std::nullopt);
} // namespace weft::render
