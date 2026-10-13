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

    // The shader's Clip record: a normalized rounded clip, and the rows of its map from target px into its space
    // ({a, c, e} and {b, d, f} of RoundedClip::to_local). inv_x[3] is how many target px one unit of the clip's
    // space spans (1 for target px): its antialiasing scale.
    struct SceneClip {
        std::array<float, 4> rect;
        std::array<float, 4> radius_x;
        std::array<float, 4> radius_y;
        std::array<float, 4> inv_x;
        std::array<float, 4> inv_y;
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
            // Path, Plane or Image item, or with texture names a run of boxes and images that sample one texture.
            Image,
            // Continue in a cleared group target one level deeper.
            BeginGroup,
            // Return to the enclosing target, where record first composites the group's target.
            EndGroup,
            // Records [first, first + count) in one instanced draw of an effect: consecutive Effect items of one
            // program and the same textures, the program and textures of DisplayList::effects[source].
            Effect,
            // Record first's count particles in one draw of the particle shaders: 6 x count vertices of one instance,
            // vertex v drawing particle v / 6. source is the emission's DisplayList::particles index.
            Particles,
            // Record first's backdrop, ScenePass::backdrops[source]: copy its region of the target it reads, filter
            // the copy as the backdrop says, and draw record first sampling the result (image_mode_backdrop): over the
            // current target, or with under beneath what the current group holds (destination over).
            Backdrop
        };

        struct Batch {
            Step step{};
            std::uint32_t first{};
            std::uint32_t count{};
            // The image source (DisplayList asset index, past the assets an external texture) an Image step binds, or
            // the effect (DisplayList::effects index) of an Effect step's first item.
            std::uint32_t source{};
        };

        // What a Backdrop step reads and how it filters it: the device pixels (left, top, right, bottom) of the
        // target it reads, within the target; the matrix (DisplayList::Backdrop::pre) applied to the copy first; the
        // size the copy is downsampled to by area (its own when it blurs no more than max_backdrop_sigma), and the
        // standard deviations of the Gaussian blur across and down at that size (0: none); and whether it reads the
        // target that encloses the current group and draws beneath the group's content.
        struct Backdrop {
            std::array<std::int32_t, 4> region{};
            paint::ColorMatrix pre = paint::identity_color_matrix;
            std::array<unsigned, 2> size{};
            std::array<float, 2> sigma{};
            bool under{};
        };

        // One record per item, in item order: record i belongs to item i.
        std::vector<SceneRecord> records;
        // Per item, the device pixels (left, top, right, bottom) a path item's coverage mask covers: what the renderer
        // rasterizes and record i draws. Empty for other items and for paths with nothing in their clip.
        std::vector<std::array<std::int32_t, 4>> masks;
        std::vector<SceneClip> clips;
        std::vector<Batch> batches;
        std::vector<Backdrop> backdrops;
        // The target's initial color, premultiplied in the pass's space: an opaque first box covering the whole target
        // becomes the clear.
        paint::Color clear{};
        Space space{};
        std::size_t group_depth{};
    };

    // The constant buffer of a backdrop pass (src/render/shaders/backdrop.hlsl, BackdropPass): the source's texel size and the
    // step between taps in uv, the kernel's weights from its centre out, the taps on each side, and a color matrix.
    struct BackdropConstants {
        SceneRecord::Vector texel{};
        std::array<SceneRecord::Vector, 4> weights{};
        SceneRecord::Vector extent{};
        std::array<SceneRecord::Vector, 4> rows{};
        SceneRecord::Vector offsets{};
    };

    static_assert(sizeof(BackdropConstants) == scene_limits::backdrop_constants_bytes);

    // The constants of a backdrop's area downsample across (or down) from a source of from texels to to texels on that
    // axis, each output texel the mean of the source texels its span covers, in part at its ends (BACKDROP_AREA); of a
    // Gaussian pass of standard deviation sigma (texels, above 0) across a w x h source or down it (BACKDROP_BLUR); and
    // of a color matrix (BACKDROP_MATRIX).
    BackdropConstants backdrop_area_constants(unsigned from, unsigned to, bool across);
    BackdropConstants backdrop_blur_constants(float sigma, bool across, unsigned w, unsigned h);
    BackdropConstants backdrop_matrix_constants(const paint::ColorMatrix& m);

    // The device pixels of a plane's content image: the plane with image_margin transparent px on each side, at scale,
    // rounded up. A renderer draws the content image_margin px in, texel per device pixel, and binds the image to the
    // plane's record.
    std::array<std::uint32_t, 2> plane_pixels(const paint::Plane& plane, float scale);

    // The field of an effect record that holds parameter i (below constants::max_effect_parameters): the fields effect
    // records leave unused, in the order the shader's effect pass reads them (scene.hlsl, ps_main of WEFT_EFFECT).
    inline SceneRecord::Vector& effect_parameter(SceneRecord& record, unsigned i) {
        static_assert(scene_limits::max_gradient_stops == 8 && scene_limits::gradient_offset_vectors == 2 &&
                      constants::max_effect_parameters == 16);
        if (i < 8) {
            return record.stops[i];
        }
        if (i < 12) {
            return record.edges[i - 8];
        }
        if (i < 14) {
            return record.offsets[i - 12];
        }
        return i == 14 ? record.tile : record.background;
    }

    // The fields of a particles record that hold its emission, by effect_parameter's order (the shader's particle
    // pass, scene.hlsl WEFT_PARTICLES, reads them in the same order): the item's map into target px (rows {a, c, e} and
    // {b, d, f}); the timing (paint::ParticleTiming's age and interval, the newest index, and 1 for a burst); the count,
    // a burst's duration, the seed's low 24 bits and the shape; the area; life and speed ranges; direction, spread,
    // drag and softness; gravity and the spin range; the angle and scale ranges; the four sizes; the four colors
    // (straight linear RGBA); the sprite's columns, rows, frame rate and mode (particle_sprite_*); the target and
    // whether there is one. The sprite's region of its texture is the record's uv.
    enum ParticleField : unsigned {
        ParticleMapX,
        ParticleMapY,
        ParticleTiming,
        ParticleEmission,
        ParticleArea,
        ParticleLifeSpeed,
        ParticleHeading,
        ParticleForces,
        ParticleTurn,
        ParticleSize,
        ParticleColor,
        ParticleSprite = ParticleColor + 4,
        ParticleTarget
    };

    // The fields of a backdrop record that hold its color matrix (DisplayList::Backdrop::post), by effect_parameter's
    // order: the rows of r, g, b and a, then their offsets.
    inline constexpr unsigned backdrop_matrix_fields = 5;

    inline SceneRecord::Vector& particle_field(SceneRecord& record, ParticleField field) {
        static_assert(ParticleTarget + 1 == constants::max_effect_parameters);
        return effect_parameter(record, field);
    }

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
