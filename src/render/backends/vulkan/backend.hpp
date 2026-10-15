#pragma once

#include "constants/render.hpp"
#include "render/backend.hpp"
#include "render/backends/vulkan/renderer.hpp"
#include "weft/render_backend.h"
#include "weft/render_backend_vulkan.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// Vulkan adapter of the renderer C ABI (weft_render_vulkan_pass). The core ABI has no Vulkan dependency: the host
// records into its own command buffers, submits them on the device's queue and reports their submission and completion
// serials; nothing here submits or waits. Display list frames and quad frames (as their display lists) draw, with host
// textures once enable_external() installed the external texture extension (weft_render_vulkan_texture). Encode returns
// WEFT_RENDER_BUSY for a frame whose new atlas page another unsubmitted recording first writes (Busy in renderer.hpp).
namespace weft::render::vulkan {
    // The Vulkan format of a target format the adapter draws, or VK_FORMAT_UNDEFINED.
    VkFormat target_format(std::uint32_t format);

    // A host texture of the external texture extension on device, checked as its Vulkan schema says
    // (weft_render_vulkan_texture of device, either version): its view, and its image when the host gave it.
    weft_render_result check_external_texture(const weft_render_external_texture& description, VkDevice device,
                                              VkImageView& view, VkImage& image);

    class Backend {
    public:
        Renderer& gpu;
        weft_renderer* renderer{};
        std::optional<PreparedFrame> prepared;
        std::array<std::optional<Encoded>, weft::constants::render::vulkan::frames_in_flight> uses;

        explicit Backend(Renderer& device);
        ~Backend();
        Backend(const Backend&) = delete;
        Backend& operator=(const Backend&) = delete;

        // The target formats (bit 1 << weft_render_target_format) this device draws: sRGB RGBA8 and BGRA8 and linear
        // RGBA16F, each where it blends.
        std::uint32_t target_formats() const;
        // The formats of host textures: linear RGBA8 and RGBA16F, and RGBA32F where the device filters it.
        std::uint32_t external_formats() const;
        // Installs the external texture extension for at most capacity host textures of at most bytes in all.
        void enable_external(unsigned capacity, std::uint64_t bytes);
        // A pass over target_view, an image of the renderer's size in format, recorded into command; clears the target
        // to transparent.
        weft_render_pass pass(VkCommandBuffer command, VkImageView target_view, weft_render_target_format format,
                              weft_render_vulkan_pass& payload) const;

        // RenderBackend: called by the renderer core through the make_c_backend table.
        weft_render_result prepare(const weft_render_frame& frame, std::uint64_t& out);
        void release_prepared(std::uint64_t native);
        weft_render_result encode(std::uint64_t native, const weft_render_pass& render_pass, std::uint64_t& out);
        void discard_encoded(std::uint64_t native);
        void submitted(std::uint64_t native, std::uint64_t serial);
        void completed(std::uint64_t serial);
        void abandon();
        weft_render_result shutdown();

        // DisplayListBackend.
        weft_render_result prepare_list(const weft_paint_list& list, const weft_render_external_asset* textures,
                                        std::uint32_t count, std::uint64_t& out);

        // ExternalTextureBackend.
        weft_render_result import_texture(const weft_render_external_texture& description, std::uint64_t& out);
        void release_texture(std::uint64_t native);
        weft_render_result prepare_external(const weft_render_frame& frame, const weft_render_external_asset* textures,
                                            std::uint32_t count, std::uint64_t& out);

        // Host textures imported and released.
        std::size_t imported{};
        std::size_t released{};

    private:
        // The views of host textures, by import token - 1; empty slots are free.
        std::vector<VkImageView> imports_;

        // The views of a frame's host textures; STALE_HANDLE for a token without an import.
        weft_render_result external_views(const weft_render_external_asset* textures, std::uint32_t count,
                                          std::vector<ExternalView>& out) const;
        // Prepares make()'s frame; a failure leaves no lease behind.
        template <class Make>
        weft_render_result prepare_with(std::uint64_t& out, Make&& make);
    };

    static_assert(DisplayListBackend<Backend>);
    static_assert(ExternalTextureBackend<Backend>);
} // namespace weft::render::vulkan
