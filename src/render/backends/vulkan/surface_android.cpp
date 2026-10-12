#include "render/backends/vulkan/swapchain.hpp"

#include <android/native_window.h>

#include <vulkan/vulkan_android.h>

namespace weft::render::vulkan {
    VkSurfaceKHR android_surface(const Device& device, ANativeWindow* window) {
        ensure(device.presents, "a device opened for presentation");
        const auto create = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(
            device.get_instance_proc(device.instance, "vkCreateAndroidSurfaceKHR"));
        ensure(create != nullptr, "vkCreateAndroidSurfaceKHR");
        VkAndroidSurfaceCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        info.window = window;
        VkSurfaceKHR surface{};
        check(create(device.instance, &info, nullptr, &surface), "vkCreateAndroidSurfaceKHR");
        return surface;
    }
} // namespace weft::render::vulkan
