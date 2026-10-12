#pragma once

#include "render/backends/vulkan/device.hpp"
#include "render/backends/vulkan/pipeline.hpp"
#include "render/backends/vulkan/renderer.hpp"
#include "scene/scene.hpp"
#include "scene/scene_native.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

// Native Vulkan pass of a scene: the passes a ScenePreparer prepares (scene/scene_native.hpp), drawn by the shared
// scene shader's encoded pass (src/render/shaders/scene.hlsl without SPACE_LINEAR, as SPIR-V) as instanced box records,
// transparent groups in scratch images, perspective planes and explicit layers whose content a nested pass draws into
// an image of its own, and the text, shadow and path images the preparer builds, uploaded once each. Every target holds
// encoded premultiplied BGRA (B8G8R8A8_UNORM), as the D3D11 scene targets do.
//
// encode() records a frame into the host's command buffer with what it uses in a slot: the host submits the commands,
// reports the submission (submitted()), and retires the slot once they completed, or discards a slot whose commands it
// never submits. Slots lease the images and layers they sample, so an image of the cache goes only after every frame
// that sampled it completed. What a recording uploads or draws into the cache is pending until its submission: another
// recording that samples it throws Busy. Recordings are submitted in the order they were recorded. draw() is that
// sequence on the renderer's own command buffer, waiting for the frame and reading its target back.
//
// Owner thread of the SceneText and of the device's queue.
namespace weft::render::vulkan {
    class SceneRenderer : public SceneNativeWork {
    public:
        // A host color target: a view of a single-sample B8G8R8A8_UNORM image of width x height device px with one
        // mip level and layer, a color attachment. The pass replaces its contents and leaves it in
        // VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL.
        struct Target {
            VkImageView view{};
            std::uint32_t width{};
            std::uint32_t height{};
        };

        // What one recorded frame uses: its buffers, scratch images, descriptor pools and target framebuffer, leases
        // of the images and layers it samples, and the cache entries it uploads or draws.
        struct Slot {
            enum class State : std::uint8_t {
                Idle,
                Recorded,
                Submitted
            };

            State state{};
            // Numbers the slot's recording among the renderer's.
            std::uint64_t recording{};
            std::vector<Buffer> buffers;
            std::vector<Image> images;
            std::vector<VkDescriptorPool> pools;
            VkFramebuffer framebuffer{};
            std::vector<std::shared_ptr<const void>> leases;
            std::vector<std::uint64_t> created;
        };

        SceneRenderer(Device& device, SceneText& text);
        ~SceneRenderer();
        SceneRenderer(const SceneRenderer&) = delete;
        SceneRenderer& operator=(const SceneRenderer&) = delete;

        // Records frame's pass over target (scale device px per CSS px, clipped to crop when given, layers drawn as
        // cached layer images) into command, a primary command buffer recording outside a render pass, with what it
        // uses in slot: an idle slot, or one whose submitted commands completed (it retires first). False, with the
        // reason counted and nothing recorded, when a native pass cannot draw the frame. Throws Busy, recording
        // nothing, while the frame samples what another recording uploads or draws and that is not submitted yet.
        bool encode(Slot& slot, const SceneFrame& frame, VkCommandBuffer command, const Target& target, float scale,
                    std::optional<std::array<float, 4>> crop, unsigned view_index,
                    const std::vector<NodeIdentity>& layers);
        // The slot's recorded commands were submitted: what they upload or draw is no longer pending.
        void submitted(Slot& slot);
        // The slot's recorded commands will never be submitted: what they would upload or draw leaves the cache, and the
        // slot is idle again.
        void discard(Slot& slot) noexcept;
        // The slot's submitted commands completed: its resources and leases go, and the slot is idle again.
        void retire(Slot& slot) noexcept;

        // Draws frame on a transparent width x height device px target with encode() on the renderer's own command
        // buffer, waits, and returns the target's encoded premultiplied BGRA rows; none, with the reason counted, when a
        // native pass cannot draw the frame.
        std::optional<std::vector<std::uint8_t>> draw(const SceneFrame& frame, float scale, unsigned width,
                                                      unsigned height, std::optional<std::array<float, 4>> crop,
                                                      unsigned view_index, const std::vector<NodeIdentity>& layers);

        Device& gpu;
        // Images uploaded, and recordings made.
        std::size_t uploads{};
        std::size_t recordings{};

    private:
        // The image of a preparer's image or layer, kept while that lives; pending while the recording that uploads or
        // draws it is not submitted.
        struct Texture {
            std::weak_ptr<const void> source;
            Image image;
            std::uint64_t pending{};
        };

        ScenePreparer preparer_;
        VkCommandPool pool_{};
        VkCommandBuffer command_{};
        VkFence fence_{};
        VkSampler sampler_{};
        VkShaderModule vertex_{};
        VkShaderModule fragment_{};
        VkDescriptorSetLayout set_layout_{};
        VkPipelineLayout layout_{};
        ScenePasses passes_;
        // Bound where a draw samples no image or a pass has no rounded clips.
        Image blank_image_;
        Buffer blank_buffer_;
        std::unordered_map<std::uint64_t, Texture> textures_;
        std::uint64_t next_recording_{};
        // draw()'s target and its copy, of the last frame's size.
        Image target_;
        Buffer readback_;
        // The command buffer and slot of the recording in progress.
        VkCommandBuffer command_buffer_{};
        Slot* slot_{};

        // Throws Busy when prepared samples, from the cache, what another unsubmitted recording uploads or draws.
        void check_pending(const ScenePrepared& prepared) const;
        // Records the uploads of the images that prepared samples and that have no image yet, its layers' contents
        // included when those are drawn here.
        void upload(const ScenePrepared& prepared);
        // Records prepared's pass into the framebuffer of an image of prepared's size: its planes' and new layers'
        // contents first, each into an image of its own.
        void record(const ScenePrepared& prepared, VkFramebuffer framebuffer, float scale);
        // The image of a layer, its content recorded the first time.
        VkImageView layer(const std::shared_ptr<const SceneLayerImage>& layer, float scale);
        // A color target image of the targets' format, sampled once drawn, with a framebuffer; the slot's when kept is
        // false.
        Image target(std::uint32_t width, std::uint32_t height, bool kept = false);
        VkFramebuffer framebuffer(VkImageView view, std::uint32_t width, std::uint32_t height);
        // Releases the slot's resources and leases.
        void release(Slot& slot) noexcept;
        // Destroys every device object the renderer holds.
        void destroy_all() noexcept;
    };
} // namespace weft::render::vulkan
