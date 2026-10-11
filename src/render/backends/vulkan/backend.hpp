#pragma once

#include "constants/render.hpp"
#include "render/backend.hpp"
#include "render/backends/vulkan/renderer.hpp"
#include "weft/render_backend.h"
#include "weft/render_backend_vulkan.h"

#include <array>
#include <cstdint>
#include <optional>
#include <type_traits>

// Vulkan adapter of the renderer C ABI (weft_render_vulkan_pass). The core ABI has no Vulkan dependency: the host
// records into its own command buffers, submits them on the device's queue and reports their submission and completion
// serials; nothing here submits or waits. Display list frames and quad frames (as their display lists) draw; external
// textures are not supported yet.
namespace weft::render::vulkan {
    // A Vulkan handle as the payload carries it, and back (render_backend_vulkan.h).
    template <class Handle>
    std::uint64_t abi_handle(Handle handle) {
        if constexpr (std::is_pointer_v<Handle>) {
            return std::uint64_t(reinterpret_cast<std::uintptr_t>(handle));
        } else {
            return std::uint64_t(handle);
        }
    }

    template <class Handle>
    Handle native_handle(std::uint64_t value) {
        if constexpr (std::is_pointer_v<Handle>) {
            return reinterpret_cast<Handle>(std::uintptr_t(value));
        } else {
            return Handle(value);
        }
    }

    // The Vulkan format of a target format the adapter draws, or VK_FORMAT_UNDEFINED.
    VkFormat target_format(std::uint32_t format);

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

    private:
        // Prepares make()'s frame; a failure leaves no lease behind.
        template <class Make>
        weft_render_result prepare_with(std::uint64_t& out, Make&& make);
    };

    static_assert(DisplayListBackend<Backend>);
} // namespace weft::render::vulkan
