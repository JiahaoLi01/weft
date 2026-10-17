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
        // content), the group at depth index (each of the pass's size), or image `image` of backdrop chain index.
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
        // an atlas page (ScenePrepared::pages), the group at depth index, or the last image of backdrop chain index
        // (a backdrop's filtered copy, or the copy an effect that reads its backdrop samples).
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
        // (destination-over), the particle pass, or an effect program's encoded pass.
        enum class Pipeline : std::uint8_t {
            Scene,
            Under,
            Particles,
            Effect
        };

        // What a draw binds in the effect texture slots: nothing, an effect's textures (ScenePrepared::effects
        // [source]), or an emission's sprite (ScenePrepared::sprites[source], none for discs) where the first goes.
        enum class Extra : std::uint8_t {
            None,
            Effect,
            Sprite
        };

        // Drawing into target starts: cleared to clear (premultiplied in the pass's space) when given, otherwise
        // keeping what it holds. The viewport is the pass's size, or a chain image's.
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
        // and extra.
        struct Draw {
            std::uint32_t first{};
            std::uint32_t count{};
            std::uint32_t vertices{};
            Pipeline pipeline{};
            Image image;
            Extra extra{};
            std::uint32_t source{};
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

        std::vector<Command> commands;
        std::vector<Chain> chains;
        // Groups the pass nests, one target each (ScenePass::group_depth).
        std::size_t groups{};
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
    };

    // The schedule of prepared's pass with host_textures bound to its texture names. clear: for a pass drawn over its
    // target (ScenePrepared::over), the color the target is cleared to first (none: kept); a pass that replaces its
    // target begins cleared to its own clear. out_readable: whether a backdrop may copy from the pass's own target.
    SceneSchedule schedule_scene_pass(const ScenePrepared& prepared, std::span<const SceneTexture> host_textures = {},
                                      const paint::Color* clear = nullptr, bool out_readable = true);
} // namespace weft
