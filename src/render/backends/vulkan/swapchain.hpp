#pragma once

#include "render/backends/vulkan/device.hpp"

#include <cstdint>
#include <optional>
#include <vector>

// Presentation on a surface that the platform created (a Win32 window, an Android native window): the images of a
// swapchain on the device's graphics queue, which presents to the surface (Device opened with presentation). Images
// are sRGB BGRA8 or RGBA8 and take transfer writes and color attachment writes; FIFO presentation with vertical sync,
// otherwise mailbox, else immediate. Owner thread of the device.
namespace weft::render::vulkan {
#ifdef _WIN32
    // A surface for the Win32 window (HWND) of a module instance (HINSTANCE), for a Swapchain.
    VkSurfaceKHR win32_surface(const Device& device, void* instance, void* window);
#endif

    class Swapchain {
    public:
        // Takes the surface, which the swapchain destroys.
        Swapchain(Device& device, VkSurfaceKHR surface, bool vsync);
        ~Swapchain();
        Swapchain(const Swapchain&) = delete;
        Swapchain& operator=(const Swapchain&) = delete;

        // Creates the images again for the surface's size, or width x height where the surface leaves the size to the
        // swapchain. The device must have finished every frame that used the old images. A minimized window has no
        // size: extent is then empty and nothing can be acquired.
        void resize(std::uint32_t width, std::uint32_t height);
        // The image to draw next, once acquired signals; nothing when the swapchain has to be resized first.
        std::optional<std::uint32_t> acquire(VkSemaphore acquired);
        // Queues image for presentation once rendered signals; false when the swapchain has to be resized.
        bool present(std::uint32_t image, VkSemaphore rendered);

        Device& gpu;
        VkSurfaceKHR surface{};
        VkSwapchainKHR swapchain{};
        VkFormat format{};
        VkExtent2D extent{};
        VkPresentModeKHR mode{};
        std::vector<VkImage> images;
        bool vsync{};
        // The surface reported that presentation no longer matches it (out of date or suboptimal).
        bool stale{};
    };
} // namespace weft::render::vulkan
