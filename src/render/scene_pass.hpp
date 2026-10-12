#pragma once

#include "constants/render.hpp"
#include "paint/display_list.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

// The portable plan of a scene pass: a display list as instance records of the shared scene shader
// (src/render/shaders/scene.hlsl) and the steps that draw them. A backend uploads the records and runs the
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
        // Image origin and extent in its asset.
        Vector uv{0, 0, 1, 1};
        // Image mode, then distance spread and stroke in source texels, and the texels one device pixel spans for the
        // antialiasing of a linear pass's distance edges.
        Vector mode;
        Vector outline;
        // First rounded clip and count in ScenePass::clips.
        Vector clips;
    };

    static_assert(sizeof(SceneRecord) == scene_limits::record_bytes);

    // The shader's Clip record: a normalized rounded clip.
    struct SceneClip {
        std::array<float, 4> rect;
        std::array<float, 4> radius_x;
        std::array<float, 4> radius_y;
    };

    static_assert(sizeof(SceneClip) == scene_limits::clip_bytes);

    // How a pass composites: encoded premultiplied colors (the scene contract shared with the D2D reference and
    // Chrome), or linear premultiplied colors (the engine's documents).
    enum class Space : std::uint8_t {
        Encoded,
        Linear
    };

    struct ScenePass {
        enum class Step : std::uint8_t {
            // Records [first, first + count) in one instanced draw without an image.
            Boxes,
            // Records [first, first + count) in one instanced draw of the image of source: one Text, Layer, Shadow,
            // Path or Image item, or with texture names a run of boxes and images that sample one texture.
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
            // The image source (DisplayList asset index, past the assets an external texture) an Image step binds.
            std::uint32_t source{};
        };

        // One record per item, in item order: record i belongs to item i.
        std::vector<SceneRecord> records;
        // Per item, the device pixels (left, top, right, bottom) a path item's coverage mask covers: what the renderer
        // rasterizes and record i draws. Empty for other items and for paths with nothing in their clip.
        std::vector<std::array<std::int32_t, 4>> masks;
        std::vector<SceneClip> clips;
        std::vector<Batch> batches;
        // The target's initial color, premultiplied in the pass's space: an opaque first box covering the whole target
        // becomes the clear.
        paint::Color clear{};
        Space space{};
        std::size_t group_depth{};
    };

    // Makes an image record sample its image from region (normalized origin and extent) of the bound texture, as an
    // atlas page holds it.
    inline void place_in(SceneRecord& record, const std::array<float, 4>& region) {
        record.uv = {region[0] + record.uv[0] * region[2], region[1] + record.uv[1] * region[3],
                     record.uv[2] * region[2], record.uv[3] * region[3]};
    }

    // Makes an image record sample its image bottom up, for a texture whose rows start at the bottom.
    inline void flip_rows(SceneRecord& record) {
        record.uv = {record.uv[0], 1 - record.uv[1], record.uv[2], -record.uv[3]};
    }

    // Plans a list that passed paint::check for a width x height device px target at scale device px per CSS px,
    // with every item clip narrowed to crop when given. A linear pass takes its colors linear and premultiplied from
    // here; rounded clips are normalized. A clip outside the rounded clip bounds, or a chain longer than
    // max_rounded_clips, is Diag::PaintList. textures, when given, names the texture of each image source (equal names
    // for one texture, such as an atlas page); then a run of consecutive boxes and images that sample one texture is
    // one draw.
    Result<ScenePass> plan_scene_pass(const paint::DisplayList& list, float scale, unsigned width, unsigned height,
                                      std::optional<paint::ClipRect> crop = std::nullopt, Space space = Space::Encoded,
                                      const std::vector<std::uint64_t>* textures = nullptr);
} // namespace weft::render
