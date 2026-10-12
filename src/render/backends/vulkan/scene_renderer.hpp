#pragma once

#include "render/backends/vulkan/device.hpp"
#include "render/backends/vulkan/pipeline.hpp"
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
// draw() records a frame on the renderer's own command buffer, submits it and waits for it: when it returns the device
// no longer uses anything the frame used. Owner thread of the SceneText and of the device's queue.
namespace weft::render::vulkan {
    class SceneRenderer : public SceneNativeWork {
    public:
        SceneRenderer(Device& device, SceneText& text);
        ~SceneRenderer();
        SceneRenderer(const SceneRenderer&) = delete;
        SceneRenderer& operator=(const SceneRenderer&) = delete;

        // Draws frame on a transparent width x height device px target at scale device px per CSS px, clipped to crop
        // when given, layers drawn as cached layer images, and returns the target's encoded premultiplied BGRA rows
        // once the frame completed; none, with the reason counted, when a native pass cannot draw the frame.
        std::optional<std::vector<std::uint8_t>> draw(const SceneFrame& frame, float scale, unsigned width,
                                                      unsigned height, std::optional<std::array<float, 4>> crop,
                                                      unsigned view_index, const std::vector<NodeIdentity>& layers);

        Device& gpu;
        // Images uploaded, and frames submitted.
        std::size_t uploads{};
        std::size_t submissions{};

    private:
        // The image of a preparer's image or layer, kept while that lives.
        struct Texture {
            std::weak_ptr<const void> source;
            Image image;
        };

        // What one frame creates and destroys once it completed, and the ids of the images and layers it adds to the
        // cache, which a frame that fails takes out again.
        struct Frame {
            std::vector<Buffer> buffers;
            std::vector<Image> images;
            std::vector<VkDescriptorPool> pools;
            std::vector<std::uint64_t> created;
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
        // draw()'s target and its copy, of the last frame's size.
        Image target_;
        Buffer readback_;

        // Records the uploads of the images that prepared samples and that have no image yet, its layers' contents
        // included when those are drawn here.
        void upload(Frame& frame, const ScenePrepared& prepared);
        // Records prepared's pass into image (of prepared's size): its planes' and new layers' contents first, each into
        // an image of its own.
        void record(Frame& frame, const ScenePrepared& prepared, const Image& image, float scale);
        // The image of a layer, its content recorded the first time.
        VkImageView layer(Frame& frame, const std::shared_ptr<const SceneLayerImage>& layer, float scale);
        // A color target image of the targets' format, sampled once drawn, with a framebuffer.
        Image target(Frame& frame, std::uint32_t width, std::uint32_t height);
        void release(Frame& frame) noexcept;
        // Destroys every device object the renderer holds.
        void destroy_all() noexcept;
    };
} // namespace weft::render::vulkan
