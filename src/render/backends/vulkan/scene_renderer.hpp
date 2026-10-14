#pragma once

#include "render/backends/vulkan/device.hpp"
#include "render/backends/vulkan/pipeline.hpp"
#include "render/backends/vulkan/renderer.hpp"
#include "scene/scene.hpp"
#include "scene/scene_native.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

// Native Vulkan pass of a scene: the passes a ScenePreparer prepares (scene/scene_native.hpp), drawn by the shared
// scene shader's encoded pass (src/render/shaders/scene.hlsl without SPACE_LINEAR, as SPIR-V) as instanced box records,
// transparent groups in scratch images, perspective planes and explicit layers whose content a nested pass draws into
// an image of its own, the text, shadow and path images the preparer builds, uploaded once each (small ones into the
// region the preparer gives them on an atlas page), and UI shader effects in their encoded pass with their textures. Every target holds encoded premultiplied BGRA (B8G8R8A8_UNORM), as the
// D3D11 scene targets do. A device that cannot filter linear float images draws no effect with a translucent color
// texture: the frame falls back.
//
// encode() records a frame into the host's command buffer with what it uses in a slot: the host submits the commands,
// reports the submission (submitted()), and retires the slot once they completed, or discards a slot whose commands it
// never submits. Slots lease the images and layers they sample, so an image of the cache goes only after every frame
// that sampled it completed. What a recording uploads or draws into the cache is pending until its submission: another
// recording that samples it throws Busy. Recordings are submitted in the order they were recorded. draw() is that
// sequence on the renderer's own command buffer, waiting for the frame and reading its target back.
//
// A view's pass is kept while its frames keep their revision (SceneFrame::revision): such a frame records the kept pass
// again with the frame's anchors (ScenePreparer::place) and their record order, its records in the slot's own buffers
// since the slot first drew that pass.
//
// Owner thread of the SceneText and of the device's queue.
namespace weft::render::vulkan {
    class SceneRenderer : public SceneNativeWork {
    public:
        // A host color target: a view of a single-sample B8G8R8A8_UNORM image of width x height device px with one
        // mip level and layer, a color attachment. The pass replaces its contents and leaves it in
        // VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL. image is the view's image when the pass may read it (its usage
        // includes VK_IMAGE_USAGE_TRANSFER_SRC_BIT): backdrops at the target's level read what the pass drew there;
        // without it they are transparent, a counted reason (not a fallback).
        struct Target {
            VkImageView view{};
            std::uint32_t width{};
            std::uint32_t height{};
            VkImage image{};
        };

        // What one recorded frame uses: its buffers, scratch images, descriptor pools and target framebuffer, leases
        // of the images and layers it samples, and the cache entries it uploads or draws. Its buffers, scratch images
        // and pools serve its later recordings; the slot destroys them with itself, after its commands completed.
        struct Slot {
            enum class State : std::uint8_t {
                Idle,
                Recorded,
                Submitted
            };

            Slot() = default;
            Slot(const Slot&) = delete;
            Slot& operator=(const Slot&) = delete;
            ~Slot();

            State state{};
            // Numbers the slot's recording among the renderer's.
            std::uint64_t recording{};
            std::vector<Buffer> buffers;
            std::vector<Image> images;
            // Descriptor pools and their capacity in sets.
            std::vector<std::pair<VkDescriptorPool, std::uint32_t>> pools;
            VkFramebuffer framebuffer{};
            std::vector<std::shared_ptr<const void>> leases;
            std::vector<std::uint64_t> created;
            // What the slot's earlier recordings used, for its next ones.
            std::vector<Buffer> spare_buffers;
            std::vector<Image> spare_images;
            std::vector<std::pair<VkDescriptorPool, std::uint32_t>> spare_pools;
            // The device of the slot's objects, once it recorded.
            const Device* device{};
            // The serial of the kept pass whose records and rounded clips kept_records and kept_clips hold (0: none).
            std::uint64_t kept{};
            Buffer kept_records;
            Buffer kept_clips;
        };

        // bounds are the preparer's image caches' and atlases' (SceneCacheBounds).
        SceneRenderer(Device& device, SceneText& text, SceneCacheBounds bounds = {});
        ~SceneRenderer();
        SceneRenderer(const SceneRenderer&) = delete;
        SceneRenderer& operator=(const SceneRenderer&) = delete;

        // Records frame's pass over target (scale device px per CSS px, clipped to crop when given, layers drawn as
        // cached layer images, anchors placing the anchored elements as ScenePreparer::prepare says) into command, a primary command buffer recording outside a render pass, with what it
        // uses in slot: an idle slot, or one whose submitted commands completed (it retires first). False, with the
        // reason counted and nothing recorded, when a native pass cannot draw the frame. Throws Busy, recording
        // nothing, while the frame samples what another recording uploads or draws and that is not submitted yet.
        bool encode(Slot& slot, const SceneFrame& frame, VkCommandBuffer command, const Target& target, float scale,
                    std::optional<std::array<float, 4>> crop, unsigned view_index,
                    const std::vector<NodeIdentity>& layers, const SceneAnchorTable* anchors = nullptr);
        // The slot's recorded commands were submitted: what they upload or draw is no longer pending.
        void submitted(Slot& slot);
        // The slot's recorded commands will never be submitted: what they would upload or draw leaves the cache, and the
        // slot is idle again.
        void discard(Slot& slot) noexcept;
        // The slot's submitted commands completed: its resources and leases go, and the slot is idle again.
        void retire(Slot& slot) noexcept;

        // Draws frame on a transparent width x height device px target with encode() on the renderer's own command
        // buffer, waits, and returns the target's encoded premultiplied BGRA rows; none, with the reason counted, when a
        // native pass cannot draw the frame. Unless readable, encode() gets no image of the target (Target::image).
        std::optional<std::vector<std::uint8_t>> draw(const SceneFrame& frame, float scale, unsigned width,
                                                      unsigned height, std::optional<std::array<float, 4>> crop,
                                                      unsigned view_index, const std::vector<NodeIdentity>& layers,
                                                      bool readable = true, const SceneAnchorTable* anchors = nullptr);

        Device& gpu;
        // Images uploaded, recordings made, and buffers, scratch images and descriptor pools made for slots (which
        // their later recordings reuse).
        std::size_t uploads{};
        std::size_t recordings{};
        std::size_t slot_objects{};

    private:
        // The image of a preparer's image or layer, kept while that lives (none for an image on an atlas page, whose
        // page holds it); pending while the recording that uploads or draws it is not submitted.
        struct Texture {
            std::weak_ptr<const void> source;
            Image image;
            std::uint64_t pending{};
        };

        // An effect program's encoded pass: its fragment stage and pipeline. The lease keeps the program alive, so its
        // address names it alone.
        struct EffectPipeline {
            std::shared_ptr<const paint::EffectProgram> program;
            VkShaderModule fragment{};
            VkPipeline pipeline{};
        };

        ScenePreparer preparer_;
        VkCommandPool pool_{};
        VkCommandBuffer command_{};
        VkFence fence_{};
        VkSampler sampler_{};
        // Repeats the textures effects sample (scene.hlsl, effectRepeat).
        VkSampler effect_sampler_{};
        std::unordered_map<const paint::EffectProgram*, EffectPipeline> effect_pipelines_;
        VkShaderModule vertex_{};
        VkShaderModule fragment_{};
        // The particle pass: both stages of scene.hlsl's WEFT_PARTICLES variant, and its pipeline.
        VkShaderModule particle_vertex_{};
        VkShaderModule particle_fragment_{};
        VkPipeline particle_pipeline_{};
        // The backdrop passes (backdrop.hlsl): their stages, set layout, pipeline layout and pipelines, the mirroring
        // sampler, and the scene pipeline that draws a backdrop beneath its group's content.
        VkShaderModule backdrop_vertex_{};
        std::array<VkShaderModule, 3> backdrop_fragments_{};
        VkDescriptorSetLayout backdrop_set_layout_{};
        VkPipelineLayout backdrop_layout_{};
        // The area downsample, the Gaussian pass and the color matrix.
        std::array<VkPipeline, 3> backdrop_pipelines_{};
        VkSampler mirror_sampler_{};
        // The distance between backdrop passes' constants in the buffer one recording gives them all: their size
        // rounded up to the device's uniform buffer offset alignment.
        VkDeviceSize backdrop_stride_{};
        VkPipeline under_pipeline_{};
        VkDescriptorSetLayout set_layout_{};
        VkPipelineLayout layout_{};
        ScenePasses passes_;
        // Bound where a draw samples no image or a pass has no rounded clips.
        Image blank_image_;
        Buffer blank_buffer_;
        std::unordered_map<std::uint64_t, Texture> textures_;
        // The preparer's atlas pages by format and page number, in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL outside the
        // copies of a recording that uploads images into them.
        std::map<std::array<std::uint32_t, 2>, Image> pages_;
        // Makes a new page shader-readable, once, before any recording uses it.
        VkCommandBuffer setup_{};
        VkFence setup_fence_{};
        std::uint64_t next_recording_{};
        // draw()'s target and its copy, of the last frame's size, and its slot.
        Image target_;
        Buffer readback_;
        Slot draw_slot_;
        // The command buffer and slot of the recording in progress.
        VkCommandBuffer command_buffer_{};
        Slot* slot_{};

        // The pass kept for a view: what its frames' revision and the pass's target were, the pass, its serial, and the
        // anchors of the frame recorded last.
        struct Kept {
            std::uint64_t revision{};
            float scale{};
            std::uint32_t width{};
            std::uint32_t height{};
            std::vector<NodeIdentity> layers;
            std::shared_ptr<const ScenePrepared> prepared;
            std::uint64_t serial{};
            SceneAnchorFrame frame;
        };

        std::unordered_map<unsigned, Kept> kept_;
        std::uint64_t serial_{};
        // Every record in its own place, for passes recorded in planned order (grown to the largest pass).
        std::vector<std::uint32_t> in_order_;

        // Throws Busy when prepared samples, from the cache, what another unsubmitted recording uploads or draws.
        void check_pending(const ScenePrepared& prepared) const;
        // Records the uploads of the images that prepared samples and that have no image yet, its layers' contents
        // included when those are drawn here.
        void upload(const ScenePrepared& prepared);
        // Records prepared's pass into the framebuffer of an image of prepared's size (image, when the pass may read
        // it): its planes' and new layers' contents first, each into an image of its own. anchors are the frame's
        // (none for a pass whose anchors are not deferred); serial names a kept pass, whose records the slot keeps.
        void record(const ScenePrepared& prepared, VkFramebuffer framebuffer, VkImage image, float scale,
                    const SceneAnchorFrame* anchors = nullptr, std::uint64_t serial = 0);
        // An atlas page's image, created empty and shader-readable the first time.
        const Image& page(const SceneAtlasPage& page);
        // The image of a layer, its content recorded the first time.
        VkImageView layer(const std::shared_ptr<const SceneLayerImage>& layer, float scale);
        // The pipeline of an effect program's encoded pass (its spirv_encoded), created on first use.
        VkPipeline effect_pipeline(const std::shared_ptr<const paint::EffectProgram>& program);
        // A color target image of the targets' format, sampled once drawn, with a framebuffer: the slot's (a spare of
        // that size, or new) unless kept, which is the cache's.
        Image target(std::uint32_t width, std::uint32_t height, bool kept = false);
        // A mapped buffer of the slot's of at least size bytes, for any of a pass's uses: a spare, or new with room.
        Buffer& buffer(VkDeviceSize size);
        // A descriptor pool of the slot's for at least sets sets: a spare, reset, or new with room.
        VkDescriptorPool pool(std::uint32_t sets);
        VkFramebuffer framebuffer(VkImageView view, std::uint32_t width, std::uint32_t height);
        // Ends the slot's recording: its buffers, scratch images and pools become spares (as many as the slot keeps),
        // its target framebuffer, leases and cache entries go.
        void release(Slot& slot) noexcept;
        // Destroys every device object the renderer holds.
        void destroy_all() noexcept;
    };
} // namespace weft::render::vulkan
