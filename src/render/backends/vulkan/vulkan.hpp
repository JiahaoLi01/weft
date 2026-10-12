#pragma once

// The Vulkan API without prototypes: the backend opens the system's loader at run time and loads every function it
// calls by name, so it links no import library and runs where the loader comes with the driver.
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <string_view>
#include <type_traits>

// The instance, debug and device functions the backend calls. Each list expands F(name) once per function.
#define WEFT_VULKAN_INSTANCE_FUNCTIONS(F)                                                                              \
    F(vkDestroyInstance)                                                                                               \
    F(vkEnumeratePhysicalDevices)                                                                                      \
    F(vkGetPhysicalDeviceProperties)                                                                                   \
    F(vkGetPhysicalDeviceQueueFamilyProperties)                                                                        \
    F(vkGetPhysicalDeviceMemoryProperties)                                                                             \
    F(vkGetPhysicalDeviceFormatProperties)                                                                             \
    F(vkCreateDevice)                                                                                                  \
    F(vkEnumerateDeviceExtensionProperties)                                                                            \
    F(vkGetDeviceProcAddr)

#define WEFT_VULKAN_DEBUG_FUNCTIONS(F)                                                                                 \
    F(vkCreateDebugUtilsMessengerEXT)                                                                                  \
    F(vkDestroyDebugUtilsMessengerEXT)

// Surface functions of VK_KHR_surface and swapchain functions of VK_KHR_swapchain, for a device that presents.
#define WEFT_VULKAN_SURFACE_FUNCTIONS(F)                                                                               \
    F(vkDestroySurfaceKHR)                                                                                             \
    F(vkGetPhysicalDeviceSurfaceSupportKHR)                                                                            \
    F(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                                                                       \
    F(vkGetPhysicalDeviceSurfaceFormatsKHR)                                                                            \
    F(vkGetPhysicalDeviceSurfacePresentModesKHR)

#define WEFT_VULKAN_SWAPCHAIN_FUNCTIONS(F)                                                                             \
    F(vkCreateSwapchainKHR)                                                                                            \
    F(vkDestroySwapchainKHR)                                                                                           \
    F(vkGetSwapchainImagesKHR)                                                                                         \
    F(vkAcquireNextImageKHR)                                                                                           \
    F(vkQueuePresentKHR)

#define WEFT_VULKAN_DEVICE_FUNCTIONS(F)                                                                                \
    F(vkDestroyDevice)                                                                                                 \
    F(vkGetDeviceQueue)                                                                                                \
    F(vkDeviceWaitIdle)                                                                                                \
    F(vkQueueSubmit)                                                                                                   \
    F(vkCreateCommandPool)                                                                                             \
    F(vkDestroyCommandPool)                                                                                            \
    F(vkAllocateCommandBuffers)                                                                                        \
    F(vkFreeCommandBuffers)                                                                                            \
    F(vkResetCommandBuffer)                                                                                            \
    F(vkBeginCommandBuffer)                                                                                            \
    F(vkEndCommandBuffer)                                                                                              \
    F(vkCreateFence)                                                                                                   \
    F(vkDestroyFence)                                                                                                  \
    F(vkWaitForFences)                                                                                                 \
    F(vkGetFenceStatus)                                                                                                \
    F(vkCreateSemaphore)                                                                                               \
    F(vkDestroySemaphore)                                                                                              \
    F(vkCreateQueryPool)                                                                                               \
    F(vkDestroyQueryPool)                                                                                              \
    F(vkGetQueryPoolResults)                                                                                           \
    F(vkCmdResetQueryPool)                                                                                             \
    F(vkCmdWriteTimestamp)                                                                                             \
    F(vkCmdBlitImage)                                                                                                  \
    F(vkResetFences)                                                                                                   \
    F(vkCreateBuffer)                                                                                                  \
    F(vkDestroyBuffer)                                                                                                 \
    F(vkGetBufferMemoryRequirements)                                                                                   \
    F(vkBindBufferMemory)                                                                                              \
    F(vkCreateImage)                                                                                                   \
    F(vkDestroyImage)                                                                                                  \
    F(vkGetImageMemoryRequirements)                                                                                    \
    F(vkBindImageMemory)                                                                                               \
    F(vkAllocateMemory)                                                                                                \
    F(vkFreeMemory)                                                                                                    \
    F(vkMapMemory)                                                                                                     \
    F(vkUnmapMemory)                                                                                                   \
    F(vkCreateImageView)                                                                                               \
    F(vkDestroyImageView)                                                                                              \
    F(vkCreateSampler)                                                                                                 \
    F(vkDestroySampler)                                                                                                \
    F(vkCreateShaderModule)                                                                                            \
    F(vkDestroyShaderModule)                                                                                           \
    F(vkCreateDescriptorSetLayout)                                                                                     \
    F(vkDestroyDescriptorSetLayout)                                                                                    \
    F(vkCreatePipelineLayout)                                                                                          \
    F(vkDestroyPipelineLayout)                                                                                         \
    F(vkCreateDescriptorPool)                                                                                          \
    F(vkResetDescriptorPool)                                                                                           \
    F(vkDestroyDescriptorPool)                                                                                         \
    F(vkAllocateDescriptorSets)                                                                                        \
    F(vkUpdateDescriptorSets)                                                                                          \
    F(vkCreateRenderPass)                                                                                              \
    F(vkDestroyRenderPass)                                                                                             \
    F(vkCreateFramebuffer)                                                                                             \
    F(vkDestroyFramebuffer)                                                                                            \
    F(vkCreateGraphicsPipelines)                                                                                       \
    F(vkDestroyPipeline)                                                                                               \
    F(vkCmdPipelineBarrier)                                                                                            \
    F(vkCmdCopyBufferToImage)                                                                                          \
    F(vkCmdCopyImageToBuffer)                                                                                          \
    F(vkCmdClearColorImage)                                                                                            \
    F(vkCmdBeginRenderPass)                                                                                            \
    F(vkCmdEndRenderPass)                                                                                              \
    F(vkCmdBindPipeline)                                                                                               \
    F(vkCmdBindDescriptorSets)                                                                                         \
    F(vkCmdSetViewport)                                                                                                \
    F(vkCmdSetScissor)                                                                                                 \
    F(vkCmdDraw)

namespace weft::render::vulkan {
    // Throws std::runtime_error("<operation> VkResult=<n>") unless result is VK_SUCCESS.
    void check(VkResult result, const char* operation);
    // Throws std::runtime_error(message) unless condition holds.
    void ensure(bool condition, std::string_view message);

#define WEFT_VULKAN_MEMBER(name) PFN_##name name{};

    struct InstanceFunctions {
        WEFT_VULKAN_INSTANCE_FUNCTIONS(WEFT_VULKAN_MEMBER)
        // Present when the debug utils extension is enabled.
        WEFT_VULKAN_DEBUG_FUNCTIONS(WEFT_VULKAN_MEMBER)
        // Present when the device presents.
        WEFT_VULKAN_SURFACE_FUNCTIONS(WEFT_VULKAN_MEMBER)
    };

    struct DeviceFunctions {
        WEFT_VULKAN_DEVICE_FUNCTIONS(WEFT_VULKAN_MEMBER)
        // Present when the device presents.
        WEFT_VULKAN_SWAPCHAIN_FUNCTIONS(WEFT_VULKAN_MEMBER)
    };

#undef WEFT_VULKAN_MEMBER

    // A handle as a 64-bit value, as the C ABI payload carries it (render_backend_vulkan.h) and as the renderer names
    // textures, and back.
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
} // namespace weft::render::vulkan
