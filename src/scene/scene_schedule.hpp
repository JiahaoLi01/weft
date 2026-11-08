#pragma once

#include "paint/display_list.hpp"
#include "render/scene_pass.hpp"
#include "scene/scene_native.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

// The commands that record a prepared scene pass (ScenePrepared) on a backend that draws into one target at a time
// and copies between targets outside of drawing: Vulkan (render passes), D3D12 (render targets and copies) and Metal
// (render and blit command encoders). A schedule names by role the targets it draws into (the pass's own, a group's,
// a backdrop chain's images), what each draw samples and with which pipeline, and the order of it all: begin drawing
// into a target (cleared, or keeping what it holds), draw, end drawing into it, copy a backdrop's region of a target,
// filter the copy. A backend maps the roles to its own textures, bindings and pipelines and runs the commands as they
// come; it records the nested passes the draws sample (plane contents, layers drawn the first time) before them.
namespace weft {
    struct SceneSchedule {
        // A target the commands draw into: the pass's own (the host's target, or the image of a plane's or a layer's
        // content), a group's (index below groups: the group at depth index, of the pass's size; past them: hoisted
        // image index - groups), or image `image` of backdrop chain index.
        struct Target {
            enum class Kind : std::uint8_t {
                Out,
                Group,
                Chain
            };

            Kind kind{};
            std::uint32_t index{};
            std::uint32_t image{};

            bool operator==(const Target&) const = default;
        };

        // The texture a draw samples as its image: none, the image item index draws (its image, its plane's content,
        // its layer, its filtered image, or the host texture its name binds; one bound without a view draws nothing),
        // an atlas page (ScenePrepared::pages), a group's (as a Target of kind Group names it), or the last image of
        // backdrop chain index (a backdrop's filtered copy, or the copy an effect that reads its backdrop samples). A
        // backend tells a group's composite where its image lies among the pass's device px (scene.hlsl,
        // View::sampled).
        struct Image {
            enum class Kind : std::uint8_t {
                Blank,
                Item,
                Page,
                Group,
                Backdrop
            };

            Kind kind{};
            std::uint32_t index{};

            bool operator==(const Image&) const = default;
        };

        // The pipeline of a draw: the scene's records with source-over, a backdrop beneath its group's content
        // (destination-over), the particle pass, an effect program's encoded pass, or a blending group's composite
        // that reads what the target holds where it draws (SceneScheduleOptions::fetch_blends: no copy of its
        // backdrop).
        enum class Pipeline : std::uint8_t {
            Scene,
            Under,
            Particles,
            Effect,
            Blend
        };

        // What a draw binds in the effect texture slots: nothing, an effect's textures (ScenePrepared::effects
        // [source]), an emission's sprite (ScenePrepared::sprites[source], none for discs) where the first goes, or the
        // image of the mask the drawn item's chain holds (ScenePrepared::mask_images[source], source the item) there.
        enum class Extra : std::uint8_t {
            None,
            Effect,
            Sprite,
            Mask
        };

        // Drawing into target starts: cleared to clear (premultiplied in the pass's space) when given, otherwise
        // keeping what it holds. The viewport is the pass's size, a hoisted image's (its origin among the pass's
        // device px the View's: scene.hlsl), or a chain image's.
        struct Begin {
            Target target;
            std::optional<paint::Color> clear;
        };

        // Drawing into target ends; sampled: a later draw or filter samples it (a group, a chain's image).
        struct End {
            Target target;
            bool sampled{};
        };

        // Records [first, first + count) as instances of `vertices` vertices each (a particle emission: one instance
        // of six vertices per particle), with pipeline (an effect: ScenePrepared::effects[source]'s program), image
        // and extra; and a blending group's composite, the last image of backdrop chain `backdrop` where an effect's
        // second texture goes (the shader's blendBackdrop).
        struct Draw {
            std::uint32_t first{};
            std::uint32_t count{};
            std::uint32_t vertices{};
            Pipeline pipeline{};
            Image image;
            Extra extra{};
            std::uint32_t source{};
            std::optional<std::uint32_t> backdrop;
        };

        // The region (left, top, right, bottom device px) of target `from` copied into chain's first image, at its
        // origin.
        struct Copy {
            Target from;
            std::array<std::int32_t, 4> region{};
            std::uint32_t chain{};
        };

        // Pass `pass` of chain: its filter over all of image `pass` drawn into image `pass + 1`, cleared first.
        struct Filter {
            std::uint32_t chain{};
            std::uint32_t pass{};
        };

        using Command = std::variant<Begin, End, Draw, Copy, Filter>;

        // A backdrop's chain: the sizes of its images (the copy, then each filter pass's target) and its passes in
        // order, each with its constants (render/scene_pass.hpp); a Copy step's (render::ScenePass::Copy) is its copy
        // alone, without passes. from_out: the copy reads the pass's own target (its format), not a group's.
        // unreadable: that target cannot be read, and the commands clear the last image instead of copying and
        // filtering (the backdrop, or what the effect reads, is transparent).
        struct Chain {
            std::vector<std::array<std::uint32_t, 2>> images;
            std::vector<std::pair<render::BackdropFilter, render::BackdropConstants>> passes;
            bool from_out{};
            bool unreadable{};
        };

        // A group drawn ahead into an image of its own (hoisted): the image's size, and its origin among the pass's
        // device px, at the device px its composite covers.
        struct Hoisted {
            std::array<std::uint32_t, 2> size{};
            std::array<std::int32_t, 2> origin{};
        };

        std::vector<Command> commands;
        std::vector<Chain> chains;
        // Groups the pass nests, one target of the pass's size each (ScenePass::group_depth), which groups drawn in
        // turn use by their depth.
        std::size_t groups{};
        // The hoisted groups' images, in the order the groups begin (Target index groups + k).
        std::vector<Hoisted> hoisted;
        // Per item, the host texture it draws (null: none, or a name nothing binds), and how many items draw a bound
        // texture and how many name one nothing binds.
        std::vector<const SceneTexture*> textures;
        std::size_t bound_textures{};
        std::size_t unbound_textures{};
        // Backdrops drawn, and those of them that could not read their target.
        std::size_t backdrops{};
        std::size_t unreadable_backdrops{};
        // Copies taken for effects that read their backdrop (Copy steps), and those of them of a target that could
        // not be read.
        std::size_t copies{};
        std::size_t unreadable_copies{};
        // Groups that blended (each copies its backdrop as it ends, unless it reads the target where it draws), and
        // those of them whose target could not be copied (they blend with a transparent backdrop: as if normal).
        std::size_t blends{};
        std::size_t unreadable_blends{};
    };

    // How a backend runs groups. By default they draw in turn: the target a group composites into ends where the
    // group begins, the group draws into the target of its depth, and the target resumes for its composite; and a
    // group that blends copies what lies under it first, the target ended for the copy. On a tiled GPU each such
    // break writes the whole target out and reads it back.
    struct SceneScheduleOptions {
        // A group that nothing in reads a target (no backdrop, copy, or blending group that copies) draws instead,
        // before the target it composites into begins, into an image of the device px its composite covers (Hoisted),
        // within a budget of images (constants::render::scene::hoisted_group_budget), and composites without a break.
        // The backend places a target's origin and a composite's image in the View (scene.hlsl).
        bool hoist{};
        // A group that blends reads what the target holds where it draws (Pipeline::Blend), without a copy or a break.
        bool fetch_blends{};
    };

    // The schedule of prepared's pass with host_textures bound to its texture names. clear: for a pass drawn over its
    // target (ScenePrepared::over), the color the target is cleared to first (none: kept); a pass that replaces its
    // target begins cleared to its own clear. out_readable: whether a backdrop may copy from the pass's own target.
    SceneSchedule schedule_scene_pass(const ScenePrepared& prepared, std::span<const SceneTexture> host_textures = {},
                                      const paint::Color* clear = nullptr, bool out_readable = true,
                                      SceneScheduleOptions options = {});
} // namespace weft
