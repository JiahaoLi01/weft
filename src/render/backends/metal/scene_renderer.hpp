#pragma once

// Objective-C++ (ARC): the Metal objects are strong references.
#import <Metal/Metal.h>

#include "scene/scene.hpp"
#include "scene/scene_native.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Native Metal pass of a scene (M.3, docs/render-backend-api.md "Metal"): the passes a ScenePreparer prepares
// (scene/scene_native.hpp), run as the portable schedule says (scene/scene_schedule.hpp), as the Vulkan and D3D12
// passes run it. The shared scene shader's encoded pass (MSL, render/backends/metal/shaders.hpp) draws instanced box
// records; transparent groups draw into scratch textures; perspective planes, explicit layers and filtered images draw
// their content with a nested pass into a texture of their own (a filter's then through its chain of backdrop passes);
// text, shadow and path images are uploaded once each (small ones into their region of an atlas page); UI shader
// effects draw in their encoded pass (paint::EffectProgram::msl_encoded) with their textures. Every target holds encoded
// premultiplied BGRA (MTLPixelFormatBGRA8Unorm), as the D3D11 and Vulkan scene targets do. Beginning a target is a
// render command encoder (load action clear or load), ending it endEncoding, a backdrop's copy a blit command encoder and
// each filter pass a render command encoder of its own; Metal orders their reads and writes of the renderer's tracked
// resources, so the pass needs no barriers.
//
// encode() encodes a frame into the host's command buffer with what it uses in a slot: the host commits it, reports the
// submission (submitted()), and retires the slot once it completed, or discards a slot whose commands it never commits.
// Slots lease the images and layers they sample, so an image of the cache goes only after every frame that sampled it
// completed. What a recording uploads or draws into the cache is pending until its submission: another recording that
// samples it throws Busy. draw() is that sequence on the renderer's own queue, waiting for the frame and reading its
// target back.
//
// A view's pass is kept while its frames keep their revision (SceneFrame::revision), as on Vulkan.
//
// Owner thread of the SceneText.
namespace weft::render::metal {
    // Thrown by encode() when the frame samples what another recording that is not submitted yet uploads or draws.
    struct Busy : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    // Throws std::runtime_error("WEFT_METAL: message") unless condition.
    void ensure(bool condition, std::string_view message);

    // A shared buffer, mapped.
    struct Buffer {
        id<MTLBuffer> buffer;
        std::size_t size{};
        void* mapped{};
    };

    // A texture and its size.
    struct Image {
        id<MTLTexture> texture;
        std::uint32_t width{};
        std::uint32_t height{};
    };

    class SceneRenderer : public SceneNativeWork {
    public:
        // Why the last pass drew nothing (a fallback's reason); empty before the first.
        const std::string& last_failure() const {
            return last_failure_;
        }

        // A host color target: a 2D MTLPixelFormatBGRA8Unorm texture with MTLTextureUsageRenderTarget, at least width x
        // height device px (the pass draws at its top-left). readable: the pass may copy from it (it is not
        // framebufferOnly): backdrops at the target's level read what the pass drew there; otherwise they are
        // transparent, a counted reason (not a fallback).
        struct Target {
            id<MTLTexture> texture;
            std::uint32_t width{};
            std::uint32_t height{};
            bool readable{};
        };

        // What one recorded frame uses: its buffers and scratch textures, leases of the images and layers it samples,
        // and the cache entries it uploads or draws. Its buffers and scratch textures serve its later recordings.
        struct Slot {
            enum class State : std::uint8_t {
                Idle,
                Recorded,
                Submitted
            };

            Slot() = default;
            Slot(const Slot&) = delete;
            Slot& operator=(const Slot&) = delete;

            State state{};
            // Numbers the slot's recording among the renderer's.
            std::uint64_t recording{};
            std::vector<Buffer> buffers;
            std::vector<Image> images;
            std::vector<std::shared_ptr<const void>> leases;
            std::vector<std::uint64_t> created;
            // What the slot's earlier recordings used, for its next ones.
            std::vector<Buffer> spare_buffers;
            std::vector<Image> spare_images;
            // The serial of the kept pass whose records and rounded clips kept_records and kept_clips hold (0: none).
            std::uint64_t kept{};
            Buffer kept_records;
            Buffer kept_clips;
        };

        // bounds are the preparer's image caches' and atlases' (SceneCacheBounds). Compiles the shaders' MSL on device.
        SceneRenderer(id<MTLDevice> device, SceneText& text, SceneCacheBounds bounds = {});
        ~SceneRenderer();
        SceneRenderer(const SceneRenderer&) = delete;
        SceneRenderer& operator=(const SceneRenderer&) = delete;

        // Encodes frame's pass over target (scale device px per CSS px, clipped to crop when given, layers drawn as cached
        // layer images, anchors placing the anchored elements as ScenePreparer::prepare says) into command, not
        // committed and with no encoder open, with what it uses in slot: an idle slot, or one whose submitted commands
        // completed (it retires first). False, with the reason counted and nothing encoded, when a native pass cannot
        // draw the frame. Throws Busy, encoding nothing, while the frame samples what another recording uploads or draws
        // and that is not submitted yet. start: whether the pass replaces what the target holds or draws over it.
        // textures: the host textures of the page's -weft-texture(name) backgrounds, their views id<MTLTexture>
        // pointers; an unbound name draws nothing.
        bool encode(Slot& slot, const SceneFrame& frame, id<MTLCommandBuffer> command, const Target& target,
                    float scale, std::optional<std::array<float, 4>> crop, unsigned view_index,
                    const std::vector<NodeIdentity>& layers, const SceneAnchorTable* anchors = nullptr,
                    const SceneTargetStart& start = {}, std::span<const SceneTexture> textures = {});
        // The slot's recorded commands were committed: what they upload or draw is no longer pending.
        void submitted(Slot& slot);
        // The slot's recorded commands will never be committed: what they would upload or draw leaves the cache, and the
        // slot is idle again.
        void discard(Slot& slot) noexcept;
        // The slot's committed commands completed: its leases go, and the slot is idle again.
        void retire(Slot& slot) noexcept;

        // Draws frame on a transparent width x height device px target with encode() on the renderer's own queue,
        // waits, and returns the target's encoded premultiplied BGRA rows; none, with the reason counted, when a native
        // pass cannot draw the frame. Unless readable, encode() may not copy from the target.
        std::optional<std::vector<std::uint8_t>> draw(const SceneFrame& frame, float scale, unsigned width,
                                                      unsigned height, std::optional<std::array<float, 4>> crop,
                                                      unsigned view_index, const std::vector<NodeIdentity>& layers,
                                                      bool readable = true, const SceneAnchorTable* anchors = nullptr);

        id<MTLDevice> device;
        // Images uploaded, recordings made, and buffers and scratch textures made for slots.
        std::size_t uploads{};
        std::size_t recordings{};
        std::size_t slot_objects{};

        // Drops what the renderer and its preparer keep for a view.
        void forget(unsigned view_index) {
            kept_.erase(view_index);
            preparer_.forget(view_index);
        }

        std::size_t kept_views() const {
            return kept_.size();
        }

    private:
        // The texture of a preparer's image, layer or filtered image, kept while that lives (none for an image on an
        // atlas page, whose page holds it); pending while the recording that uploads or draws it is not submitted.
        struct Texture {
            std::weak_ptr<const void> source;
            Image image;
            std::uint64_t pending{};
        };

        ScenePreparer preparer_;
        std::string last_failure_;
        id<MTLCommandQueue> queue_;
        id<MTLSamplerState> clamp_sampler_;
        id<MTLSamplerState> repeat_sampler_;
        id<MTLSamplerState> mirror_sampler_;
        id<MTLFunction> scene_vertex_;
        id<MTLRenderPipelineState> scene_pipeline_;
        id<MTLRenderPipelineState> under_pipeline_;
        // A blending group's composite that reads the target where it draws (framebuffer fetch), on Apple GPUs; nil
        // elsewhere (blending groups copy their backdrops).
        id<MTLRenderPipelineState> blend_pipeline_;
        id<MTLRenderPipelineState> particle_pipeline_;
        // The area downsample, the Gaussian pass and the color matrix (render::BackdropFilter order).
        std::array<id<MTLRenderPipelineState>, 3> backdrop_pipelines_;
        // An effect program's encoded pass; the lease keeps the program alive, so its address names it alone.
        struct EffectPipeline {
            std::shared_ptr<const paint::EffectProgram> program;
            id<MTLRenderPipelineState> pipeline;
        };
        std::unordered_map<const paint::EffectProgram*, EffectPipeline> effect_pipelines_;
        // Bound where a draw samples no image or a pass has no rounded clips or anchors.
        id<MTLTexture> blank_texture_;
        id<MTLBuffer> blank_buffer_;
        bool float_filtering_{};
        std::unordered_map<std::uint64_t, Texture> textures_;
        // The preparer's atlas pages by format and page number.
        std::map<std::array<std::uint32_t, 2>, Image> pages_;
        std::uint64_t next_recording_{};
        // draw()'s target and readback of the last frame's size, and its slot.
        Image target_;
        id<MTLBuffer> readback_;
        Slot draw_slot_;
        // The command buffer and slot of the recording in progress.
        id<MTLCommandBuffer> command_buffer_;
        Slot* slot_{};

        // The pass kept for a view, as the Vulkan renderer keeps it.
        struct Kept {
            std::uint64_t revision{};
            float scale{};
            std::uint32_t width{};
            std::uint32_t height{};
            std::vector<NodeIdentity> layers;
            std::shared_ptr<ScenePrepared> prepared;
            std::uint64_t serial{};
            SceneAnchorFrame frame;
            std::vector<bool> flips;
            ScenePatchHistory history;
        };

        std::unordered_map<unsigned, Kept> kept_;
        std::uint64_t serial_{};
        std::vector<std::uint32_t> in_order_;

        id<MTLRenderPipelineState> pipeline(std::string_view vertex, std::string_view fragment, bool under = false);
        id<MTLFunction> function(std::string_view source, const char* name,
                                 MTLLanguageVersion version = MTLLanguageVersion2_1);
        void check_pending(const ScenePrepared& prepared) const;
        void upload(const ScenePrepared& prepared);
        // Encodes prepared's pass into target (a texture of prepared's size or larger; readable when the pass may copy
        // from it): its planes' and new layers' contents first, each into a texture of its own. As Vulkan's record().
        void record(const ScenePrepared& prepared, id<MTLTexture> target, bool readable, float scale,
                    const SceneAnchorFrame* anchors = nullptr, std::uint64_t serial = 0,
                    const paint::Color* clear = nullptr, std::span<const SceneTexture> host_textures = {},
                    const ScenePatchHistory* history = nullptr);
        const Image& page(const SceneAtlasPage& page);
        id<MTLTexture> layer(const std::shared_ptr<const SceneLayerImage>& layer, float scale);
        id<MTLTexture> filter(const std::shared_ptr<const SceneFilterImage>& filter);
        id<MTLRenderPipelineState> effect_pipeline(const std::shared_ptr<const paint::EffectProgram>& program);
        // A color target texture of the targets' format: the slot's (a spare of that size, or new) unless kept.
        Image target(std::uint32_t width, std::uint32_t height, bool kept = false);
        // A shared buffer of the slot's of at least size bytes: a spare, or new with room.
        Buffer& buffer(std::size_t size);
        Buffer new_buffer(std::size_t size);
        // A render command encoder over all of texture's width x height (or the pass's), cleared to clear or loading.
        id<MTLRenderCommandEncoder> begin(id<MTLTexture> texture, std::uint32_t width, std::uint32_t height,
                                          const std::optional<paint::Color>& clear);
        void release(Slot& slot) noexcept;
    };
} // namespace weft::render::metal
