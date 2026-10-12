#include "render/backends/vulkan/swapchain.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <vulkan/vulkan_win32.h>

namespace weft::render::vulkan {
    VkSurfaceKHR win32_surface(const Device& device, void* instance, void* window) {
        ensure(device.presents, "a device opened for presentation");
        const auto create = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
            device.get_instance_proc(device.instance, "vkCreateWin32SurfaceKHR"));
        ensure(create != nullptr, "vkCreateWin32SurfaceKHR");
        VkWin32SurfaceCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        info.hinstance = static_cast<HINSTANCE>(instance);
        info.hwnd = static_cast<HWND>(window);
        VkSurfaceKHR surface{};
        check(create(device.instance, &info, nullptr, &surface), "vkCreateWin32SurfaceKHR");
        return surface;
    }
} // namespace weft::render::vulkan
