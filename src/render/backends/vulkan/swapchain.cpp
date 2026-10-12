#include "render/backends/vulkan/swapchain.hpp"

#include "constants/render.hpp"

#include <algorithm>
#include <limits>

namespace weft::render::vulkan {
    namespace {
        namespace limits = weft::constants::render::vulkan;
    } // namespace

    Swapchain::Swapchain(Device& device, VkSurfaceKHR s, bool sync) : gpu(device), surface(s), vsync(sync) {
        const auto& d = gpu;
        try {
            ensure(d.presents, "a device opened for presentation");
            VkBool32 supported{};
            check(d.vki.vkGetPhysicalDeviceSurfaceSupportKHR(d.physical, d.queue_family, surface, &supported),
                  "vkGetPhysicalDeviceSurfaceSupportKHR");
            ensure(supported == VK_TRUE, "the graphics queue presents to the surface");
            std::uint32_t count{};
            check(d.vki.vkGetPhysicalDeviceSurfaceFormatsKHR(d.physical, surface, &count, nullptr),
                  "vkGetPhysicalDeviceSurfaceFormatsKHR");
            std::vector<VkSurfaceFormatKHR> formats(count);
            check(d.vki.vkGetPhysicalDeviceSurfaceFormatsKHR(d.physical, surface, &count, formats.data()),
                  "vkGetPhysicalDeviceSurfaceFormatsKHR");
            for (const auto wanted : {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB}) {
                for (const auto& f : formats) {
                    if (format == VK_FORMAT_UNDEFINED && f.format == wanted &&
                        f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                        format = wanted;
                    }
                }
            }
            ensure(format != VK_FORMAT_UNDEFINED, "an sRGB BGRA8 or RGBA8 surface format");
            check(d.vki.vkGetPhysicalDeviceSurfacePresentModesKHR(d.physical, surface, &count, nullptr),
                  "vkGetPhysicalDeviceSurfacePresentModesKHR");
            std::vector<VkPresentModeKHR> modes(count);
            check(d.vki.vkGetPhysicalDeviceSurfacePresentModesKHR(d.physical, surface, &count, modes.data()),
                  "vkGetPhysicalDeviceSurfacePresentModesKHR");
            // Every implementation offers FIFO.
            mode = VK_PRESENT_MODE_FIFO_KHR;
            if (!vsync) {
                for (const auto wanted : {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR}) {
                    if (std::find(modes.begin(), modes.end(), wanted) != modes.end()) {
                        mode = wanted;
                    }
                }
            }
        } catch (...) {
            d.vki.vkDestroySurfaceKHR(d.instance, surface, nullptr);
            throw;
        }
    }

    Swapchain::~Swapchain() {
        const auto& d = gpu;
        d.vkd.vkDeviceWaitIdle(d.device);
        if (swapchain) {
            d.vkd.vkDestroySwapchainKHR(d.device, swapchain, nullptr);
        }
        d.vki.vkDestroySurfaceKHR(d.instance, surface, nullptr);
    }

    void Swapchain::resize(std::uint32_t width, std::uint32_t height) {
        const auto& d = gpu;
        VkSurfaceCapabilitiesKHR caps{};
        check(d.vki.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d.physical, surface, &caps),
              "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        extent = caps.currentExtent;
        if (extent.width == std::numeric_limits<std::uint32_t>::max()) {
            extent = {std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width),
                      std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height)};
        }
        stale = false;
        if (!extent.width || !extent.height) {
            if (swapchain) {
                d.vkd.vkDestroySwapchainKHR(d.device, swapchain, nullptr);
                swapchain = {};
            }
            images.clear();
            return;
        }
        constexpr VkImageUsageFlags usage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ensure((caps.supportedUsageFlags & usage) == usage, "swapchain images take transfer and color writes");
        VkSwapchainCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.surface = surface;
        info.minImageCount = caps.minImageCount + std::uint32_t(limits::swapchain_extra_images);
        if (caps.maxImageCount) {
            info.minImageCount = std::min(info.minImageCount, caps.maxImageCount);
        }
        info.imageFormat = format;
        info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        info.imageExtent = extent;
        info.imageArrayLayers = 1;
        info.imageUsage = usage;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = caps.currentTransform;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) {
            for (auto bit = 1u; bit; bit <<= 1) {
                if (caps.supportedCompositeAlpha & bit) {
                    info.compositeAlpha = VkCompositeAlphaFlagBitsKHR(bit);
                    break;
                }
            }
        }
        info.presentMode = mode;
        info.clipped = VK_TRUE;
        info.oldSwapchain = swapchain;
        VkSwapchainKHR created{};
        check(d.vkd.vkCreateSwapchainKHR(d.device, &info, nullptr, &created), "vkCreateSwapchainKHR");
        if (swapchain) {
            d.vkd.vkDestroySwapchainKHR(d.device, swapchain, nullptr);
        }
        swapchain = created;
        std::uint32_t count{};
        check(d.vkd.vkGetSwapchainImagesKHR(d.device, swapchain, &count, nullptr), "vkGetSwapchainImagesKHR");
        images.resize(count);
        check(d.vkd.vkGetSwapchainImagesKHR(d.device, swapchain, &count, images.data()), "vkGetSwapchainImagesKHR");
    }

    std::optional<std::uint32_t> Swapchain::acquire(VkSemaphore acquired) {
        if (!swapchain) {
            return std::nullopt;
        }
        std::uint32_t index{};
        const auto result = gpu.vkd.vkAcquireNextImageKHR(gpu.device, swapchain, limits::fence_timeout_ns, acquired,
                                                          VK_NULL_HANDLE, &index);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            stale = true;
            return std::nullopt;
        }
        // A suboptimal image is acquired and its semaphore signals: draw it, then resize.
        stale = result == VK_SUBOPTIMAL_KHR;
        if (!stale) {
            check(result, "vkAcquireNextImageKHR");
        }
        return index;
    }

    bool Swapchain::present(std::uint32_t image, VkSemaphore rendered) {
        VkPresentInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &rendered;
        info.swapchainCount = 1;
        info.pSwapchains = &swapchain;
        info.pImageIndices = &image;
        const auto result = gpu.vkd.vkQueuePresentKHR(gpu.queue, &info);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            stale = true;
            return false;
        }
        check(result, "vkQueuePresentKHR");
        return true;
    }
} // namespace weft::render::vulkan
